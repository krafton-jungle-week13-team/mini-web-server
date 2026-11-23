/*
 * Phase 3 헤더 파서 단위 테스트
 * 케이스: docs/test/phase3/test-cases.md 1장 (TC-CL, TC-TRIM, TC-HDR)
 *
 * 빌드: make test-unit (http_parse.o, csapp.o 링크)
 */
#include "csapp.h"
#include "http_parse.h"

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

/* ---------- parse_content_length ---------- */

static void cl_ok(const char *id, const char *v, long expected)
{
    long out = -12345;
    int rc = parse_content_length(v, &out);
    CHECK(id, rc == 0 && out == expected,
          "input=\"%s\" rc=%d out=%ld (expected rc=0 out=%ld)", v, rc, out, expected);
}

static void cl_err(const char *id, const char *v)
{
    long out = -12345;
    int rc = parse_content_length(v, &out);
    CHECK(id, rc == -1, "input=\"%s\" rc=%d (expected -1)", v, rc);
}

static void test_parse_content_length(void)
{
    cl_ok("TC-CL-01", "14", 14);
    cl_ok("TC-CL-02", "0", 0);
    cl_err("TC-CL-03", "");
    cl_err("TC-CL-04", "-1");
    cl_err("TC-CL-05", "+14");
    cl_err("TC-CL-06", " 14");
    cl_err("TC-CL-07", "14abc");
    cl_err("TC-CL-08", "99999999999999999999");
}

/* ---------- trim ---------- */

static void trim_case(const char *id, const char *input, const char *expected)
{
    char buf[64];
    char *r;

    snprintf(buf, sizeof(buf), "%s", input);   /* trim은 문자열을 고치므로 복사본 사용 */
    r = trim(buf);
    CHECK(id, r != NULL && strcmp(r, expected) == 0,
          "got=\"%s\" (expected \"%s\")", r ? r : "(null)", expected);
}

static void test_trim(void)
{
    trim_case("TC-TRIM-01", "localhost", "localhost");
    trim_case("TC-TRIM-02", "  localhost  ", "localhost");
    trim_case("TC-TRIM-03", "\tlocalhost\t", "localhost");
    trim_case("TC-TRIM-04", " localhost\r\n", "localhost");
    trim_case("TC-TRIM-05", "", "");
    trim_case("TC-TRIM-06", "  \r\n", "");
    trim_case("TC-TRIM-07", "local host", "local host");
}

/* ---------- read_requesthdrs ---------- */

/* raw를 pipe에 쓰고 쓰기 끝을 닫은 뒤, 읽기 끝을 rio_t로 읽힌다. */
static int run_hdrs(const char *raw, size_t rawlen, req_headers_t *h)
{
    int p[2], rc, saved_stdout;
    rio_t rio;

    if (pipe(p) < 0) {
        perror("pipe");
        exit(2);
    }
    if (rio_writen(p[1], (void *)raw, rawlen) != (ssize_t)rawlen) {
        perror("write");
        exit(2);
    }
    close(p[1]);

    /* read_requesthdrs가 헤더를 printf하므로 테스트 출력이 섞이지 않게 stdout을 잠시 막는다 */
    fflush(stdout);
    saved_stdout = dup(STDOUT_FILENO);
    int devnull = open("/dev/null", O_WRONLY);
    dup2(devnull, STDOUT_FILENO);
    close(devnull);

    memset(h, 0xAB, sizeof(*h));   /* 함수가 h를 초기화하는지도 함께 확인 */
    Rio_readinitb(&rio, p[0]);
    rc = read_requesthdrs(&rio, h);

    fflush(stdout);
    dup2(saved_stdout, STDOUT_FILENO);
    close(saved_stdout);
    close(p[0]);
    return rc;
}

static void hdr_err(const char *id, const char *raw)
{
    req_headers_t h;
    int rc = run_hdrs(raw, strlen(raw), &h);
    CHECK(id, rc == -1, "rc=%d (expected -1)", rc);
}

static void test_read_requesthdrs(void)
{
    req_headers_t h;
    int rc;

    rc = run_hdrs("Host: localhost\r\nContent-Length: 14\r\n\r\n",
                  strlen("Host: localhost\r\nContent-Length: 14\r\n\r\n"), &h);
    CHECK("TC-HDR-01", rc == 0 && h.has_host == 1 && !strcmp(h.host, "localhost") &&
                       h.has_content_length == 1 && h.content_length == 14,
          "rc=%d has_host=%d host=\"%.32s\" has_cl=%d cl=%ld",
          rc, h.has_host, h.host, h.has_content_length, h.content_length);

    rc = run_hdrs("host: localhost\r\ncontent-LENGTH: 14\r\n\r\n",
                  strlen("host: localhost\r\ncontent-LENGTH: 14\r\n\r\n"), &h);
    CHECK("TC-HDR-02", rc == 0 && h.has_host == 1 && !strcmp(h.host, "localhost") &&
                       h.has_content_length == 1 && h.content_length == 14,
          "rc=%d has_host=%d host=\"%.32s\" has_cl=%d cl=%ld",
          rc, h.has_host, h.host, h.has_content_length, h.content_length);

    rc = run_hdrs("Host:   localhost  \r\n\r\n", strlen("Host:   localhost  \r\n\r\n"), &h);
    CHECK("TC-HDR-03", rc == 0 && h.has_host == 1 && !strcmp(h.host, "localhost"),
          "rc=%d host=\"%.32s\"", rc, h.host);

    rc = run_hdrs("\r\n", 2, &h);
    CHECK("TC-HDR-04", rc == 0 && h.has_host == 0 && h.has_content_length == 0,
          "rc=%d has_host=%d has_cl=%d", rc, h.has_host, h.has_content_length);

    rc = run_hdrs("Accept: */*\r\n\r\n", strlen("Accept: */*\r\n\r\n"), &h);
    CHECK("TC-HDR-05", rc == 0 && h.has_host == 0 && h.has_content_length == 0,
          "rc=%d has_host=%d has_cl=%d", rc, h.has_host, h.has_content_length);

    hdr_err("TC-HDR-06", "Host localhost\r\n\r\n");
    hdr_err("TC-HDR-07", ": localhost\r\n\r\n");
    hdr_err("TC-HDR-08", "Host : localhost\r\n\r\n");
    hdr_err("TC-HDR-09", "Host: a\r\nHost: b\r\n\r\n");
    hdr_err("TC-HDR-10", "Content-Length: 14\r\nContent-Length: 20\r\n\r\n");

    rc = run_hdrs("Content-Length: 14\r\nContent-Length: 14\r\n\r\n",
                  strlen("Content-Length: 14\r\nContent-Length: 14\r\n\r\n"), &h);
    CHECK("TC-HDR-11", rc == 0 && h.has_content_length == 1 && h.content_length == 14,
          "rc=%d has_cl=%d cl=%ld", rc, h.has_content_length, h.content_length);

    hdr_err("TC-HDR-12", "Content-Length: abc\r\n\r\n");
    hdr_err("TC-HDR-13", "Host: localhost\r\n");

    /* TC-HDR-14: "X-Long: " + 'a' * MAXLINE + "\r\n\r\n" */
    {
        const char *prefix = "X-Long: ";
        size_t plen = strlen(prefix);
        size_t total = plen + MAXLINE + 4;
        char *raw = malloc(total + 1);

        memcpy(raw, prefix, plen);
        memset(raw + plen, 'a', MAXLINE);
        memcpy(raw + plen + MAXLINE, "\r\n\r\n", 4);
        raw[total] = '\0';
        rc = run_hdrs(raw, total, &h);
        CHECK("TC-HDR-14", rc == -1, "rc=%d (expected -1)", rc);
        free(raw);
    }
}

int main(void)
{
    test_parse_content_length();
    test_trim();
    test_read_requesthdrs();

    fprintf(stderr, "\nheader parser: %d passed, %d failed\n", passes, failures);
    return failures ? 1 : 0;
}
