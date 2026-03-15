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

Client *clients = NULL;
int client_count = 0;
int client_cap = 0;
int epfd;

#define MSG_TYPE_DATA 1
#define MSG_TYPE_PING 2
#define MSG_TYPE_PONG 3
#define MSG_TYPE_LOGIN 4
#define MSG_TYPE_LOGIN_OK 5
#define MSG_TYPE_LOGIN_FAIL 6

typedef struct {
    char username[32];
    char password[32];
} LoginMsg;

typedef struct {
    char username[32];
    char password[32];
} User;

User user_table[] = {
    {"admin", "123456"},
    {"test",  "abcdef"},
};
int user_count = 2;

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
    header.len = htonl(len);
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
    if (n < 0 || t < 1 || t > 6) return -1;
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
    ev.events = EPOLLIN;
    ev.data.fd = fd;
    epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev);
}

void client_add(int fd) {
    if (client_count == client_cap) {
        client_cap = (client_cap == 0) ? 16 : client_cap * 2;
        clients = realloc(clients, client_cap * sizeof(Client));
    }
    clients[client_count].fd = fd;
    clients[client_count].last_ping = time(NULL);
    clients[client_count].is_logged_in = 0;
    clients[client_count].uid[0] = '\0';
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
        if (now - clients[i].last_ping > 9) {
            printf("心跳超时, fd=%d uid=%s\n", clients[i].fd, clients[i].uid);
            client_remove(clients[i].fd);
            i--;
        }
    }
}

int check_login(char *username, char *password) {
    for (int i = 0; i < user_count; i++) {
        if (strcmp(user_table[i].username, username) == 0 &&
            strcmp(user_table[i].password, password) == 0) {
            return 1;
        }
    }
    return 0;
}

int main() {
    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    printf("监听fd是%d\n", listen_fd);

    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_port = htons(8888);
    addr.sin_addr.s_addr = INADDR_ANY;

    bind(listen_fd, (struct sockaddr*)&addr, sizeof(addr));
    listen(listen_fd, 10);

    epfd = epoll_create1(0);
    add_to_epoll(listen_fd);
    add_to_epoll(STDIN_FILENO);

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
                printf("有客户端连接, conn_fd=%d\n", conn_fd);
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
                        printf("已发送给 %s\n", uid);
                        break;
                    }
                }
            } else {
                int fd = events[i].data.fd;
                int ret = recv_msg(fd, buf, &len, &type);
                if (ret == -1) {
                    client_remove(fd);
                    continue;
                }
                if (type == MSG_TYPE_DATA) {
                    buf[len] = '\0';
                    printf("来自fd=%d: %s\n", fd, buf);
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
                            if (clients[j].fd == fd) {
                                clients[j].is_logged_in = 1;
                                strcpy(clients[j].uid, login->username);
                                break;
                            }
                        }
                        printf("登录成功: %s, fd=%d\n", login->username, fd);
                        send_msg(fd, "", 0, MSG_TYPE_LOGIN_OK);
                    } else {
                        printf("登录失败, fd=%d\n", fd);
                        send_msg(fd, "", 0, MSG_TYPE_LOGIN_FAIL);
                        client_remove(fd);
                    }
                }
            }
        }
    }

    close(listen_fd);
    return 0;
}
