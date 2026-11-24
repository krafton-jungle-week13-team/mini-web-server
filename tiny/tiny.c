#include "csapp.h"
#include "http_parse.h"

void *thread(void *vargp);
static void set_cloexec(int fd);
void doit(int fd);
int parse_uri(char *uri, char *filename, char *cgiargs);
void serve_static(int fd, char *filename, int filesize);
void get_filetype(char *filename, char *filetype);
void serve_dynamic(int fd, char *filename, char *cgiargs,
                   char *method, char *body, long body_len);
void clienterror(int fd, char *cause, char *errnum,
                 char *shortmsg, char *longmsg, char *allow);

int main(int argc, char **argv)
{
    int listenfd, *connfdp, rc;
    socklen_t clientlen;
    struct sockaddr_storage clientaddr;
    char client_hostname[MAXLINE], client_port[MAXLINE];
    pthread_t tid;

    if (argc != 2) {
        fprintf(stderr, "usage: %s <port>\n", argv[0]);
        exit(0);
    }

    /* CGI가 body를 다 읽기 전에 끝나도 pipe 쓰기에서 서버가 죽지 않게 한다 */
    Signal(SIGPIPE, SIG_IGN);

    listenfd = Open_listenfd(argv[1]);
    set_cloexec(listenfd);                   /* CGI 자식이 exec할 때 listenfd를 물려받지 않게 */

    while (1) {
        clientlen = sizeof(struct sockaddr_storage);
        /* connfd마다 malloc한 칸을 따로 준다.
         * 지역 변수 주소(&connfd)를 넘기면 다음 accept가 값을 덮어써서
         * 두 스레드가 같은 connfd를 쓰는 경쟁이 생긴다 (CS:APP 12.3.8) */
        connfdp = Malloc(sizeof(int));
        *connfdp = accept(listenfd, (SA *)&clientaddr, &clientlen);
        if (*connfdp < 0) {                  /* ECONNABORTED 등: 이 연결만 버리고 서버는 계속 */
            free(connfdp);
            continue;
        }
        set_cloexec(*connfdp);               /* 다른 스레드의 CGI 자식에게 넘어가지 않게 */
        Getnameinfo((SA *) &clientaddr, clientlen, client_hostname, MAXLINE, client_port, MAXLINE, 0);
        printf("Accepted connection from (%s, %s)\n", client_hostname, client_port);

        if ((rc = pthread_create(&tid, NULL, thread, connfdp)) != 0) {
            fprintf(stderr, "pthread_create error: %s\n", strerror(rc));
            close(*connfdp);
            free(connfdp);
        }
    }
    exit(0);
}

/* 연결 하나를 처리하는 피어 스레드 */
void *thread(void *vargp)
{
    int connfd = *((int *)vargp);

    Pthread_detach(pthread_self());          /* join하지 않으므로 끝나면 스스로 정리되게 */
    Free(vargp);                             /* main이 malloc한 칸은 값을 옮긴 뒤 해제 */
    doit(connfd);
    Close(connfd);                           /* fd는 스레드끼리 공유하므로 main은 닫지 않는다 */
    return NULL;
}

/* exec할 때 fd가 자동으로 닫히게 한다.
 * 스레드는 fd를 공유하고 fork는 그 순간 열린 fd를 전부 복사하므로, 표시가 없으면
 * 다른 스레드가 연 connfd/pipe가 CGI 자식에게 넘어가 참조 카운트가 남는다
 * → 그 연결의 클라이언트는 CGI가 끝날 때까지 연결 종료(EOF)를 받지 못한다.
 * accept/pipe와 이 호출 사이에 다른 스레드가 fork하면 막지 못하는 작은 틈이 있다.
 * Linux의 accept4(SOCK_CLOEXEC)/pipe2(O_CLOEXEC)는 이 틈이 없지만 macOS에는 없어서 쓰지 않는다. */
static void set_cloexec(int fd)
{
    int flags = fcntl(fd, F_GETFD);
    if (flags >= 0)
        fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
}

void doit(int fd)
{
    int is_static, is_post;
    struct stat sbuf;
    char body[MAXBODY + 1];
    long body_len;
    ssize_t n;
    char buf[MAXLINE], method[MAXLINE], uri[MAXLINE], version[MAXLINE];
    char filename[MAXLINE], cgiargs[MAXLINE];
    rio_t rio;
    req_headers_t hdrs;

    /* 스레드 안에서는 대문자 래퍼를 피한다. 에러 때 exit()을 불러 서버 전체가 죽기 때문이다.
     * 소문자 rio 함수는 -1을 돌려주므로 이 연결만 정리하고 돌아간다. */
    Rio_readinitb(&rio, fd);
    if (rio_readlineb(&rio, buf, MAXLINE) <= 0)   /* EOF 또는 ECONNRESET 등 */
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

    is_static = parse_uri(uri, filename, cgiargs);

    /* 405: 정적 파일 경로(예: /home.html)로 POST 요청이 오는 경우 */
    if (is_post && is_static) {
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
        n = rio_readnb(&rio, body, body_len);    /* 에러면 -1 → 아래 400 처리 */
        if (n != body_len) {
            clienterror(fd, "body", "400", "Bad Request",
                        "Request body is shorter than Content-Length", NULL);
            return;
        }
    }
    body[body_len] = '\0';

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

    if (filesize == 0)                       /* mmap은 길이 0이면 실패하므로 본문 없이 끝낸다 */
        return;

    /* O_CLOEXEC: 다른 스레드의 CGI 자식에게 이 fd가 넘어가지 않게 한다 */
    if ((srcfd = open(filename, O_RDONLY | O_CLOEXEC, 0)) < 0)
        return;
    srcp = mmap(0, filesize, PROT_READ, MAP_PRIVATE, srcfd, 0);
    close(srcfd);
    if (srcp == MAP_FAILED)
        return;
    Rio_writen(fd, srcp, filesize);
    munmap(srcp, filesize);
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

void serve_dynamic(int fd, char *filename, char *cgiargs,
                   char *method, char *body, long body_len)
{
    int p[2];
    pid_t pid;
    char buf[MAXLINE], lenbuf[32], *emptylist[] = { NULL };

    /* pipe를 먼저 만든다. 상태줄(200)을 보낸 뒤에 실패하면 500을 보낼 수 없다. */
    if (pipe(p) < 0) {
        clienterror(fd, filename, "500", "Internal Server Error",
                    "Failed to create pipe", NULL);
        return;
    }
    /* 다른 스레드의 CGI 자식이 이 pipe를 물려받으면, 그 자식이 끝날 때까지
     * 쓰기 끝의 참조가 남아 우리 CGI가 stdin에서 EOF를 받지 못한다.
     * 우리 자식에게 필요한 읽기 끝은 아래 dup2로 stdin에 복사되는데,
     * dup2로 만든 fd에는 CLOEXEC가 붙지 않아서 exec 후에도 남는다. */
    set_cloexec(p[0]);
    set_cloexec(p[1]);

    sprintf(buf, "HTTP/1.0 200 OK\r\n");
    Rio_writen(fd, buf, strlen(buf));
    sprintf(buf, "Server: Tiny Web Server\r\n");
    Rio_writen(fd, buf, strlen(buf));

    if ((pid = fork()) < 0) {                /* Fork()는 실패 시 exit()하므로 소문자 사용 */
        close(p[0]);
        close(p[1]);
        return;                              /* 상태줄은 이미 보냈으니 연결만 닫힌다 */
    }
    if (pid == 0) {                          /* 자식 */
        close(p[1]);
        setenv("REQUEST_METHOD", method, 1);
        setenv("QUERY_STRING", cgiargs, 1);
        if (!strcasecmp(method, "POST")) {
            snprintf(lenbuf, sizeof(lenbuf), "%ld", body_len);
            setenv("CONTENT_LENGTH", lenbuf, 1);
        }
        dup2(p[0], STDIN_FILENO);            /* 읽기 끝 → stdin */
        close(p[0]);
        dup2(fd, STDOUT_FILENO);             /* stdout → 클라이언트 */
        execve(filename, emptylist, environ);
        exit(1);
    }

    close(p[0]);                             /* 부모 */
    if (body_len > 0)
        rio_writen(p[1], body, body_len);    /* 실패해도 서버가 죽지 않게 소문자 버전 사용 */
    close(p[1]);                             /* 자식에게 EOF 전달 */
    /* Wait(NULL)은 "아무 자식이나" 회수한다. 여러 스레드가 동시에 CGI를 돌리면
     * 다른 스레드의 자식을 가로채고, 빼앗긴 쪽 Wait는 ECHILD로 exit() → 서버 전체 종료.
     * 그래서 내가 만든 pid만 기다린다. */
    while (waitpid(pid, NULL, 0) < 0 && errno == EINTR)
        ;
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
