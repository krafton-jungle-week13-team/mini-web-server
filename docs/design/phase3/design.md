# Phase 3 설계: HTTP 서버 POST 지원

이 문서를 읽으면 Tiny 서버가 POST 요청의 헤더를 검사하고, body를 읽어 CGI 프로그램의 stdin으로 넘기기까지 어떤 함수를 어떻게 바꾸면 되는지 알 수 있어요.
요구사항은 [Phase 3 요구사항](../../requirements/phase3/requirements.md)을 기준으로 해요.

## 상수

```c
#define MAXBODY 8192    /* POST body 최대 크기: 8KB */
```

## 처리 순서

`doit()`은 아래 순서로 검사하고, 처음 실패한 단계의 상태 코드로 응답해요.

1. 요청줄 파싱 → 501 (GET, POST 외의 메서드)
2. `read_requesthdrs()` → 400 (헤더 형식 오류)
3. `parse_uri()` → 405 (정적 경로로 POST)
4. 411 (POST에 Content-Length 없음)
5. 413 (body가 `MAXBODY`보다 큼)
6. body 읽기 → 400 (body가 Content-Length보다 짧음)

## 헤더 파싱

```c
#include <ctype.h>

typedef struct {
    char host[MAXLINE];
    int has_host;
    long content_length;
    int has_content_length;
} req_headers_t;

static char *trim(char *s)
{
    char *end;

    while (*s == ' ' || *s == '\t')
        s++;
    end = s + strlen(s);
    while (end > s && (end[-1] == ' ' || end[-1] == '\t' ||
                       end[-1] == '\r' || end[-1] == '\n'))
        end--;
    *end = '\0';
    return s;
}

static int parse_content_length(const char *v, long *out)
{
    char *end;
    long n;

    if (!isdigit((unsigned char)*v))
        return -1;
    errno = 0;
    n = strtol(v, &end, 10);
    if (errno == ERANGE || *end != '\0')
        return -1;
    *out = n;
    return 0;
}

int read_requesthdrs(rio_t *rp, req_headers_t *h)
{
    char buf[MAXLINE], *colon, *name, *value;
    ssize_t n;
    long len;

    memset(h, 0, sizeof(*h));

    while ((n = Rio_readlineb(rp, buf, MAXLINE)) > 0) {
        if (!strcmp(buf, "\r\n"))
            return 0;
        if (buf[n - 1] != '\n')
            return -1;

        printf("%s", buf);

        colon = strchr(buf, ':');
        if (colon == NULL || colon == buf ||
            colon[-1] == ' ' || colon[-1] == '\t')
            return -1;
        *colon = '\0';
        name = buf;
        value = trim(colon + 1);

        if (!strcasecmp(name, "Host")) {
            if (h->has_host)
                return -1;
            strncpy(h->host, value, MAXLINE - 1);
            h->has_host = 1;
        } else if (!strcasecmp(name, "Content-Length")) {
            if (parse_content_length(value, &len) < 0)
                return -1;
            if (h->has_content_length && h->content_length != len)
                return -1;
            h->content_length = len;
            h->has_content_length = 1;
        }
    }
    return -1;
}
```

`read_requesthdrs()`가 `-1`을 돌려주면 `doit()`은 400 Bad Request로 응답해요.

- **헤더의 끝:** 빈 줄(`\r\n`)이 나오면 헤더가 끝나요. 빈 줄이 오기 전에 연결이 끊기면(EOF) 불완전한 요청이라 400이에요.
- **너무 긴 줄:** `Rio_readlineb`는 줄이 `MAXLINE`보다 길면 `\n` 없이 잘라서 돌려줘요. 그래서 읽은 줄이 `\n`으로 끝나는지 꼭 확인해야 해요. 안 그러면 줄의 나머지가 다음 헤더로 읽혀요.
- **콜론 위치:** 첫 번째 `:`를 기준으로 나눠요. `:`가 없거나, 이름이 비었거나, 이름과 `:` 사이에 공백이 있으면(`Host : x`) 400이에요.
- **이름은 대소문자 구분 없음:** `host`, `HOST`도 같은 헤더예요. `strcasecmp`로 비교해요.
- **값 앞뒤 공백 제거:** `Host:  localhost  ` → `localhost`
- **중복 헤더:** Host가 두 번 오거나, Content-Length가 서로 다른 값으로 두 번 오면 400이에요.
- **Content-Length 형식:** 숫자로 시작하지 않거나(`-1`, `abc`) 숫자 뒤에 다른 문자가 붙으면 400이에요.

## 유효성 검사

### clienterror 시그니처 변경

405 응답에 `Allow` 헤더를 넣기 위해 `clienterror()`에 `allow` 인자를 추가해요.

```c
void clienterror(int fd, char *cause, char *errnum,
                 char *shortmsg, char *longmsg, char *allow);
```

- `allow`: `char *` — 응답에 넣을 `Allow` 헤더 값이에요. 405일 때만 `"GET"`을 넘기고, 다른 에러에서는 `NULL`을 넘겨요.

```c
sprintf(buf, "HTTP/1.0 %s %s\r\n", errnum, shortmsg);
Rio_writen(fd, buf, strlen(buf));
if (allow != NULL) {                       /* 405일 때만 "GET" 전달 */
    sprintf(buf, "Allow: %s\r\n", allow);
    Rio_writen(fd, buf, strlen(buf));
}
```

### 405, 411, 413 검사와 body 읽기

405는 정적 파일 경로(예: `/home.html`)로 POST 요청이 온 경우예요.

```c
if (is_post && is_static) {
    clienterror(fd, filename, "405", "Method Not Allowed",
                "POST is not allowed for static content", "GET");
    return;
}

if (is_post && !hdrs.has_content_length) {
    clienterror(fd, "body", "411", "Length Required",
                "POST request requires Content-Length", NULL);
    return;
}

long body_len = hdrs.has_content_length ? hdrs.content_length : 0;

if (body_len > MAXBODY) {
    clienterror(fd, "body", "413", "Content Too Large",
                "Request body is too large", NULL);
    return;
}
if (body_len > 0) {
    n = Rio_readnb(&rio, body, body_len);
    if (n != body_len) {
        clienterror(fd, "body", "400", "Bad Request",
                    "Request body is shorter than Content-Length", NULL);
        return;
    }
}
body[body_len] = '\0';
```

body는 헤더를 읽은 같은 `rio` 버퍼로 읽어요. 새 버퍼로 읽으면 헤더와 함께 이미 버퍼에 들어온 body 일부를 잃어요.

## body 전달

### doit

```c
void doit(int fd)
{
    char body[MAXBODY + 1];        /* ① 여기서 만들어져요 (doit의 스택) */
    ...
    /* 요청줄 파싱 → 501 검사 */
    /* read_requesthdrs → 400 검사 */
    /* parse_uri → 405 검사 → 411 검사 → 413 검사 */

    n = Rio_readnb(&rio, body, body_len);   /* ② 여기서 채워져요 */
    body[body_len] = '\0';

    if (is_static)
        serve_static(fd, filename, filesize);
    else
        serve_dynamic(fd, filename, cgiargs, method, body, body_len);  /* ③ 전달 */
}
```

### serve_dynamic

부모가 body를 파이프에 쓰고, 자식은 파이프의 읽기 끝을 stdin으로 받아요.

```c
void serve_dynamic(int fd, char *filename, char *cgiargs,
                   char *method, char *body, long body_len)
{
    int p[2];
    char lenbuf[32];
    char *emptylist[] = { NULL };

    /* 상태줄 + Server 헤더 전송 (기존 코드) */

    if (pipe(p) < 0) { /* 500 */ }

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
        rio_writen(p[1], body, body_len);    /* 실패 시 서버가 죽지 않게 소문자 버전 사용 */
    close(p[1]);                             /* 자식에게 EOF 전달 */
    Wait(NULL);
}
```

## 환경변수 세팅

| 항목 | GET | POST |
| --- | --- | --- |
| 데이터 위치 | URL의 `?` 뒤 (`/cgi-bin/adder?num1=3&num2=16`) | 요청 body (`num1=3&num2=16`) |
| `REQUEST_METHOD` | `"GET"` | `"POST"` |
| `QUERY_STRING` | `?` 뒤 문자열 (`"num1=3&num2=16"`) | `""` (빈 문자열). URL에 `?`가 있으면 그 값 |
| `CONTENT_LENGTH` | 설정하지 않음 (body가 없으니까) | body 바이트 수 (`"14"`) |
| `CONTENT_TYPE` | 설정하지 않음 | 요청 헤더 값 그대로 (`"application/x-www-form-urlencoded"`). 선택 |

## adder 수정

adder는 `REQUEST_METHOD`를 보고 입력을 어디서 읽을지 정해요.
POST면 `CONTENT_LENGTH`를 환경변수에서 가져와 그 길이만큼 body를 stdin에서 읽고, GET이면 `QUERY_STRING`을 읽어요.

```c
char *method = getenv("REQUEST_METHOD");
char buf[MAXBODY + 1];
char *end;
long len;

if (method && !strcmp(method, "POST")) {
    char *lenstr = getenv("CONTENT_LENGTH");
    if (lenstr == NULL) { /* 에러 */ }
    len = strtol(lenstr, &end, 10);
    if (*end != '\0' || len < 0 || len > MAXBODY) { /* 에러 */ }
    if (len > 0 && rio_readn(STDIN_FILENO, buf, len) != len) { /* 에러 */ }
    buf[len] = '\0';
} else {
    char *qs = getenv("QUERY_STRING");
    if (qs == NULL) { /* 에러 */ }
    strncpy(buf, qs, MAXBODY);
    buf[MAXBODY] = '\0';
}
/* 이후 buf에서 num1, num2 파싱 (공통) */
```
