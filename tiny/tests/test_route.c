/*
 * Phase 4 라우팅 단위 테스트
 * 케이스: route_match(경계, 가장 긴 접두사, 매칭 없음), route_pick, route_rewrite_path
 *
 * 빌드: make test-route (route.o 링크)
 */
#include <stdio.h>
#include <string.h>
#include "route.h"

#define MAXPATH 256

static int failures = 0;
static int passes = 0;

#define CHECK(id, cond, ...)                                  \
    do {                                                      \
        if (cond) {                                           \
            passes++;                                         \
            fprintf(stderr, "[PASS] %s\n", id);               \
        } else {                                              \
            failures++;                                       \
            fprintf(stderr, "[FAIL] %s: ", id);               \
            fprintf(stderr, __VA_ARGS__);                     \
            fprintf(stderr, "\n");                            \
        }                                                     \
    } while (0)

/* ---------- 테스트용 라우트 테이블 ---------- */

/* 규칙 순서를 일부러 섞어 둔다: 짧은 /가 먼저 나와도 긴 접두사가 이겨야 한다 */
static void add_route(route_table_t *t, const char *prefix)
{
    route_t *r = &t->r[t->n++];
    memset(r, 0, sizeof(*r));
    strcpy(r->prefix, prefix);
}

static void build_table(route_table_t *t, int with_root)
{
    memset(t, 0, sizeof(*t));
    if (with_root)
        add_route(t, "/");
    add_route(t, "/api");
    add_route(t, "/img/");
    add_route(t, "/api/v2/");
}

/* expected가 NULL이면 매칭 없음(NULL 반환)을 기대 */
static void match_case(const char *id, route_table_t *t,
                       const char *uri, const char *expected)
{
    route_t *r = route_match(t, uri);
    const char *got = r ? r->prefix : NULL;
    int ok = (expected == NULL) ? (got == NULL)
                                : (got != NULL && strcmp(got, expected) == 0);
    CHECK(id, ok, "uri=\"%s\" got=%s (expected %s)",
          uri, got ? got : "NULL", expected ? expected : "NULL");
}

/* ---------- 경계 검사 ---------- */

static void test_boundary(void)
{
    route_table_t t;
    build_table(&t, 0);

    match_case("TC-RT-01", &t, "/api",        "/api");   /* 정확히 일치 */
    match_case("TC-RT-02", &t, "/api/users",  "/api");   /* 다음 글자 / */
    match_case("TC-RT-03", &t, "/api?q=1",    "/api");   /* 다음 글자 ? */
    match_case("TC-RT-04", &t, "/apix",       NULL);     /* 경로 조각 중간 */
    match_case("TC-RT-05", &t, "/api-docs/a", NULL);
    match_case("TC-RT-06", &t, "/img/cat.png","/img/");  /* /로 끝나는 규칙 */
    match_case("TC-RT-07", &t, "/img",        NULL);     /* /img/ 규칙은 /img를 안 잡음 */
}

/* ---------- 가장 긴 접두사 ---------- */

static void test_longest(void)
{
    route_table_t t;
    build_table(&t, 1);

    match_case("TC-RT-08", &t, "/api/v2/users", "/api/v2/");
    match_case("TC-RT-09", &t, "/api/v1/users", "/api");
    match_case("TC-RT-10", &t, "/api/v2",       "/api");  /* 끝 / 없음 → /api/v2/ 불일치 */
    match_case("TC-RT-11", &t, "/apix",         "/");     /* /api 탈락 후 / 규칙 */
    match_case("TC-RT-12", &t, "/home.html",    "/");
}

/* ---------- 매칭 없음 ---------- */

static void test_no_match(void)
{
    route_table_t t;
    build_table(&t, 0);
    match_case("TC-RT-13", &t, "/home.html", NULL);       /* tiny가 직접 처리 */
    match_case("TC-RT-14", &t, "/",          NULL);

    memset(&t, 0, sizeof(t));                             /* 규칙 0개 */
    match_case("TC-RT-15", &t, "/api/users", NULL);
}

/* ---------- route_pick (라운드로빈) ---------- */

static void test_pick(void)
{
    route_t r;
    int a, b, c;

    memset(&r, 0, sizeof(r));
    r.nbe = 2;
    a = route_pick(&r);
    b = route_pick(&r);
    c = route_pick(&r);
    CHECK("TC-RR-01", a == 0 && b == 1 && c == 0,
          "nbe=2 got %d,%d,%d (expected 0,1,0)", a, b, c);

    memset(&r, 0, sizeof(r));
    r.nbe = 1;
    a = route_pick(&r);
    b = route_pick(&r);
    CHECK("TC-RR-02", a == 0 && b == 0,
          "nbe=1 got %d,%d (expected 0,0)", a, b);

    memset(&r, 0, sizeof(r));
    r.nbe = 3;
    a = route_pick(&r);
    b = route_pick(&r);
    c = route_pick(&r);
    CHECK("TC-RR-03", a == 0 && b == 1 && c == 2 && r.rr == 0,
          "nbe=3 got %d,%d,%d rr=%d (expected 0,1,2 rr=0)", a, b, c, r.rr);
}

/* ---------- route_rewrite_path ---------- */

static void rewrite_case(const char *id, const char *prefix, int strip,
                         const char *uri, const char *expected)
{
    route_t r;
    char out[MAXPATH];
    int rc;

    memset(&r, 0, sizeof(r));
    strcpy(r.prefix, prefix);
    r.strip = strip;
    rc = route_rewrite_path(&r, uri, out, sizeof(out));
    CHECK(id, rc == 0 && strcmp(out, expected) == 0,
          "prefix=\"%s\" strip=%d uri=\"%s\" rc=%d out=\"%s\" (expected \"%s\")",
          prefix, strip, uri, rc, rc == 0 ? out : "", expected);
}

static void test_rewrite(void)
{
    route_t r;
    char small[5];
    int rc;

    rewrite_case("TC-RW-01", "/img/", 1, "/img/cat.png", "/cat.png");
    rewrite_case("TC-RW-02", "/img/", 1, "/img/",        "/");       /* 빈 경로 → / */
    rewrite_case("TC-RW-03", "/img/", 1, "/img/?a=1",    "/?a=1");   /* 쿼리만 남음 */
    rewrite_case("TC-RW-04", "/api",  1, "/api/users",   "/users");
    rewrite_case("TC-RW-05", "/api",  1, "/api?q=1",     "/?q=1");
    rewrite_case("TC-RW-06", "/api",  0, "/api/users",   "/api/users"); /* strip 꺼짐 */

    /* 버퍼가 작으면 -1 */
    memset(&r, 0, sizeof(r));
    strcpy(r.prefix, "/api");
    rc = route_rewrite_path(&r, "/api/users", small, sizeof(small));
    CHECK("TC-RW-07", rc == -1, "small buffer rc=%d (expected -1)", rc);
}

int main(void)
{
    test_boundary();
    test_longest();
    test_no_match();
    test_pick();
    test_rewrite();

    fprintf(stderr, "\nroute: %d passed, %d failed\n", passes, failures);
    return failures ? 1 : 0;
}