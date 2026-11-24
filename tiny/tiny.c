#include "csapp.h"
#include "http_parse.h"
#include "sbuf.h"
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

void *worker(void *vargp);
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
    int i, listenfd, connfd;
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
