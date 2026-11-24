#include "csapp.h"

/* HTTP 응답 헤더와 본문을 출력하고 정상 종료한다. */
static void respond(const char *content)
{
    printf("Connection: close\r\n");
    printf("Content-length: %d\r\n", (int)strlen(content));
    printf("Content-type: text/html; charset=utf-8\r\n\r\n");
    printf("%s", content);
    fflush(stdout);
    exit(0);
}

static void error_response(const char *msg)
{
    char content[MAXLINE];

    snprintf(content, sizeof(content),
             "Welcome to add.com: THE Internet addition portal.\r\n<p>"
             "Invalid arguments: %s\r\n", msg);
    respond(content);
}

int main(void)
{
    char buf[MAXBODY + 1];    /* buf[len] = '\0'를 위해 +1 */
    char content[MAXLINE];
    char *method = getenv("REQUEST_METHOD");
    char *end, *amp, *eq1, *eq2;
    long len;
    int n1, n2;

    if (method && !strcasecmp(method, "POST")) {
        /* POST: CONTENT_LENGTH만큼 stdin에서 body를 읽는다 */
        char *lenstr = getenv("CONTENT_LENGTH");
        if (lenstr == NULL || *lenstr == '\0')
            error_response("missing CONTENT_LENGTH");
        len = strtol(lenstr, &end, 10);
        if (*end != '\0' || len < 0 || len > MAXBODY)
            error_response("bad CONTENT_LENGTH");
        if (len > 0 && rio_readn(STDIN_FILENO, buf, len) != len)
            error_response("body is shorter than CONTENT_LENGTH");
        buf[len] = '\0';
    } else {
        /* GET: QUERY_STRING에서 읽는다 */
        char *qs = getenv("QUERY_STRING");
        if (qs == NULL)
            error_response("missing QUERY_STRING");
        strncpy(buf, qs, MAXBODY);
        buf[MAXBODY] = '\0';
    }

    /* num1=3&num2=16 형식 검사 후 파싱 (POST, GET 공통) */
    amp = strchr(buf, '&');
    eq1 = strchr(buf, '=');
    eq2 = amp ? strchr(amp + 1, '=') : NULL;
    if (amp == NULL || eq1 == NULL || eq1 > amp || eq2 == NULL)
        error_response("expected num1=<n>&num2=<n>");
    *amp = '\0';
    n1 = atoi(eq1 + 1);
    n2 = atoi(eq2 + 1);

    snprintf(content, sizeof(content),
             "Welcome to add.com: THE Internet addition portal.\r\n<p>"
             "The answer is: %d + %d = %d\r\n<p>"
             "Thanks for visiting!\r\n", n1, n2, n1 + n2);
    respond(content);
    return 0;
}
