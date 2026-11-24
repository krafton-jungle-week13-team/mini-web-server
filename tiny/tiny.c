#include "csapp.h"
#include "http_parse.h"
#include "route.h"

static volatile sig_atomic_t stop = 0;

static void on_signal(int sig)
{
    (void)sig;
    stop = 1;              /* 핸들러에서는 플래그만 세운다 */
}

static route_table_t routes;

/* ===== 연결 상태 ===== */

enum { READ_REQLINE, READ_HEADERS, READ_BODY };   /* 요청을 어디까지 받았는지 */
enum { KEEP = 0, CLOSE = 1 };                     /* 단계 함수 반환값: 연결 유지 / 닫기 */

/* 연결 하나의 진행 상황 (상태 기계 하나) */
typedef struct {
    rio_t rio;                                    /* 읽다 남은 버퍼 */
    int state;
    char method[MAXLINE], uri[MAXLINE], version[MAXLINE];
    req_headers_t hdrs;
    char filename[MAXLINE], cgiargs[MAXLINE];
    int is_static, is_post;
    char body[MAXBODY + 1];
    long body_len, body_read;
    char hdr_raw[MAXBUF];          /* 백엔드로 넘길 헤더 원본 */
    size_t hdr_len;
    char client_ip[NI_MAXHOST];    /* X-Forwarded-For 용 */
    route_t *route;                /* 매칭된 규칙, 없으면 NULL */
} conn_t;

/* 활성 연결들의 pool */
typedef struct {
    int maxfd;                                    /* read_set에서 가장 큰 fd (select 범위) */
    fd_set read_set;                              /* 감시할 fd 목록 (원본) */
    fd_set ready_set;                             /* 이번 회차에 준비된 fd */
    int nready;                                   /* 이번 회차에 준비된 fd 개수 */
    int maxi;                                     /* clientfd[]에서 사용 중인 마지막 칸 */
    int clientfd[FD_SETSIZE];                     /* -1이면 빈칸 */
    conn_t conns[FD_SETSIZE];                     /* 칸마다 연결 상태 */
} pool;

/* ===== 함수 선언 ===== */

int save_header(conn_t *c, const char *line);
int is_hop_by_hop(const char *name, size_t len);
void finish_request(int fd, conn_t *c);
void proxy_stub(int fd, conn_t *c);

/* 이벤트 루프 / pool */
void init_pool(int listenfd, pool *p);
void add_client(int connfd, pool *p, const char *client_ip);
void check_clients(pool *p);
void command(void);

/* 연결별 상태 기계 */
int handle_client(int fd, conn_t *c);
int on_request_line(int fd, conn_t *c, char *buf);
int on_header_line(int fd, conn_t *c, char *buf);
int on_body(int fd, conn_t *c);
void respond(int fd, conn_t *c);
ssize_t rio_read_some(rio_t *rp, char *usrbuf, size_t n);

/* 응답 */
int parse_uri(char *uri, char *filename, char *cgiargs);
void serve_static(int fd, char *filename, int filesize);
void get_filetype(char *filename, char *filetype);
void serve_dynamic(int fd, char *filename, char *cgiargs,
                   char *method, char *body, long body_len);
void clienterror(int fd, char *cause, char *errnum,
                 char *shortmsg, char *longmsg, char *allow);

/* ===== 이벤트 루프 ===== */

int main(int argc, char **argv)
{
    int listenfd, connfd;
    socklen_t clientlen;
    struct sockaddr_storage clientaddr;
    char client_hostname[MAXLINE], client_port[MAXLINE];
    static pool pool;                             /* 크기가 커서 스택이 아닌 static에 둔다 */

    if (argc != 2 && !(argc == 4 && !strcmp(argv[2], "-c"))) {
        fprintf(stderr, "usage: %s <port> [-c routes.conf]\n", argv[0]);
        exit(1);
    }

    if (argc == 4) {
        if (route_load(argv[3], &routes) < 0)
            exit(1);                  /* 잘못된 설정이면 시작하지 않음 */
        route_print(&routes);
    }

    /* CGI가 body를 다 읽기 전에 끝나도 pipe 쓰기에서 서버가 죽지 않게 한다 */
    Signal(SIGPIPE, SIG_IGN);
    Signal(SIGINT, on_signal);
    Signal(SIGTERM, on_signal);

    listenfd = Open_listenfd(argv[1]);
    init_pool(listenfd, &pool);

    while (!stop) {
        pool.ready_set = pool.read_set;
        /* 준비된 fd 개수. add_client, check_clients에서 하나씩 차감 */
        pool.nready = select(pool.maxfd + 1, &pool.ready_set, NULL, NULL, NULL);
        if (pool.nready < 0) {
            if (errno == EINTR)
                continue;          /* 시그널로 깨어남 → stop 확인하러 감 */
            perror("select");
            break;
        }

        if (FD_ISSET(listenfd, &pool.ready_set)) {
            clientlen = sizeof(struct sockaddr_storage);
            connfd = Accept(listenfd, (SA *)&clientaddr, &clientlen);
            Getnameinfo((SA *)&clientaddr, clientlen, client_hostname, MAXLINE,
                        client_port, MAXLINE, NI_NUMERICHOST | NI_NUMERICSERV);
            printf("Accepted connection from (%s, %s)\n", client_hostname, client_port);
            add_client(connfd, &pool, client_hostname);
        }
        check_clients(&pool);
    }

    for (int i = 0; i <= pool.maxi; i++)
    if (pool.clientfd[i] >= 0)
        Close(pool.clientfd[i]);
    Close(listenfd);
    printf("server stopped\n");

    return 0;
}

/* pool 초기화: 처음에는 listenfd와 stdin만 감시 */
void init_pool(int listenfd, pool *p)
{
    int i;

    p->maxi = -1;                                 /* 아직 사용 중인 칸 없음 */
    for (i = 0; i < FD_SETSIZE; i++)
        p->clientfd[i] = -1;

    p->maxfd = listenfd;
    FD_ZERO(&p->read_set);
    FD_SET(listenfd, &p->read_set);
}

/* 새 연결을 빈칸에 등록. 처리는 다음 회차부터 */
void add_client(int connfd, pool *p, const char *client_ip)
{
    int i;

    p->nready--;
    for (i = 0; i < FD_SETSIZE; i++) {
        if (p->clientfd[i] < 0) {
            conn_t *c = &p->conns[i];

            p->clientfd[i] = connfd;
            memset(c, 0, sizeof(*c));             /* 이전 연결이 남긴 값 지우기 */
            snprintf(c->client_ip, sizeof(c->client_ip), "%s", client_ip);
            Rio_readinitb(&c->rio, connfd);
            c->state = READ_REQLINE;

            FD_SET(connfd, &p->read_set);
            if (connfd > p->maxfd)
                p->maxfd = connfd;
            if (i > p->maxi)
                p->maxi = i;
            return;
        }
    }
    Close(connfd);                                /* 빈칸 없음: 이 연결만 거절 */
}

/* 준비된 연결마다 상태 기계를 한 단계 이상 진행. 끝난 연결은 정리 */
void check_clients(pool *p)
{
    int i, connfd;

    for (i = 0; (i <= p->maxi) && (p->nready > 0); i++) {
        connfd = p->clientfd[i];
        if ((connfd >= 0) && FD_ISSET(connfd, &p->ready_set)) {
            p->nready--;
            if (handle_client(connfd, &p->conns[i]) == CLOSE) {
                Close(connfd);
                FD_CLR(connfd, &p->read_set);
                p->clientfd[i] = -1;
            }
        }
    }
}

/* ===== 연결별 상태 기계 ===== */

/* 지금 읽을 수 있는 만큼 진행. 요청이 덜 왔으면 KEEP, 끝났으면 CLOSE */
int handle_client(int fd, conn_t *c)
{
    char buf[MAXLINE];
    int ret;

    do {
        if (c->state == READ_BODY) {
            ret = on_body(fd, c);
        } else {
            if (rio_readlineb(&c->rio, buf, MAXLINE) <= 0)
                return CLOSE;                     /* EOF / 에러 */
            if (c->state == READ_REQLINE)
                ret = on_request_line(fd, c, buf);
            else
                ret = on_header_line(fd, c, buf);
        }
        if (ret == CLOSE)
            return CLOSE;
    } while (c->rio.rio_cnt > 0);                 /* rio 버퍼에 남은 데이터는 지금 처리 */

    return KEEP;                                  /* 덜 왔음 → select로 복귀 */
}

/* READ_REQLINE: 요청 줄 파싱 */
int on_request_line(int fd, conn_t *c, char *buf)
{
    printf("Request headers:\n%s", buf);
    sscanf(buf, "%s %s %s", c->method, c->uri, c->version);
    printf("method=[%s] uri=[%s] version=[%s]\n", c->method, c->uri, c->version);

    c->is_post = !strcasecmp(c->method, "POST");
    if (strcasecmp(c->method, "GET") && !c->is_post) {
        clienterror(fd, c->method, "501", "Not implemented",
                    "Tiny does not implement this method", NULL);
        return CLOSE;
    }
    c->state = READ_HEADERS;
    return KEEP;
}

/* READ_HEADERS: 헤더 한 줄 저장. 빈 줄이면 요청 검사 후 body 대기 또는 응답 */
int on_header_line(int fd, conn_t *c, char *buf)
{
    if (strcmp(buf, "\r\n") && strcmp(buf, "\n")) {
        if (save_header(c, buf) < 0 || parse_header_line(buf, &c->hdrs) < 0) {
                clienterror(fd, "header", "400", "Bad Request",
                        "Tiny couldn't parse the request headers", NULL);
            return CLOSE;
        }
        return KEEP;
    }

    c->route = route_match(&routes, c->uri);     /* 405 검사보다 먼저 */
    if (!c->route)
        c->is_static = parse_uri(c->uri, c->filename, c->cgiargs);

    /* 405: 정적 파일 경로(예: /home.html)로 POST 요청이 오는 경우 */
    if (!c->route && c->is_post && c->is_static) {
        clienterror(fd, c->filename, "405", "Method Not Allowed",
                    "POST is not allowed for static content", "GET");
        return CLOSE;
    }

    /* 411: POST인데 Content-Length가 없는 경우 */
    if (c->is_post && !c->hdrs.has_content_length) {
        clienterror(fd, "body", "411", "Length Required",
                    "POST request requires Content-Length", NULL);
        return CLOSE;
    }

    c->body_len = c->hdrs.has_content_length ? c->hdrs.content_length : 0;

    /* 413: body가 버퍼보다 큰 경우 */
    if (c->body_len > MAXBODY) {
        clienterror(fd, "body", "413", "Content Too Large",
                    "Request body is too large", NULL);
        return CLOSE;
    }

    if (c->body_len > 0) {
        c->state = READ_BODY;
        return KEEP;
    }
    finish_request(fd, c);
    return CLOSE;
}

/* READ_BODY: 받은 만큼 body 뒤에 이어 붙임. 다 받으면 응답 */
int on_body(int fd, conn_t *c)
{
    ssize_t n = rio_read_some(&c->rio, c->body + c->body_read,
                              c->body_len - c->body_read);

    /* 400: 실제 body가 Content-Length보다 짧은 채로 연결이 끊긴 경우 */
    if (n <= 0) {
        clienterror(fd, "body", "400", "Bad Request",
                    "Request body is shorter than Content-Length", NULL);
        return CLOSE;
    }
    c->body_read += n;
    if (c->body_read < c->body_len)
        return KEEP;

    c->body[c->body_len] = '\0';
    finish_request(fd, c);
    return CLOSE;
}

/* rio 버퍼에 있는 만큼, 비었으면 read 한 번만 해서 최대 n바이트 복사 */
ssize_t rio_read_some(rio_t *rp, char *usrbuf, size_t n)
{
    size_t cnt;

    if (rp->rio_cnt <= 0) {
        rp->rio_cnt = read(rp->rio_fd, rp->rio_buf, sizeof(rp->rio_buf));
        if (rp->rio_cnt <= 0)
            return rp->rio_cnt;                   /* 0: EOF, -1: 에러 */
        rp->rio_bufptr = rp->rio_buf;
    }
    cnt = (n < (size_t)rp->rio_cnt) ? n : (size_t)rp->rio_cnt;
    memcpy(usrbuf, rp->rio_bufptr, cnt);
    rp->rio_bufptr += cnt;
    rp->rio_cnt -= cnt;
    return cnt;
}

/* hop-by-hop 헤더: 이 연결에만 의미가 있어 백엔드로 넘기지 않는다 */
int is_hop_by_hop(const char *name, size_t len)
{
    static const char *hop[] = { "Connection", "Keep-Alive", "Proxy-Connection",
                                 "TE", "Trailer", "Upgrade", NULL };

    for (int i = 0; hop[i]; i++)
        if (strlen(hop[i]) == len && !strncasecmp(name, hop[i], len))
            return 1;
    return 0;
}

/* 헤더 한 줄을 hdr_raw 뒤에 붙인다. parse_header_line이 buf를 바꾸기 전에 호출 */
int save_header(conn_t *c, const char *line)
{
    size_t n = strlen(line);
    const char *colon = strchr(line, ':');

    if (colon && is_hop_by_hop(line, colon - line))
        return 0;                       /* 버리기만, 에러 아님 */
    if (c->hdr_len + n >= sizeof(c->hdr_raw))
        return -1;                      /* 헤더 전체가 너무 김 → 400 */
    memcpy(c->hdr_raw + c->hdr_len, line, n + 1);
    c->hdr_len += n;
    return 0;
}

/* 요청을 다 받은 뒤: 규칙이 있으면 프록시, 없으면 직접 응답 */
void finish_request(int fd, conn_t *c)
{
    if (c->route)
        proxy_stub(fd, c);
    else
        respond(fd, c);
}

/* 2단계 임시: 어디로 보낼지만 출력하고 502. 4단계에서 실제 전달로 교체 */
void proxy_stub(int fd, conn_t *c)
{
    char path[MAXLINE];
    int i = route_pick(c->route);
    backend_t *be = &c->route->be[i];

    if (route_rewrite_path(c->route, c->uri, path, sizeof(path)) < 0) {
        clienterror(fd, c->uri, "414", "URI Too Long",
                    "Rewritten path is too long", NULL);
        return;
    }
    printf("proxy %s %s -> %s:%s %s (from %s)\n", c->method, c->uri,
           be->host, be->port, path, c->client_ip);
    printf("forward headers:\n%s", c->hdr_raw);
    clienterror(fd, c->uri, "502", "Bad Gateway",
                "Proxy forwarding is not implemented yet", NULL);
}

/* 요청을 다 받은 뒤 파일 확인 후 정적/동적 응답 */
void respond(int fd, conn_t *c)
{
    struct stat sbuf;

    if (stat(c->filename, &sbuf) < 0) {
        clienterror(fd, c->filename, "404", "Not found",
                    "Tiny couldn't find this file", NULL);
        return;
    }

    if (c->is_static) {
        if (!(S_ISREG(sbuf.st_mode)) || !(S_IRUSR & sbuf.st_mode)) {
            clienterror(fd, c->filename, "403", "Forbidden",
                        "Tiny couldn't read this file", NULL);
            return;
        }
        serve_static(fd, c->filename, sbuf.st_size);
    } else {
        if (!(S_ISREG(sbuf.st_mode)) || !(S_IXUSR & sbuf.st_mode)) {
            clienterror(fd, c->filename, "403", "Forbidden",
                        "Tiny couldn't run the CGI program", NULL);
            return;
        }
        serve_dynamic(fd, c->filename, c->cgiargs, c->method, c->body, c->body_len);
    }
}

/* ===== 응답 ===== */

int parse_uri(char *uri, char *filename, char *cgiargs)
{
    char *ptr;

    if (!strstr(uri, "cgi-bin")) {
        strcpy(cgiargs, "");
        strcpy(filename, ".");
        strcat(filename, uri);
        if (uri[strlen(uri) - 1] == '/')
            strcat(filename, "home.html");
        return 1;
    } else {
        ptr = index(uri, '?');
        if (ptr) {
            strcpy(cgiargs, ptr + 1);
            *ptr = '\0';
        } else
            strcpy(cgiargs, "");
        strcpy(filename, ".");
        strcat(filename, uri);
        return 0;
    }
}

void serve_static(int fd, char *filename, int filesize)
{
    int srcfd;
    char *srcp, filetype[MAXLINE], buf[MAXLINE];
    int len = 0;

    get_filetype(filename, filetype);

    len += sprintf(buf + len, "HTTP/1.0 200 OK\r\n");
    len += sprintf(buf + len, "Server: Tiny Web Server\r\n");
    len += sprintf(buf + len, "Connection: close\r\n");
    len += sprintf(buf + len, "Content-length: %d\r\n", filesize);
    len += sprintf(buf + len, "Content-type: %s\r\n\r\n", filetype);
    Rio_writen(fd, buf, len);

    srcfd = Open(filename, O_RDONLY, 0);
    srcp = Mmap(0, filesize, PROT_READ, MAP_PRIVATE, srcfd, 0);
    Close(srcfd);
    Rio_writen(fd, srcp, filesize);
    Munmap(srcp, filesize);
}

static const struct { const char *ext; const char *type; } mime_table[] = {
    { "html", "text/html; charset=utf-8" },
    { "htm",  "text/html; charset=utf-8" },
    { "css",  "text/css; charset=utf-8" },
    { "js",   "text/javascript; charset=utf-8" },
    { "png",  "image/png" },
    { "jpg",  "image/jpeg" },
    { "jpeg", "image/jpeg" },
    { "gif",  "image/gif" },
    { "txt",  "text/plain; charset=utf-8" },
    { NULL,   NULL }
};

void get_filetype(char *filename, char *filetype)
{
    char *dot = strrchr(filename, '.');
    char *slash = strrchr(filename, '/');

    /* 마지막 '.'이 파일명 부분에 있을 때 mime 테이블 매핑: ./home.html */
    if (dot && (!slash || dot > slash)) {
        for (int i = 0; mime_table[i].ext; i++) {
            if (!strcasecmp(dot + 1, mime_table[i].ext)) {
                strcpy(filetype, mime_table[i].type);
                return;
            }
        }
    }
    strcpy(filetype, "application/octet-stream");
}

void serve_dynamic(int fd, char *filename, char *cgiargs,
                   char *method, char *body, long body_len)
{
    int p[2];
    char buf[MAXLINE], lenbuf[32], *emptylist[] = { NULL };

    /* pipe를 먼저 만든다. 상태줄(200)을 보낸 뒤에 실패하면 500을 보낼 수 없다. */
    if (pipe(p) < 0) {
        clienterror(fd, filename, "500", "Internal Server Error",
                    "Failed to create pipe", NULL);
        return;
    }

    sprintf(buf, "HTTP/1.0 200 OK\r\n");
    Rio_writen(fd, buf, strlen(buf));
    sprintf(buf, "Server: Tiny Web Server\r\n");
    Rio_writen(fd, buf, strlen(buf));

    if (Fork() == 0) {                            /* 자식 */
        close(p[1]);
        setenv("REQUEST_METHOD", method, 1);
        setenv("QUERY_STRING", cgiargs, 1);
        if (!strcasecmp(method, "POST")) {
            snprintf(lenbuf, sizeof(lenbuf), "%ld", body_len);
            setenv("CONTENT_LENGTH", lenbuf, 1);
        }
        dup2(p[0], STDIN_FILENO);                 /* 읽기 끝 → stdin */
        close(p[0]);
        dup2(fd, STDOUT_FILENO);                  /* stdout → 클라이언트 */
        execve(filename, emptylist, environ);
        _exit(1);                                 /* stdio 버퍼를 클라이언트로 흘리지 않게 */
    }

    close(p[0]);                                  /* 부모 */
    if (body_len > 0)
        rio_writen(p[1], body, body_len);         /* 실패해도 서버가 죽지 않게 소문자 버전 사용 */
    close(p[1]);                                  /* 자식에게 EOF 전달 */
    Wait(NULL);
}

void clienterror(int fd, char *cause, char *errnum,
                 char *shortmsg, char *longmsg, char *allow)
{
    char buf[MAXLINE], body[MAXBUF];
    int len = 0;

    len += sprintf(body + len, "<html><title>Tiny Error</title>");
    len += sprintf(body + len, "<body bgcolor=\"ffffff\">\r\n");
    len += sprintf(body + len, "%s: %s\r\n", errnum, shortmsg);
    len += sprintf(body + len, "<p>%s: %s\r\n", longmsg, cause);
    len += sprintf(body + len, "<hr><em>The Tiny Web server</em>\r\n");

    sprintf(buf, "HTTP/1.0 %s %s\r\n", errnum, shortmsg);
    Rio_writen(fd, buf, strlen(buf));
    if (allow != NULL) {                          /* 405일 때만 "GET" 전달 */
        sprintf(buf, "Allow: %s\r\n", allow);
        Rio_writen(fd, buf, strlen(buf));
    }
    sprintf(buf, "Content-type: text/html; charset=utf-8\r\n");
    Rio_writen(fd, buf, strlen(buf));
    sprintf(buf, "Content-length: %d\r\n\r\n", len);
    Rio_writen(fd, buf, strlen(buf));

    Rio_writen(fd, body, len);
}