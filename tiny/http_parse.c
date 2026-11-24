#include <ctype.h>
#include "http_parse.h"

/* 앞의 공백·탭, 뒤의 공백·탭·\r·\n을 지운 문자열의 시작 주소를 돌려준다. */
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

/* 숫자로만 이루어진 0 이상의 값만 허용한다 (부호·공백·문자·범위 초과는 실패). */
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

int parse_header_line(char *buf, req_headers_t *h)
{
    char *colon, *name, *value;
    ssize_t n;
    long len;

    if (n == 0 || buf[n - 1] != '\n')          /* MAXLINE보다 긴 줄은 잘려서 \n이 없다 */
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
    return 0;
}

/* 빈 줄까지 헤더를 읽어 h에 채운다. 형식 오류나 빈 줄 전 EOF면 -1. */
int read_requesthdrs(rio_t *rp, req_headers_t *h)
{
    char buf[MAXLINE];
    memset(h, 0, sizeof(*h));
    while (Rio_readlineb(rp, buf, MAXLINE) > 0) {
        if (!strcmp(buf, "\r\n"))
            return 0;
        if (parse_header_line(buf, h) < 0)
            return -1;
    }
    return -1;                           /* 빈 줄 전에 EOF */
}
