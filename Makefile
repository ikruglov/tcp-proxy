CC=clang
CFLAGS=-std=gnu99 -O3 -g -Wall -pthread -DDEBUG -DEV_STANDALONE=1 -fno-strict-aliasing
TSAN=-fsanitize=thread -fsanitize-blacklist=blacklist.tsan -fPIE -pie # need clang for compilation
INCLUDE=-I . -I src -I libev
SOURCE=src/net.c src/server_ctx.c src/tcp-proxy.c

all: tcp-proxy

mkdir:
	@mkdir -p bin

tcp-proxy: mkdir
	$(CC) -c -O3 -g -DEV_STANDALONE=1 -Wno-all libev/ev.c -o ev.o
	$(CC) $(CFLAGS) $(INCLUDE) ev.o $(SOURCE) -o bin/tcp-proxy

tsan: mkdir
	$(CC) -c -O3 -g -fPIC -DEV_STANDALONE=1 -Wno-all libev/ev.c -o ev.o
	$(CC) $(CFLAGS) $(INCLUDE) $(TSAN) ev.o $(SOURCE) -o bin/tcp-proxy

clean:
	rm -f *.o
	rm -f bin/relay
