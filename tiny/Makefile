CC = gcc
CFLAGS = -O2 -Wall -I .

LIB = -lpthread

all: tiny cgi

tiny: tiny.c csapp.o http_parse.o sbuf.o route.o
	$(CC) $(CFLAGS) -o tiny tiny.c csapp.o http_parse.o sbuf.o route.o $(LIB)

csapp.o: csapp.c
	$(CC) $(CFLAGS) -c csapp.c

sbuf.o: sbuf.c sbuf.h
	$(CC) $(CFLAGS) -c sbuf.c

http_parse.o: http_parse.c http_parse.h
	$(CC) $(CFLAGS) -c http_parse.c

route.o: route.c route.h
	$(CC) $(CFLAGS) -c route.c

cgi:
	(cd cgi-bin; make)

# ---------- Phase 3 테스트 (docs/test/phase3/test-cases.md) ----------
test: test-unit test-route test-adder test-server test-proxy

tests/test_http_parse: tests/test_http_parse.c http_parse.o csapp.o
	$(CC) $(CFLAGS) -o $@ tests/test_http_parse.c http_parse.o csapp.o $(LIB)

test-unit: tests/test_http_parse
	./tests/test_http_parse

test-adder: cgi
	bash tests/test_adder.sh

test-server: tiny cgi
	bash tests/test_server.sh

tests/test_route: tests/test_route.c route.o
	$(CC) $(CFLAGS) -o $@ tests/test_route.c route.o

test-route: tests/test_route
	./tests/test_route

# Phase 5 리버스 프록시 (docs/design/phase5/reverse-proxy.md)
test-proxy: tiny cgi
	bash tests/test_proxy.sh

.PHONY: all cgi clean test test-unit test-route test-adder test-server test-proxy

clean:
	rm -f *.o tiny *~ tests/test_http_parse tests/test_route
	(cd cgi-bin; make clean)
