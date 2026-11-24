#include "csapp.h"
#include "http_parse.h"

void doit(int fd);
int parse_uri(char *uri, char *filename, char *cgiargs);
void serve_static(int fd, char *filename, int filesize);
void get_filetype(char *filename, char *filetype);
void serve_dynamic(int fd, char *filename, char *cgiargs,
                   char *method, char *body, long body_len);
void clienterror(int fd, char *cause, char *errnum,
                 char *shortmsg, char *longmsg, char *allow);
void command(void);
  
int main(int argc, char **argv)
{
    int listenfd, connfd;
    socklen_t clientlen;
    struct sockaddr_storage clientaddr;
    char client_hostname[MAXLINE], client_port[MAXLINE];
    fd_set read_set, ready_set;

    if (argc != 2) {
        fprintf(stderr, "usage: %s <port>\n", argv[0]);
        exit(0);
    }

    /* CGI가 body를 다 읽기 전에 끝나도 pipe 쓰기에서 서버가 죽지 않게 한다 */
    Signal(SIGPIPE, SIG_IGN);

    listenfd = Open_listenfd(argv[1]);

    FD_ZERO(&read_set);
    FD_SET(STDIN_FILENO, &read_set);
    FD_SET(listenfd, &read_set);

    while (1) {
        ready_set = read_set;
        Select(listenfd+1, &ready_set, NULL, NULL, NULL);
        if (FD_ISSET(STDIN_FILENO, &ready_set))
            command();
        if (FD_ISSET(listenfd, &ready_set)) {
            clientlen = sizeof(struct sockaddr_storage);
            connfd = Accept(listenfd, (SA *)&clientaddr, &clientlen);
            Getnameinfo((SA *) &clientaddr, clientlen, client_hostname, MAXLINE, client_port, MAXLINE, 0);
            printf("Accepted connection from (%s, %s)\n", client_hostname, client_port);
            doit(connfd);
            Close(connfd);
        }
    }
}

void command(void) {
    char buf[MAXLINE];
    if (!Fgets(buf, MAXLINE, stdin))
        exit(0);
    printf("%s", buf);
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
        n = Rio_readnb(&rio, body, body_len);
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

    if (Fork() == 0) {                       /* 자식 */
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
