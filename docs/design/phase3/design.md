# HTTP 서버 POST 지원

## 1. 전체 흐름

요청 하나는 `doit`에서 아래 순서로 처리돼요. 검사에 걸리면 그 자리에서 에러 응답을 보내고 끝나요.

| 순서 | 단계 | 실패 시 |
| --- | --- | --- |
| 1 | 요청줄 파싱 (GET/POST만 허용) | 501 Not Implemented |
| 2 | 헤더 파싱 (`read_requesthdrs`) | 400 Bad Request |
| 3 | 정적 파일에 POST | 405 Method Not Allowed |
| 4 | POST인데 Content-Length 없음 | 411 Length Required |
| 5 | body가 `MAXBODY`보다 큼 | 413 Content Too Large |
| 6 | body가 Content-Length보다 짧음 | 400 Bad Request |
| 7 | 정적 → `serve_static` / 동적 → `serve_dynamic` | |

body 최대 크기는 8KB예요. `tiny.c`와 `adder.c`가 같은 값을 쓰도록 `csapp.h`에 정의해요.

```c
#define MAXBODY 8192    /* POST body 최대 크기 (8KB) */
```

```c
void doit(int fd)
{
    char body[MAXBODY + 1];        /* ① 여기서 만들어져요 (doit의 스택) */
    req_headers_t hdrs;
    ...
    /* 요청줄 파싱 → 501 검사 */
    /* read_requesthdrs → 400 검사 */
    /* parse_uri → 405 검사 → 411 검사 → 413 검사 */
    /* body 읽기 → 400 검사 (3장 참고) */   /* ② 여기서 채워져요 */

    if (is_static)
        serve_static(fd, filename, filesize);
    else
        serve_dynamic(fd, filename, cgiargs, method, body, body_len);  /* ③ 전달 */
}
```

## 2. 헤더 파싱

### 파일 분리

헤더 파서는 `tiny.c`가 아니라 `http_parse.c`, `http_parse.h`에 둬요.
`tiny.c`에는 `main`이 있어서 테스트 코드와 함께 링크할 수 없고, 함수가 `static`이면 다른 파일에서 호출할 수 없기 때문이에요.
파서를 따로 두면 테스트 코드가 `http_parse.o`만 링크해서 함수를 직접 시험할 수 있어요.

| 파일 | 내용 |
| --- | --- |
| `http_parse.h` | `req_headers_t` 정의, `trim`, `parse_content_length`, `read_requesthdrs` 선언 |
| `http_parse.c` | 세 함수 구현 (`static` 없이) |
| `tiny.c` | `#include "http_parse.h"` 후 `read_requesthdrs` 호출 |

```c
/* http_parse.h */
#ifndef HTTP_PARSE_H
#define HTTP_PARSE_H

#include "csapp.h"

typedef struct {
    char host[MAXLINE];
    int has_host;
    long content_length;
    int has_content_length;
} req_headers_t;

char *trim(char *s);
int parse_content_length(const char *v, long *out);   /* 성공 0, 실패 -1 */
int read_requesthdrs(rio_t *rp, req_headers_t *h);    /* 성공 0, 실패 -1 → 400 */

#endif
```

Makefile은 `tiny`가 `http_parse.o`도 함께 링크하도록 바꿔요.

```makefile
tiny: tiny.c csapp.o http_parse.o
	$(CC) $(CFLAGS) -o tiny tiny.c csapp.o http_parse.o $(LIB)

http_parse.o: http_parse.c http_parse.h
	$(CC) $(CFLAGS) -c http_parse.c
```

### req_headers_t

- `host`: `char[MAXLINE]` — `Host` 헤더 값이에요. 앞뒤 공백을 제거한 문자열로 저장해요. (예: `"localhost:8000"`)
- `has_host`: `int` — `Host` 헤더를 이미 읽었는지 나타내요. 읽었으면 `1`, 아니면 `0`이에요. 값이 `1`인데 `Host`가 또 오면 중복이라 400이에요.
- `content_length`: `long` — `Content-Length` 헤더 값이에요. 0 이상의 정수로 저장해요. (예: `14`) `has_content_length`가 `0`이면 의미가 없어요.
- `has_content_length`: `int` — `Content-Length` 헤더가 있었는지 나타내요. 있으면 `1`, 없으면 `0`이에요. POST에서 `0`이면 411이에요.

### 함수 구현 (http_parse.c)

```c
/* http_parse.c */
#include <ctype.h>
#include "http_parse.h"

char *trim(char *s)
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

int parse_content_length(const char *v, long *out)
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

- **헤더의 끝**: 빈 줄(`\r\n`)이 나오면 헤더가 끝나요. 빈 줄이 오기 전에 연결이 끊기면(EOF) 불완전한 요청이라 400이에요.
- **너무 긴 줄**: `Rio_readlineb`는 줄이 `MAXLINE`보다 길면 `\n` 없이 잘라서 돌려줘요. 그래서 읽은 줄이 `\n`으로 끝나는지 꼭 확인해야 해요. 안 그러면 줄의 나머지가 다음 헤더로 읽혀요.
- **콜론 위치**: 첫 번째 `:`를 기준으로 나눠요. `:`가 없거나, 이름이 비었거나, 이름과 `:` 사이에 공백이 있으면(`Host : x`) 400이에요.
- **이름은 대소문자 구분 없음**: `host`, `HOST`도 같은 헤더예요. `strcasecmp`로 비교해요.
- **값 앞뒤 공백 제거**: `Host:  localhost  ` → `localhost`
- **Content-Length**: 숫자로만 이루어져야 하고(음수·공백·문자 불가), 여러 번 나오면 값이 모두 같아야 해요.
## 3. 유효성 검사 + body 읽기

### clienterror 시그니처 변경

405 응답에는 `Allow` 헤더가 필요해서 `clienterror`에 `allow` 인자를 하나 추가해요. 다른 에러에서는 마지막 인자로 `NULL`을 넘겨요.

```c
void clienterror(int fd, char *cause, char *errnum,
                 char *shortmsg, char *longmsg, char *allow);
```

```c
sprintf(buf, "HTTP/1.0 %s %s\r\n", errnum, shortmsg);
Rio_writen(fd, buf, strlen(buf));
if (allow != NULL) {                       /* 405일 때만 "GET" 전달 */
    sprintf(buf, "Allow: %s\r\n", allow);
    Rio_writen(fd, buf, strlen(buf));
}
```

### 검사 코드 (doit 안)

```c
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

long body_len = hdrs.has_content_length ? hdrs.content_length : 0;

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
```

## 4. CGI로 body 전달

### 환경 변수

| 항목 | GET | POST |
| --- | --- | --- |
| 데이터 위치 | URL의 `?` 뒤 (`/cgi-bin/adder?num1=3&num2=16`) | 요청 body (`num1=3&num2=16`) |
| `REQUEST_METHOD` | `"GET"` | `"POST"` |
| `QUERY_STRING` | `?` 뒤 문자열 (`"num1=3&num2=16"`) | `""` (빈 문자열). URL에 `?`가 있으면 그 값 |
| `CONTENT_LENGTH` | 설정하지 않음 (body가 없으니까) | body 바이트 수 (`"14"`) |
| `CONTENT_TYPE` | 설정하지 않음 | 요청 헤더 값 그대로 (`"application/x-www-form-urlencoded"`). 선택 항목, 현재 미구현 |

### serve_dynamic

body는 pipe로 CGI 자식의 stdin에 넣어 줘요. 부모가 body를 쓰고 쓰기 끝을 닫으면 자식은 EOF를 받아요.

```c
void serve_dynamic(int fd, char *filename, char *cgiargs,
                   char *method, char *body, long body_len)
{
    int p[2];
    char buf[MAXLINE], lenbuf[32];
    char *emptylist[] = { NULL };

    /* pipe를 먼저 만들어요. 상태줄(200)을 보낸 뒤에 실패하면 500을 보낼 수 없어요. */
    if (pipe(p) < 0) {
        clienterror(fd, filename, "500", "Internal Server Error",
                    "Failed to create pipe", NULL);
        return;
    }

    /* 상태줄 + Server 헤더 전송 (기존 코드) */
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
        rio_writen(p[1], body, body_len);    /* 실패 시 서버가 죽지 않게 소문자 버전 사용 */
    close(p[1]);                             /* 자식에게 EOF 전달 */
    Wait(NULL);
}
```

- **SIGPIPE 처리**: CGI가 body를 다 읽기 전에 끝나면 `rio_writen`이 SIGPIPE를 받아 서버 프로세스가 죽어요. `main`에서 무시하도록 설정해요.
```c
int main(int argc, char **argv)
{
    Signal(SIGPIPE, SIG_IGN);
    ...
}
```

## 5. CGI 프로그램(adder) 수정

adder는 `REQUEST_METHOD`를 보고 데이터를 가져올 곳을 정해요.

- **POST**: `CONTENT_LENGTH` 환경 변수에서 길이를 가져오고, 그 길이만큼 stdin에서 body를 읽어요.
- **GET**: `QUERY_STRING` 환경 변수에서 읽어요.
두 경우 모두 결과를 `buf`에 담고, 그 뒤의 `num1`, `num2` 파싱은 공통으로 처리해요.

서버는 메서드를 대소문자 구분 없이 받고, 클라이언트가 보낸 그대로 `REQUEST_METHOD`에 넣어요. 그래서 adder도 `strcasecmp`로 비교해요. `strcmp`로 비교하면 `post`로 온 요청을 GET으로 보고 `QUERY_STRING`을 읽어요.

```c
char buf[MAXBODY + 1];    /* buf[len] = '\0'를 위해 +1 */
char *method = getenv("REQUEST_METHOD");
char *end;
long len;

if (method && !strcasecmp(method, "POST")) {   /* "post"도 POST로 처리 */
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

### num1, num2 파싱

POST body(`num1=3&num2=16`)와 `QUERY_STRING`은 형식이 같아서, 원래 adder의 파싱 방식을 그대로 써요.
다만 원래 코드는 `&`나 `=`가 없으면 NULL 포인터를 건드려 adder가 죽어요. body는 클라이언트가 마음대로 보낼 수 있으니(예: `curl -d "hello"`), 파싱 전에 검사를 추가해요.
GET도 `/cgi-bin/adder`처럼 `?` 없이 요청하면 `QUERY_STRING`이 빈 문자열이라 같은 검사가 필요해요.

```c
char *amp = strchr(buf, '&');
char *eq1 = strchr(buf, '=');
char *eq2 = amp ? strchr(amp + 1, '=') : NULL;

if (amp == NULL || eq1 == NULL || eq1 > amp || eq2 == NULL) {
    /* 에러 응답 (예: "Invalid arguments") */
}
*amp = '\0';
n1 = atoi(eq1 + 1);
n2 = atoi(eq2 + 1);
```

- **인자 순서:** 원래 방식처럼 첫 번째 값을 `num1`, 두 번째 값을 `num2`로 봐요. 인자 이름은 확인하지 않아요.
- **브라우저 테스트:** `tiny/form.html`은 `method="GET"`이라 POST를 보낼 수 없어요. POST는 `curl -d "num1=3&num2=16" http://localhost:8000/cgi-bin/adder`로 확인해요.

## 6. 요청 보내는 방법

서버는 `./tiny 8000`으로 실행했다고 가정해요.

POST 요청은 `curl -d`로 보내요. `-d`를 쓰면 curl이 메서드를 POST로 바꾸고 `Content-Length`와 `Content-Type: application/x-www-form-urlencoded`를 자동으로 붙여요.

```bash
curl -i -d "num1=3&num2=16" http://localhost:8000/cgi-bin/adder
```

실제로 서버에 가는 요청은 아래와 같아요. 헤더와 body 사이에는 빈 줄이 하나 있어요.

```http
POST /cgi-bin/adder HTTP/1.1
Host: localhost:8000
Content-Type: application/x-www-form-urlencoded
Content-Length: 14

num1=3&num2=16
```

헤더를 빼거나 값을 틀리게 보내야 할 때는 `nc`로 요청 원문을 직접 보내요. 줄 끝은 `\r\n`으로 써요.

```bash
printf 'POST /cgi-bin/adder HTTP/1.0\r\nHost: localhost:8000\r\nContent-Length: 14\r\n\r\nnum1=3&num2=16' | nc -N localhost 8000
```

`-N`은 입력이 끝나면 쓰기 방향을 닫는 옵션이에요(Linux 기준).
