#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <stdlib.h>
#include <stdint.h>
#include <errno.h>
#include "log.h"
#include <readline/readline.h>
typedef struct {
    uint32_t len;
    uint32_t type;
} MsgHeader;

typedef struct {
    char username[32];
    char password[32];
} LoginMsg;

typedef struct {
    char to_uid[32];
    char from_uid[32];
    char content[960];
} ChatMsg;

pthread_mutex_t send_mutex = PTHREAD_MUTEX_INITIALIZER;

#define MSG_TYPE_DATA     	1
#define MSG_TYPE_PING     	2
#define MSG_TYPE_PONG     	3
#define MSG_TYPE_LOGIN    	4
#define MSG_TYPE_LOGIN_OK 	5
#define MSG_TYPE_LOGIN_FAIL 	6
#define MSG_TYPE_CHAT     	7
#define MSG_TYPE_KICK     	8
#define MSG_TYPE_REGISTER       9
#define MSG_TYPE_REGISTER_OK    10
#define MSG_TYPE_REGISTER_FAIL  11
int recv_all(int fd, char *buf, int len) {
    int total = 0;
    while (total < len) {
        int k = recv(fd, buf + total, len - total, 0);
        if (k <= 0) return -1;
        total += k;
    }
    return 0;
}

int send_msg(int fd, char *buf, int len, int type) {
    MsgHeader header;
    header.len  = htonl(len);
    header.type = htonl(type);
    int ret = send(fd, &header, sizeof(MsgHeader), 0);
    if (ret <= 0) return -1;
    if (len > 0) {
        int sent = send(fd, buf, len, 0);
        if (sent <= 0) return -1;
    }
    return 0;
}

int recv_msg(int fd, char *buf, int *len, int *type) {
    MsgHeader header;
    int ret = recv_all(fd, (char*)&header, sizeof(MsgHeader));
    if (ret < 0) return -1;
    int n = ntohl(header.len);
    int t = ntohl(header.type);
    if (t < 1 || t > 11) return -1;
    if (n > 0) {
        int ret1 = recv_all(fd, buf, n);
        if (ret1 < 0) return -1;
    }
    *len = n;
    *type = t;
    return 0;
}

void* recv_thread(void* arg) {
    int conn_fd = *(int*)arg;
    free(arg);
    int len, type;
    char buf[1024];
    while (1) {
        int ret = recv_msg(conn_fd, buf, &len, &type);
        if (ret == -1) {
            log_warn("与服务器断开连接");
            break;
        }
        if (type == MSG_TYPE_PONG) continue;
        if (type == MSG_TYPE_DATA) {
            buf[len] = '\0';
            printf("服务端发来: %s\n", buf);
        }
        if (type == MSG_TYPE_CHAT) {
            ChatMsg *chat = (ChatMsg*)buf;
            chat->content[959] = '\0';
            printf("%s说: %s\n", chat->from_uid, chat->content);
        }
	if (type == MSG_TYPE_KICK) {
    		printf("您的账号在其他地方登录，已被踢下线\n");
    		break;
	}
    }
    return NULL;
}

void* heartbeat(void* arg) {
    int conn_fd = *(int*)arg;
    free(arg);
    while (1) {
        sleep(3);
        pthread_mutex_lock(&send_mutex);
        int sent = send_msg(conn_fd, "", 0, MSG_TYPE_PING);
        pthread_mutex_unlock(&send_mutex);
        if (sent < 0) break;
    }
    return NULL;
}
void clean_input(char *str) {
    int i = 0, j = 0;
    while (str[i]) {
        if (str[i] == 8 || str[i] == 127) {
            if (j > 0) j--;
        } else if (str[i] >= 32 && str[i] < 127) {
            str[j++] = str[i];
        }
        i++;
    }
    str[j] = '\0';
}

int main() {
    int conn_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (conn_fd < 0) {
        log_error("socket创建失败: %s", strerror(errno));
        return -1;
    }

    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(8888);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

    if (connect(conn_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        log_error("连接服务器失败: %s", strerror(errno));
        return -1;
    }
    log_info("已连接到服务器");
	printf("1. 登录\n2. 注册\n请选择: ");
	char choice[4];
	fgets(choice, sizeof(choice), stdin);
	
	LoginMsg login;
	memset(&login, 0, sizeof(LoginMsg));
	char *uname = readline("用户: ");
	char *passwd = readline("密码: ");
	strncpy(login.username, uname, 31);
	strncpy(login.password, passwd, 31);
	clean_input(login.username);
	clean_input(login.password);
	free(uname);
	free(passwd);
	
	if (choice[0] == '2') {
    	send_msg(conn_fd, (char*)&login, sizeof(LoginMsg), MSG_TYPE_REGISTER);
    	char buf[1024];
    	int len, type;
    	recv_msg(conn_fd, buf, &len, &type);
    	if (type == MSG_TYPE_REGISTER_OK) {
        	printf("注册成功，请重新启动客户端登录\n");
    	} else {
        	printf("注册失败，用户名已存在\n");
    	}
    	close(conn_fd);
    	return 0;
	}
	send_msg(conn_fd, (char*)&login, sizeof(LoginMsg), MSG_TYPE_LOGIN);
    	char buf[1024];
    	int len, type;
    	recv_msg(conn_fd, buf, &len, &type);
    	if (type == MSG_TYPE_LOGIN_OK) {
        log_info("登录成功");

        int *rv_ptr = malloc(sizeof(int));
        *rv_ptr = conn_fd;
        pthread_t rv_tid;
        pthread_create(&rv_tid, NULL, recv_thread, rv_ptr);
        pthread_detach(rv_tid);

        int *hb_ptr = malloc(sizeof(int));
        *hb_ptr = conn_fd;
        pthread_t hb_tid;
        pthread_create(&hb_tid, NULL, heartbeat, hb_ptr);
        pthread_detach(hb_tid);
    } else {
        log_warn("登录失败");
        close(conn_fd);
        return 0;
    }

    while (1) {
        fgets(buf, sizeof(buf), stdin);
        buf[strcspn(buf, "\n")] = '\0';
        if (strcmp(buf, "quit") == 0) break;

        char *colon = strchr(buf, ':');
        if (colon == NULL) {
            printf("格式错误，请输入 目标用户:消息内容\n");
            continue;
        }
        *colon = '\0';
        char *to_uid  = buf;
        char *content = colon + 1;

        ChatMsg chat;
        memset(&chat, 0, sizeof(ChatMsg));
        strncpy(chat.to_uid, to_uid, 31);
        strncpy(chat.content, content, 959);

        pthread_mutex_lock(&send_mutex);
        send_msg(conn_fd, (char*)&chat, sizeof(ChatMsg), MSG_TYPE_CHAT);
        pthread_mutex_unlock(&send_mutex);
    }

    close(conn_fd);
    return 0;
}
