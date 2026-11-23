#include "csapp.h"

int main(void)
{
    char *buf, *p;
    char arg1[MAXLINE], arg2[MAXLINE], content[MAXLINE];
    int n1=0, n2=0;

    if ((buf = getenv("QUERY_STRING")) != NULL) {
        p = strchr(buf, '&');
        *p = '\0';
        strcpy(arg1, buf);
        strcpy(arg2, p+1);
        n1 = atoi(strchr(arg1, '=') + 1);
        n2 = atoi(strchr(arg2, '=') + 1);
    }

    int len = 0;
    len += sprintf(content + len, "Welcome to add.com: ");
    len += sprintf(content + len, "THE Internet addition portal.\r\n<p>");
    len += sprintf(content + len, "The answer is: %d + %d = %d\r\n<p>", n1, n2, n1 + n2);
    len += sprintf(content + len, "Thanks for visiting!\r\n");

    printf("Connection: close\r\n");
    printf("Content-length: %d\r\n", len);
    printf("Content-type: text/html\r\n\r\n");
    printf("%s", content);
    fflush(stdout);
    exit(0);
}
