CC      = gcc
CFLAGS  = -std=c99 -Wall

all: server client esp_client

server: server.c protocol.h log.h config.h
	$(CC) $(CFLAGS) server.c -o server -lpthread -lssl -lcrypto

client: client.c protocol.h log.h
	$(CC) $(CFLAGS) client.c -o client -lpthread -lreadline

esp_client: esp_client.c protocol.h log.h
	$(CC) $(CFLAGS) esp_client.c -o esp_client

clean:
	rm -f server client esp_client

.PHONY: all clean
