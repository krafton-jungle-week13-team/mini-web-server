#ifndef HTTP_PARSE_H
#define HTTP_PARSE_H

#include "csapp.h"

typedef struct {
    char host[MAXLINE];       /* Host 헤더 값 (앞뒤 공백 제거) */
    int has_host;             /* Host 헤더를 읽었으면 1 */
    long content_length;      /* Content-Length 값 (0 이상) */
    int has_content_length;   /* Content-Length 헤더가 있었으면 1 */
    char raw[MAXBUF];         /* 읽은 헤더 줄 원문 (빈 줄 제외). 프록시가 백엔드로 넘길 때 씀 */
    size_t raw_len;           /* raw에 담긴 바이트 수 */
    int raw_overflow;         /* 헤더가 MAXBUF를 넘어 raw에 다 담지 못했으면 1 */
} req_headers_t;

char *trim(char *s);
int parse_content_length(const char *v, long *out);   /* 성공 0, 실패 -1 */
int read_requesthdrs(rio_t *rp, req_headers_t *h);    /* 성공 0, 실패 -1 → 400 */

#endif
