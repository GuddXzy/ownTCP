#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <stdlib.h>
#include <stdint.h>
#include <time.h>
#include <errno.h>
#include <sys/epoll.h>
#include "log.h"
#include <openssl/md5.h>
#include "config.h"
typedef struct {
    uint32_t len;
    uint32_t type;
} MsgHeader;

typedef struct {
    int fd;
    time_t last_ping;
    int is_logged_in;
    char uid[32];
} Client;

typedef struct {
    char to_uid[32];
    char from_uid[32];
    char content[960];
} ChatMsg;

typedef struct {
    char username[32];
    char password[32];
} LoginMsg;

typedef struct {
    char username[32];
    char password[33];
} User;

Client *clients = NULL;
int client_count = 0;
int client_cap = 0;
int epfd;
int LOG_LEVEL = 0;

#define MSG_TYPE_DATA     	1
#define MSG_TYPE_PING     	2
#define MSG_TYPE_PONG     	3
#define MSG_TYPE_LOGIN    	4
#define MSG_TYPE_LOGIN_OK 	5
#define MSG_TYPE_LOGIN_FAIL     6
#define MSG_TYPE_CHAT           7
#define MSG_TYPE_KICK 	        8
#define MSG_TYPE_REGISTER       9
#define MSG_TYPE_REGISTER_OK    10
#define MSG_TYPE_REGISTER_FAIL  11

User user_table[256];
int user_count = 0;

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
    if (n < 0 || t < 1 || t > 11) return -1;
    if (n > 0) {
        int rec = recv_all(fd, buf, n);
        if (rec < 0) return -1;
    }
    *len = n;
    *type = t;
    return 0;
}

void add_to_epoll(int fd) {
    struct epoll_event ev;
    ev.events  = EPOLLIN;
    ev.data.fd = fd;
    epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev);
}

void client_add(int fd) {
    if (client_count == client_cap) {
        client_cap = (client_cap == 0) ? 16 : client_cap * 2;
        clients = realloc(clients, client_cap * sizeof(Client));
    }
    clients[client_count].fd           = fd;
    clients[client_count].last_ping    = time(NULL);
    clients[client_count].is_logged_in = 0;
    clients[client_count].uid[0]       = '\0';
    client_count++;
}

void client_remove(int fd) {
    epoll_ctl(epfd, EPOLL_CTL_DEL, fd, NULL);
    close(fd);
    for (int i = 0; i < client_count; i++) {
        if (clients[i].fd == fd) {
            clients[i] = clients[client_count - 1];
            client_count--;
            break;
        }
    }
}

void check_heartbeat() {
    time_t now = time(NULL);
    for (int i = 0; i < client_count; i++) {
        if (now - clients[i].last_ping > g_config.heartbeat_timeout) {
            log_warn("心跳超时, fd=%d uid=%s", clients[i].fd, clients[i].uid);
            client_remove(clients[i].fd);
            i--;
        }
    }
}

void load_users() {
    FILE *f = fopen("users.txt", "r");
    if (f == NULL) {
        log_warn("users.txt不存在，从空用户表启动");
        return;
    }
    while (fscanf(f, "%31[^:]:%31s\n", user_table[user_count].username,
                  user_table[user_count].password) == 2) {
        user_count++;
        if (user_count >= 256) break;
    }
    fclose(f);
    log_info("加载用户数: %d", user_count);
}

int check_register(char *username) {
    for (int i = 0; i < user_count; i++) {
        if (strcmp(user_table[i].username, username) == 0) {
            return 0;
        }
    }
    return 1;
}

void md5_hash(const char *input, char *output) {
    unsigned char digest[16];
    MD5((unsigned char*)input, strlen(input), digest);
    for (int i = 0; i < 16; i++) {
        sprintf(output + i * 2, "%02x", digest[i]);
    }
    output[32] = '\0';
}

void save_user(char *username, char *password) {
    char hashed[33];
    md5_hash(password, hashed);
    FILE *f = fopen("users.txt", "a");
    if (f == NULL) {
        log_error("无法写入users.txt");
        return;
    }
    fprintf(f, "%s:%s\n", username, hashed);
    fclose(f);
    strcpy(user_table[user_count].username, username);
    strcpy(user_table[user_count].password, hashed);
    user_count++;
    log_info("新用户注册: %s", username);
}

int check_login(char *username, char *password) {
    char hashed[33];
    md5_hash(password, hashed);
    for (int i = 0; i < user_count; i++) {
        if (strcmp(user_table[i].username, username) == 0 &&
            strcmp(user_table[i].password, hashed) == 0) {
            return 1;
        }
    }
    return 0;
}

int main() {
    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        log_error("socket创建失败: %s", strerror(errno));
        return -1;
    }

    load_config("config.txt");
    LOG_LEVEL = g_config.log_level;

    int opt = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    addr.sin_family      = AF_INET;
    addr.sin_port = htons(g_config.port);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(listen_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        log_error("bind失败: %s", strerror(errno));
        return -1;
    }
    if (listen(listen_fd, 10) < 0) {
        log_error("listen失败: %s", strerror(errno));
        return -1;
    }

    epfd = epoll_create1(0);
    if (epfd < 0) {
        log_error("epoll_create1失败: %s", strerror(errno));
        return -1;
    }

    add_to_epoll(listen_fd);
    add_to_epoll(STDIN_FILENO);
    load_users();
    log_info("服务器启动成功，监听8888端口");

    char buf[1024];
    int len, type;
    char *reply = "收到";
    struct epoll_event events[64];
    int n;

    while (1) {
        n = epoll_wait(epfd, events, 64, 3000);
        if (n == 0) {
            check_heartbeat();
            continue;
        }
        for (int i = 0; i < n; i++) {
            if (events[i].data.fd == listen_fd) {
                int conn_fd = accept(listen_fd, NULL, NULL);
                if (conn_fd < 0) {
                    log_error("accept失败: %s", strerror(errno));
                    continue;
                }
                log_info("有客户端连接, fd=%d", conn_fd);
                add_to_epoll(conn_fd);
                client_add(conn_fd);
            } else if (events[i].data.fd == STDIN_FILENO) {
                char input[1024];
                fgets(input, sizeof(input), stdin);
                input[strcspn(input, "\n")] = '\0';
                char *colon = strchr(input, ':');
                if (colon == NULL) continue;
                *colon = '\0';
                char *uid = input;
                char *msg = colon + 1;
                for (int j = 0; j < client_count; j++) {
                    if (strcmp(clients[j].uid, uid) == 0) {
                        send_msg(clients[j].fd, msg, strlen(msg), MSG_TYPE_DATA);
                        log_info("已发送给 %s", uid);
                        break;
                    }
                }
            } else {
                int fd = events[i].data.fd;
                int ret = recv_msg(fd, buf, &len, &type);
                if (ret == -1) {
                    log_warn("客户端断开, fd=%d", fd);
                    client_remove(fd);
                    continue;
                }
                if (type == MSG_TYPE_DATA) {
                    buf[len] = '\0';
                    log_info("来自fd=%d: %s", fd, buf);
                    send_msg(fd, reply, strlen(reply), MSG_TYPE_DATA);
                }
                if (type == MSG_TYPE_PING) {
                    for (int j = 0; j < client_count; j++) {
                        if (clients[j].fd == fd) {
                            clients[j].last_ping = time(NULL);
                            break;
                        }
                    }
                    send_msg(fd, "", 0, MSG_TYPE_PONG);
                }
                if (type == MSG_TYPE_LOGIN) {
                    LoginMsg *login = (LoginMsg*)buf;
                    if (check_login(login->username, login->password)) {
                        for (int j = 0; j < client_count; j++) {
    			    if (strcmp(clients[j].uid, login->username) == 0) {
        			log_warn("顶替旧连接: %s fd=%d", login->username, clients[j].fd);
        			send_msg(clients[j].fd, "", 0, MSG_TYPE_KICK);
        			client_remove(clients[j].fd);
        			break;
    				}
			}
			for (int j = 0; j < client_count; j++) {
                            if (clients[j].fd == fd) {
                                clients[j].is_logged_in = 1;
                                strcpy(clients[j].uid, login->username);
                                break;
                            }
                        }
                        log_info("登录成功: %s fd=%d", login->username, fd);
                        send_msg(fd, "", 0, MSG_TYPE_LOGIN_OK);
                    } else {
                        log_warn("登录失败, fd=%d", fd);
                        send_msg(fd, "", 0, MSG_TYPE_LOGIN_FAIL);
                        client_remove(fd);
                    }
                }
                if (type == MSG_TYPE_CHAT) {
                    ChatMsg *chat = (ChatMsg*)buf;
                    chat->content[959] = '\0';
                    for (int j = 0; j < client_count; j++) {
                        if (clients[j].fd == fd) {
                            strcpy(chat->from_uid, clients[j].uid);
                            break;
                        }
                    }
                    for (int j = 0; j < client_count; j++) {
                        if (strcmp(clients[j].uid, chat->to_uid) == 0) {
                            send_msg(clients[j].fd, (char*)chat, sizeof(ChatMsg), MSG_TYPE_CHAT);
                            log_info("转发: %s -> %s: %s", chat->from_uid, chat->to_uid, chat->content);
                            break;
                        }
                    }
                }
		        if (type == MSG_TYPE_REGISTER) {
                                LoginMsg *reg = (LoginMsg*)buf;
                                if (check_register(reg->username)) {
                                save_user(reg->username, reg->password);
                                send_msg(fd, "", 0, MSG_TYPE_REGISTER_OK);
                                } else {
                        log_warn("注册失败，用户名已存在: %s", reg->username);
                        send_msg(fd, "", 0, MSG_TYPE_REGISTER_FAIL);
                        }
                        client_remove(fd);
                        }
            }
        }
    }

    close(listen_fd);
    return 0;
}
