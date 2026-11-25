#ifndef HTTP_PARSE_H
#define HTTP_PARSE_H

#include "csapp.h"

typedef struct {
    char host[MAXLINE];       /* Host 헤더 값 (앞뒤 공백 제거) */
    int has_host;             /* Host 헤더를 읽었으면 1 */
    long content_length;      /* Content-Length 값 (0 이상) */
    int has_content_length;   /* Content-Length 헤더가 있었으면 1 */
} req_headers_t;

char *trim(char *s);
int parse_content_length(const char *v, long *out);   /* 성공 0, 실패 -1 */
int read_requesthdrs(rio_t *rp, req_headers_t *h);    /* 성공 0, 실패 -1 → 400 */
int parse_header_line(char *buf, req_headers_t *h);

#endif
