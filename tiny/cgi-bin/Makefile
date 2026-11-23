CC = gcc
CFLAGS = -O2 -Wall -I ..

all: adder

# rio_readn을 쓰므로 csapp.c를 함께 링크한다
adder: adder.c ../csapp.c ../csapp.h
	$(CC) $(CFLAGS) -o adder adder.c ../csapp.c -lpthread

clean:
	rm -f adder *~
