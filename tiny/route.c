#include <stdio.h>
#include <string.h>
#include "route.h"

/* uri가 prefix로 시작하고, 경로 경계에서 끝나면 1 */
static int prefix_matches(const char *prefix, const char *uri)
{
    size_t len = strlen(prefix);

    if (strncmp(uri, prefix, len) != 0)
        return 0;                       /* 앞부분부터 다름 */
    if (len > 0 && prefix[len - 1] == '/')
        return 1;                       /* "/api/"처럼 /로 끝나면 이미 경계 */

    char next = uri[len];               /* 접두사 바로 다음 글자 */
    return next == '\0' || next == '/' || next == '?';
}

route_t *route_match(route_table_t *t, const char *uri)
{
    route_t *best = NULL;
    size_t best_len = 0;

    for (int i = 0; i < t->n; i++) {
        size_t len = strlen(t->r[i].prefix);
        if (prefix_matches(t->r[i].prefix, uri) &&
            (best == NULL || len > best_len)) {
            best = &t->r[i];
            best_len = len;
        }
    }
    return best;                        /* 없으면 NULL → 기존 정적/CGI 처리 */
}

/* 라운드로빈: 이번 차례 인덱스를 돌려주고 다음 차례로 넘긴다 */
int route_pick(route_t *r)
{
    int i = r->rr;
    r->rr = (r->rr + 1) % r->nbe;
    return i;
}

/* 백엔드로 보낼 경로를 out에 만든다. 버퍼가 작으면 -1 */
int route_rewrite_path(const route_t *r, const char *uri, char *out, size_t n)
{
    const char *rest = uri;

    if (r->strip)
        rest = uri + strlen(r->prefix);          /* 접두사 뒤부터 */
    if (r->strip && rest[0] != '/') {
        if (snprintf(out, n, "/%s", rest) >= (int)n)
            return -1;                           /* 빈 문자열이나 ?로 시작하면 / 붙임 */
    } else {
        if (snprintf(out, n, "%s", rest) >= (int)n)
            return -1;
    }
    return 0;
}