
TARGET=http_server.exe
CC=gcc
LIBS=-lws2_32

all: release

help:
	@echo "make debug|release"

release:
	$(CC) -I. -o $(TARGET) http_server.c wepoll.c $(LIBS)

debug:
	$(CC) -g -I. -o $(TARGET) http_server.c wepoll.c $(LIBS)

clean:
	rm $(TARGET)
