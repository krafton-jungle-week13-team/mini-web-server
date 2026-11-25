#include <stdio.h>
#include <string.h>
#include <stdlib.h>
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


/* "127.0.0.1:8001" → host, port. 포트는 1~65535 숫자만 */
static int parse_backend(const char *s, backend_t *be)
{
    const char *colon = strrchr(s, ':');
    size_t hlen;
    char *end;
    long port;

    if (colon == NULL || colon == s)
        return -1;
    hlen = colon - s;
    if (hlen >= sizeof(be->host))
        return -1;
    port = strtol(colon + 1, &end, 10);
    if (colon[1] == '\0' || *end != '\0' || port < 1 || port > 65535)
        return -1;
    memcpy(be->host, s, hlen);
    be->host[hlen] = '\0';
    snprintf(be->port, sizeof(be->port), "%ld", port);
    return 0;
}

/* 한 줄을 규칙 하나로. 성공 0, 실패 -1 */
static int parse_line(char *line, route_table_t *t, int lineno)
{
    char *save, *prefix, *strip, *tok;
    route_t *r;

    prefix = strtok_r(line, " \t\r\n", &save);
    if (prefix == NULL || prefix[0] == '#')
        return 0;                                 /* 빈 줄, 주석 */

    if (prefix[0] != '/' || strlen(prefix) >= sizeof(r->prefix)) {
        fprintf(stderr, "routes:%d: prefix는 /로 시작하고 127자 이하여야 함\n", lineno);
        return -1;
    }
    for (int i = 0; i < t->n; i++) {
        if (!strcmp(t->r[i].prefix, prefix)) {
            fprintf(stderr, "routes:%d: 중복된 prefix %s\n", lineno, prefix);
            return -1;
        }
    }
    if (t->n >= MAX_ROUTES) {
        fprintf(stderr, "routes:%d: 규칙은 최대 %d개\n", lineno, MAX_ROUTES);
        return -1;
    }

    r = &t->r[t->n];
    memset(r, 0, sizeof(*r));
    strcpy(r->prefix, prefix);

    strip = strtok_r(NULL, " \t\r\n", &save);
    if (strip == NULL || (strcmp(strip, "yes") && strcmp(strip, "no"))) {
        fprintf(stderr, "routes:%d: strip은 yes 또는 no\n", lineno);
        return -1;
    }
    r->strip = !strcmp(strip, "yes");

    while ((tok = strtok_r(NULL, " \t\r\n", &save)) != NULL) {
        if (tok[0] == '#')
            break;                                /* 줄 끝 주석 */
        if (r->nbe >= MAX_BACKENDS) {
            fprintf(stderr, "routes:%d: 백엔드는 최대 %d개\n", lineno, MAX_BACKENDS);
            return -1;
        }
        if (parse_backend(tok, &r->be[r->nbe]) < 0) {
            fprintf(stderr, "routes:%d: 잘못된 백엔드 \"%s\" (host:port)\n", lineno, tok);
            return -1;
        }
        r->nbe++;
    }
    if (r->nbe == 0) {
        fprintf(stderr, "routes:%d: 백엔드가 없음\n", lineno);
        return -1;
    }
    t->n++;
    return 0;
}

int route_load(const char *path, route_table_t *t)
{
    FILE *fp;
    char line[1024];
    int lineno = 0, rc = 0;

    memset(t, 0, sizeof(*t));
    if ((fp = fopen(path, "r")) == NULL) {
        perror(path);
        return -1;
    }
    while (fgets(line, sizeof(line), fp) != NULL) {
        lineno++;
        if (strchr(line, '\n') == NULL && !feof(fp)) {
            fprintf(stderr, "routes:%d: 줄이 너무 김\n", lineno);
            rc = -1;
            break;
        }
        if (parse_line(line, t, lineno) < 0) {
            rc = -1;
            break;
        }
    }
    fclose(fp);
    return rc;
}

void route_print(const route_table_t *t)
{
    for (int i = 0; i < t->n; i++) {
        const route_t *r = &t->r[i];
        printf("route %-12s strip=%-3s ->", r->prefix, r->strip ? "yes" : "no");
        for (int j = 0; j < r->nbe; j++)
            printf(" %s:%s", r->be[j].host, r->be[j].port);
        printf("\n");
    }
}