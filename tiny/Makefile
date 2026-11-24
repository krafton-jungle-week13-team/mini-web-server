CC = gcc
CFLAGS = -O2 -Wall -I .

LIB = -lpthread

all: tiny cgi

tiny: tiny.c csapp.o http_parse.o route.o
	$(CC) $(CFLAGS) -o tiny tiny.c csapp.o http_parse.o route.o $(LIB)

csapp.o: csapp.c
	$(CC) $(CFLAGS) -c csapp.c

http_parse.o: http_parse.c http_parse.h
	$(CC) $(CFLAGS) -c http_parse.c

cgi:
	(cd cgi-bin; make)

# ---------- Phase 3 테스트 (docs/test/phase3/test-cases.md) ----------
test: test-unit test-adder test-server

tests/test_http_parse: tests/test_http_parse.c http_parse.o csapp.o
	$(CC) $(CFLAGS) -o $@ tests/test_http_parse.c http_parse.o csapp.o $(LIB)

test-unit: tests/test_http_parse
	./tests/test_http_parse

test-adder: cgi
	bash tests/test_adder.sh

test-server: tiny cgi
	bash tests/test_server.sh

route.o: route.c route.h
	$(CC) $(CFLAGS) -c route.c

tests/test_route: tests/test_route.c route.o
	$(CC) $(CFLAGS) -o $@ tests/test_route.c route.o

test-route: tests/test_route
	./tests/test_route

.PHONY: all cgi clean test test-unit test-adder test-server

clean:
	rm -f *.o tiny *~ tests/test_http_parse
	(cd cgi-bin; make clean)
