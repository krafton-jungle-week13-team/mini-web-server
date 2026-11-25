#include "csapp.h"
#include "http_parse.h"
#include "sbuf.h"
#include "route.h"
#ifdef __linux__
#include <sys/syscall.h>
#endif

/* 스레드 풀 크기. make CFLAGS="... -DNTHREADS=8"처럼 바꿀 수 있다. */
#ifndef NTHREADS
#define NTHREADS 4      /* 미리 만들어 두는 worker 스레드 수 */
#endif
#ifndef SBUFSIZE
#define SBUFSIZE 16     /* worker를 기다리는 연결을 담는 칸 수 */
#endif

static sbuf_t sbuf;     /* main(생산자) → worker(소비자)로 connfd를 넘기는 버퍼 */

/* 리버스 프록시 라우팅 규칙 (-c 설정 파일). main에서 한 번 읽고 이후에는 읽기만 한다.
 * 단, route_pick은 라운드로빈 차례(rr)를 바꾸므로 rr_lock으로 감싼다. */
static route_table_t routes;
static pthread_mutex_t rr_lock = PTHREAD_MUTEX_INITIALIZER;

void *worker(void *vargp);
void doit(int fd);
void proxy_request(int fd, route_t *rt, char *method, char *uri,
                   req_headers_t *hdrs, char *body, long body_len);
int parse_uri(char *uri, char *filename, char *cgiargs);
void serve_static(int fd, char *filename, int filesize);
void get_filetype(char *filename, char *filetype);
void serve_dynamic(int fd, char *filename, char *cgiargs,
                   char *method, char *body, long body_len);
void clienterror(int fd, char *cause, char *errnum,
                 char *shortmsg, char *longmsg, char *allow);

int main(int argc, char **argv)
{
    int i, listenfd, connfd;
    socklen_t clientlen;
    struct sockaddr_storage clientaddr;
    char client_hostname[MAXLINE], client_port[MAXLINE];
    pthread_t tid;

    if (argc != 2 && !(argc == 4 && !strcmp(argv[2], "-c"))) {
        fprintf(stderr, "usage: %s <port> [-c routes.conf]\n", argv[0]);
        exit(0);
    }
    if (argc == 4) {
        if (route_load(argv[3], &routes) < 0)
            exit(1);
        route_print(&routes);
    }

    /* CGI가 body를 다 읽기 전에 끝나도 pipe 쓰기에서 서버가 죽지 않게 한다 */
    Signal(SIGPIPE, SIG_IGN);

    listenfd = Open_listenfd(argv[1]);

    /* worker를 미리 만들어 두고 요청마다 재사용 */
    sbuf_init(&sbuf, SBUFSIZE);
    for (i = 0; i < NTHREADS; i++)
        Pthread_create(&tid, NULL, worker, NULL);

    /* main은 연결을 받아 버퍼에 넣기만 함. 버퍼 차면 빈 칸이 날 때까지 기다림 */
    while (1) {
        clientlen = sizeof(struct sockaddr_storage);
        connfd = Accept(listenfd, (SA *)&clientaddr, &clientlen); 
        Getnameinfo((SA *) &clientaddr, clientlen, client_hostname, MAXLINE, client_port, MAXLINE, 0);
        printf("Accepted connection from (%s, %s)\n", client_hostname, client_port);
        sbuf_insert(&sbuf, connfd);
    }
    exit(0);
}

/* worker 스레드: 버퍼에서 connfd를 꺼내 요청 하나를 처리하고 닫기를 반복한다 */
void *worker(void *vargp)
{
    Pthread_detach(pthread_self()); 
    while (1) {
        int connfd = sbuf_remove(&sbuf);
        doit(connfd);
        Close(connfd);
    }
    return NULL;
}

void doit(int fd)
{
    int is_static, is_post;
    route_t *rt;
    struct stat sbuf;
    char body[MAXBODY + 1];
    long body_len;
    ssize_t n;
    char buf[MAXLINE], method[MAXLINE], uri[MAXLINE], version[MAXLINE];
    char filename[MAXLINE], cgiargs[MAXLINE];
    rio_t rio;
    req_headers_t hdrs;

    Rio_readinitb(&rio, fd);
    if (!Rio_readlineb(&rio, buf, MAXLINE))
        return;
    printf("Request headers:\n");
    printf("%s", buf);

    sscanf(buf, "%s %s %s", method, uri, version);
    printf("method=[%s] uri=[%s] version=[%s]\n", method, uri, version);

    is_post = !strcasecmp(method, "POST");
    if (strcasecmp(method, "GET") && !is_post) {
        clienterror(fd, method, "501", "Not implemented", "Tiny does not implement this method", NULL);
        return;
    }

    if (read_requesthdrs(&rio, &hdrs) < 0) {
        clienterror(fd, "header", "400", "Bad Request", "Tiny couldn't parse the request headers", NULL);
        return;
    }

    /* 라우팅 규칙에 맞으면 백엔드로 전달한다. 맞는 규칙이 없으면 아래의 정적/CGI 처리. */
    rt = route_match(&routes, uri);
    is_static = rt ? 0 : parse_uri(uri, filename, cgiargs);

    /* 405: 정적 파일 경로(예: /home.html)로 POST 요청이 오는 경우 */
    if (!rt && is_post && is_static) {
        clienterror(fd, filename, "405", "Method Not Allowed",
                    "POST is not allowed for static content", "GET");
        return;
    }

    /* 411: POST인데 Content-Length가 없는 경우 */
    if (is_post && !hdrs.has_content_length) {
        clienterror(fd, "body", "411", "Length Required",
                    "POST request requires Content-Length", NULL);
        return;
    }

    body_len = hdrs.has_content_length ? hdrs.content_length : 0;

    /* 413: body가 버퍼보다 큰 경우 */
    if (body_len > MAXBODY) {
        clienterror(fd, "body", "413", "Content Too Large",
                    "Request body is too large", NULL);
        return;
    }

    /* body 읽기 + 400: 실제 body가 Content-Length보다 짧은 경우 */
    if (body_len > 0) {
        n = Rio_readnb(&rio, body, body_len);
        if (n != body_len) {
            clienterror(fd, "body", "400", "Bad Request",
                        "Request body is shorter than Content-Length", NULL);
            return;
        }
    }
    body[body_len] = '\0';

    /* 프록시 응답의 끝은 백엔드가 연결을 닫는 것으로 안다 */
    if (rt) {
        proxy_request(fd, rt, method, uri, &hdrs, body, body_len);
        return;
    }

    if (stat(filename, &sbuf) < 0) {
        clienterror(fd, filename, "404", "Not found", "Tiny couldn't find this file", NULL);
        return;
    }

     if (is_static) {
        if (!(S_ISREG(sbuf.st_mode)) || !(S_IRUSR & sbuf.st_mode)) {
            clienterror(fd, filename, "403", "Forbidden", "Tiny couldn't read this file", NULL);
            return;
        }
        serve_static(fd, filename, sbuf.st_size);
    }
    else {
        if (!(S_ISREG(sbuf.st_mode)) || !(S_IXUSR & sbuf.st_mode)) {
            clienterror(fd, filename, "403", "Forbidden", "Tiny couldn't run the CGI program", NULL);
            return;
        }
        serve_dynamic(fd, filename, cgiargs, method, body, body_len);
    }
}

int parse_uri(char *uri, char *filename, char *cgiargs)
{
    char *ptr;

     if (!strstr(uri, "cgi-bin")) {
        strcpy(cgiargs, "");
        strcpy(filename, ".");
        strcat(filename, uri);
        if (uri[strlen(uri)-1] == '/')
            strcat(filename, "home.html");
        return 1;
     }

    else {
        ptr = index(uri, '?');
        if (ptr) {
            strcpy(cgiargs, ptr+1);
            *ptr = '\0';
        }
        else
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

    /* 마지막 '.'이 파일명 부분에 있을 때 mime 테이블 매핑: ./home.html, */
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

/* CGI로 넘길 변수인지 확인한다 (서버 환경에 같은 이름이 있으면 요청 값으로 바꾸려고) */
static int is_cgi_var(const char *kv)
{
    return !strncmp(kv, "REQUEST_METHOD=", 15) ||
           !strncmp(kv, "QUERY_STRING=", 13) ||
           !strncmp(kv, "CONTENT_LENGTH=", 15);
}

/* fork한 자식에서 3번 이상의 fd를 모두 닫는다.
 * 다른 worker의 connfd·pipe까지 물려받기 때문에, 닫지 않으면
 * 그 연결의 종료(EOF)가 이 CGI가 끝날 때까지 늦어진다. */
static void close_inherited_fds(long maxfd)
{
    long i;

#if defined(__linux__) && defined(SYS_close_range)
    if (syscall(SYS_close_range, 3, ~0U, 0) == 0)
        return;
#endif
    for (i = 3; i < maxfd; i++)
        close(i);
}

void serve_dynamic(int fd, char *filename, char *cgiargs,
                   char *method, char *body, long body_len)
{
    int p[2], i, n;
    pid_t pid;
    long maxfd;
    char buf[MAXLINE], *emptylist[] = { NULL };
    char method_env[MAXLINE + 32], query_env[MAXLINE + 32], length_env[64];
    char **envp;

    /* pipe를 먼저 만든다. 상태줄(200)을 보낸 뒤에 실패하면 500을 보낼 수 없다. */
    if (pipe(p) < 0) {
        clienterror(fd, filename, "500", "Internal Server Error",
                    "Failed to create pipe", NULL);
        return;
    }

    /* 환경 변수 배열은 부모에서 미리 만든다.
     * 여러 스레드가 있는 프로세스에서 fork한 자식은 exec 전에 setenv(내부 malloc)를 부르면
     * 다른 스레드가 잡고 있던 락에 걸릴 수 있다. */
    snprintf(method_env, sizeof(method_env), "REQUEST_METHOD=%s", method);
    snprintf(query_env, sizeof(query_env), "QUERY_STRING=%s", cgiargs);
    snprintf(length_env, sizeof(length_env), "CONTENT_LENGTH=%ld", body_len);
    for (i = 0; environ[i]; i++)
        ;
    envp = Malloc((i + 4) * sizeof(char *));
    for (i = 0, n = 0; environ[i]; i++)
        if (!is_cgi_var(environ[i]))
            envp[n++] = environ[i];
    envp[n++] = method_env;
    envp[n++] = query_env;
    if (!strcasecmp(method, "POST"))
        envp[n++] = length_env;
    envp[n] = NULL;

    maxfd = sysconf(_SC_OPEN_MAX);
    if (maxfd < 0)
        maxfd = 1024;

    sprintf(buf, "HTTP/1.0 200 OK\r\n");
    Rio_writen(fd, buf, strlen(buf));
    sprintf(buf, "Server: Tiny Web Server\r\n");
    Rio_writen(fd, buf, strlen(buf));

    if ((pid = Fork()) == 0) {               /* 자식 */
        dup2(p[0], STDIN_FILENO);            /* 읽기 끝 → stdin */
        dup2(fd, STDOUT_FILENO);             /* stdout → 클라이언트 */
        close_inherited_fds(maxfd);          /* p[0], p[1], fd와 다른 연결의 fd까지 닫음 */
        execve(filename, emptylist, envp);
        _exit(1);                            /* exit는 부모에게서 복사된 printf 버퍼를 클라이언트로 내보낸다 */
    }

    Free(envp);
    close(p[0]);                             /* 부모 */
    if (body_len > 0)
        rio_writen(p[1], body, body_len);    /* 실패해도 서버가 죽지 않게 소문자 버전 사용 */
    close(p[1]);                             /* 자식에게 EOF 전달 */
    Waitpid(pid, NULL, 0);                   /* wait는 다른 worker의 자식을 거둘 수 있다 */
}

/* ---------- 리버스 프록시 ---------- */

/* 이 연결에만 의미가 있는 헤더(hop-by-hop)는 백엔드로 넘기지 않는다 (RFC 9110 7.6.1) */
static int is_hop_by_hop(const char *name, size_t len)
{
    static const char *hop[] = { "Connection", "Keep-Alive", "Proxy-Connection", "TE",
                                 "Trailer", "Transfer-Encoding", "Upgrade", NULL };

    for (int i = 0; hop[i]; i++)
        if (strlen(hop[i]) == len && !strncasecmp(name, hop[i], len))
            return 1;
    return 0;
}

/* 백엔드에 연결한다. 성공하면 fd, 실패하면 -1.
 * open_clientfd와 같지만 실패해도 서버를 끝내지 않는다 (Open_clientfd는 unix_error로 exit). */
static int open_backend(const backend_t *be)
{
    struct addrinfo hints, *list, *p;
    int fd = -1;

    memset(&hints, 0, sizeof(hints));
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_NUMERICSERV;
    if (getaddrinfo(be->host, be->port, &hints, &list) != 0)
        return -1;
    for (p = list; p; p = p->ai_next) {
        if ((fd = socket(p->ai_family, p->ai_socktype, p->ai_protocol)) < 0)
            continue;
        if (connect(fd, p->ai_addr, p->ai_addrlen) == 0)
            break;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(list);
    return fd;
}

/* dst에 src n바이트를 붙인다. 넘치면 -1 */
static int append(char *dst, size_t cap, size_t *len, const char *src, size_t n)
{
    if (*len + n >= cap)
        return -1;
    memcpy(dst + *len, src, n);
    *len += n;
    dst[*len] = '\0';
    return 0;
}

static ssize_t read_retry(int fd, char *buf, size_t n)
{
    ssize_t rc;

    while ((rc = read(fd, buf, n)) < 0 && errno == EINTR)
        ;
    return rc;
}

/* 요청을 라우팅 규칙의 백엔드로 전달하고, 백엔드 응답을 그대로 클라이언트에 돌려준다.
 * 백엔드에는 HTTP/1.0 + Connection: close로 보내서 응답의 끝을 연결 종료로 안다. */
void proxy_request(int fd, route_t *rt, char *method, char *uri,
                   req_headers_t *hdrs, char *body, long body_len)
{
    char path[MAXLINE], req[MAXLINE + MAXBUF + 2 * MAXLINE], buf[MAXBUF];
    char client_ip[NI_MAXHOST], xff[MAXLINE], tmp[MAXLINE];
    char *line, *next, *colon, *v;
    struct sockaddr_storage peer;
    socklen_t peerlen = sizeof(peer);
    size_t len = 0, nlen;
    ssize_t n;
    backend_t *be;
    int bfd, i;

    if (hdrs->raw_overflow) {
        clienterror(fd, "header", "431", "Request Header Fields Too Large",
                    "Request headers are too large to forward", NULL);
        return;
    }
    if (route_rewrite_path(rt, uri, path, sizeof(path)) < 0) {
        clienterror(fd, uri, "414", "URI Too Long", "Rewritten path is too long", NULL);
        return;
    }

    pthread_mutex_lock(&rr_lock);
    i = route_pick(rt);
    pthread_mutex_unlock(&rr_lock);
    be = &rt->be[i];

    if (getpeername(fd, (SA *)&peer, &peerlen) < 0 ||
        getnameinfo((SA *)&peer, peerlen, client_ip, sizeof(client_ip), NULL, 0, NI_NUMERICHOST) != 0)
        strcpy(client_ip, "unknown");
    printf("Proxy %s %s -> %s:%s%s\n", method, uri, be->host, be->port, path);

    /* 요청줄 + 클라이언트 헤더(hop-by-hop 제외) + Host(없으면) + X-Forwarded-For + Connection: close */
    snprintf(req, sizeof(req), "%s %s HTTP/1.0\r\n", method, path);
    len = strlen(req);
    xff[0] = '\0';
    for (line = hdrs->raw; *line; line = next) {
        next = strchr(line, '\n');
        next = next ? next + 1 : line + strlen(line);
        colon = strchr(line, ':');               /* read_requesthdrs가 ':'가 있는 줄만 받음 */
        nlen = colon - line;
        if (is_hop_by_hop(line, nlen))
            continue;
        if (nlen == 15 && !strncasecmp(line, "X-Forwarded-For", 15)) {
            /* 앞단 프록시가 붙인 값 뒤에 이번 클라이언트 IP를 덧붙인다 */
            snprintf(tmp, sizeof(tmp), "%.*s", (int)(next - colon - 1), colon + 1);
            v = trim(tmp);
            if (*v)
                snprintf(xff + strlen(xff), sizeof(xff) - strlen(xff), "%s, ", v);
            continue;
        }
        if (append(req, sizeof(req), &len, line, next - line) < 0)
            break;
    }
    if (!hdrs->has_host) {
        snprintf(tmp, sizeof(tmp), "Host: %s:%s\r\n", be->host, be->port);
        append(req, sizeof(req), &len, tmp, strlen(tmp));
    }
    snprintf(tmp, sizeof(tmp), "X-Forwarded-For: %s%s\r\nConnection: close\r\n\r\n", xff, client_ip);
    if (append(req, sizeof(req), &len, tmp, strlen(tmp)) < 0) {
        clienterror(fd, "header", "431", "Request Header Fields Too Large",
                    "Request headers are too large to forward", NULL);
        return;
    }

    if ((bfd = open_backend(be)) < 0) {
        clienterror(fd, uri, "502", "Bad Gateway", "Tiny couldn't connect to the backend", NULL);
        return;
    }

    if (rio_writen(bfd, req, len) != (ssize_t)len ||
        (body_len > 0 && rio_writen(bfd, body, body_len) != body_len)) {
        close(bfd);
        clienterror(fd, uri, "502", "Bad Gateway", "Tiny couldn't send the request to the backend", NULL);
        return;
    }

    /* 백엔드 응답은 시간 제한 없이 기다린다. 응답하지 않는 백엔드는 이 worker를 계속 붙잡는다.
     * 첫 응답 바이트가 오기 전까지만 502를 보낼 수 있다.
     * 그 뒤에 실패하면 이미 응답 일부가 나갔으므로 연결만 끊는다. */
    n = read_retry(bfd, buf, sizeof(buf));
    if (n <= 0) {
        close(bfd);
        clienterror(fd, uri, "502", "Bad Gateway", "The backend closed the connection without a response", NULL);
        return;
    }
    do {
        if (rio_writen(fd, buf, n) != n)         /* 클라이언트가 떠남 */
            break;
    } while ((n = read_retry(bfd, buf, sizeof(buf))) > 0);
    close(bfd);
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
    if (allow != NULL) {                     /* 405일 때만 "GET" 전달 */
        sprintf(buf, "Allow: %s\r\n", allow);
        Rio_writen(fd, buf, strlen(buf));
    }
    sprintf(buf, "Content-type: text/html; charset=utf-8\r\n");
    Rio_writen(fd, buf, strlen(buf));
    sprintf(buf, "Content-length: %d\r\n\r\n", len);
    Rio_writen(fd, buf, strlen(buf));

    Rio_writen(fd, body, len);
}
