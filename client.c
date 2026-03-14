#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <stdlib.h>

typedef struct {
	uint32_t len;
	uint32_t type;
} MsgHeader;

pthread_mutex_t send_mutex = PTHREAD_MUTEX_INITIALIZER;

#define MSG_TYPE_DATA 1
#define MSG_TYPE_PING 2
#define MSG_TYPE_PONG 3

int recv_all (int fd, char *buf, int len){
        int total = 0;
        while  (total < len ) {
        int k = recv(fd, buf + total, len - total, 0);
        if (k <= 0) {
        return -1;
        }
        total = total + k;
}
        return 0;
}

int send_msg(int fd, char *buf, int len, int type) {
        MsgHeader header;
	header.len = htonl(len);
	header.type = htonl(type);
	int ret = send(fd, &header,sizeof(MsgHeader), 0);
        if (ret <= 0 || (type != 1 && type != 2 && type != 3)) {
        return -1;
        }
        int sent = send(fd, buf ,len ,0);
        if (sent <= 0) {
        return -1;
        }
        return 0;
}


int recv_msg(int fd, char *buf,int *len, int *type) {
        MsgHeader header;
	int ret = recv_all(fd, (char*)&header, sizeof(MsgHeader));
	int n = ntohl(header.len);
	int t = ntohl(header.type);
	if (ret < 0 ||(t != 1 && t != 2 && t !=3)) return -1;
	
        int ret1 = recv_all(fd,buf, n);
        if (ret1 < 0) {
        return -1;
    	}
        *len = n;
	*type = t;
        return 0;
}

void* heartbeat(void* arg) {
	int conn_fd = *(int*)arg;
	free(arg);
	while(1) {
		sleep(3);
		pthread_mutex_lock(&send_mutex);
		int sent = send_msg(conn_fd, "",0, MSG_TYPE_PING);	
		pthread_mutex_unlock(&send_mutex);
		if (sent <= 0) {
		break;}
		}
	return NULL;
	}
	
int main()
{	
	int conn_fd = socket(AF_INET,SOCK_STREAM,0);
		
	struct sockaddr_in addr;
	addr.sin_family = AF_INET;
	addr.sin_port  = htons(8888);
	inet_pton(AF_INET,"127.0.0.1", &addr.sin_addr);
	connect(conn_fd,(struct sockaddr*)&addr,sizeof(addr));	
	
	int *hb_ptr = malloc(sizeof(int));
	*hb_ptr = conn_fd;
	pthread_t hb_tid;
	pthread_create(&hb_tid, NULL, heartbeat, hb_ptr);
	pthread_detach(hb_tid);	

	char buf[1024];
	int len,type;
	while(1) {
	fgets(buf, sizeof(buf), stdin);
	buf[strcspn(buf, "\n")] = '\0';
	if(strcmp(buf, "quit") == 0) break;
	
	pthread_mutex_lock(&send_mutex);
	send_msg(conn_fd,buf,strlen(buf), MSG_TYPE_DATA);
	pthread_mutex_unlock(&send_mutex);
	if(recv_msg(conn_fd,buf,&len, &type)==-1)break;
	if(type == MSG_TYPE_PONG) continue;
	buf[len] = '\0';
	printf("%s\n",buf);
	}
	close(conn_fd);

	return 0;
}
