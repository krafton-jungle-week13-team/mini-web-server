#ifndef ROUTE_H
#define ROUTE_H

#define MAX_ROUTES   16
#define MAX_BACKENDS 8

#include <stddef.h>

typedef struct { char host[64], port[8]; } backend_t;

typedef struct {
    char prefix[128];
    int strip;
    backend_t be[MAX_BACKENDS];
    int nbe;
    int rr;
} route_t;

typedef struct { route_t r[MAX_ROUTES]; int n; } route_table_t;

route_t *route_match(route_table_t *t, const char *uri);

int route_pick(route_t *r);

int route_rewrite_path(const route_t *r, const char *uri, char *out, size_t n);

int route_load(const char *path, route_table_t *t);      /* 성공 0, 실패 -1 */

void route_print(const route_table_t *t);

#endif