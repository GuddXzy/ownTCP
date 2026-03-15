all:
	gcc server.c -o server -lpthread -lssl -lcrypto -std=c99
	gcc client.c -o client -lpthread -lreadline -std=c99

clean:
	rm -f server client
