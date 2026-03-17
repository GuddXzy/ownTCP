#define _XOPEN_SOURCE 600
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <termios.h>
#include <stdint.h>
#include <time.h>
#include <arpa/inet.h>
#include "log.h"
#include "protocol.h"

/*
 *  * ESP8266 AT 固件客户端 v2
 *   *
 *    * 架构: Linux主机 --UART--> ESP8266(AT固件) --WiFi/TCP--> 服务器
 *     *
 *      * v2 新增:
 *       * - 协议v2: Magic(0xABCD) + CRC-16/MODBUS 校验
 *        * - 断线自动重连: TCP断开 -> 重连TCP -> 失败则重连WiFi -> 重新登录
 *         * - 指数退避: 重连间隔 2s -> 4s -> 8s -> ... -> 60s，防止重连风暴
 *          *
 *           * 面试要点:
 *            * Q: 断线重连为什么要加退避(backoff)?
 *             * A: 如果服务器宕机，成百上千个设备同时重连会造成"重连风暴"，
 *              *    指数退避让设备错峰重连，减轻服务器瞬时压力。
 *               *
 *                * Q: 怎么区分WiFi断开和TCP断开?
 *                 * A: AT+CIPSEND返回"link is not valid"或收到"CLOSED"是TCP断开，
 *                  *    ESP会异步推送"WIFI DISCONNECT"表示WiFi断开。
 *                   *    TCP断开只需重连TCP，WiFi断开需要先重连WiFi再连TCP。
 *                    */

#define SERVER_IP   "10.68.216.116"
#define SERVER_PORT "8888"
#define UART_DEV    "/dev/ttyUSB0"
#define WIFI_SSID   "gddxzy"
#define WIFI_PASS   "12345678"

#define BACKOFF_INIT    2    /* 初始重连等待秒数 */
#define BACKOFF_MAX     60   /* 最大重连等待秒数 */

int LOG_LEVEL = 0;
int uart_fd = -1;

/* ---- 串口操作 ---- */

int uart_open(const char *dev) {
    int fd = open(dev, O_RDWR | O_NOCTTY | O_NDELAY);
    if (fd < 0) {
        log_error("打开串口失败: %s", dev);
        return -1;
    }
    struct termios tty;
    tcgetattr(fd, &tty);
    cfsetispeed(&tty, B115200);
    cfsetospeed(&tty, B115200);
    tty.c_cflag &= ~PARENB;
    tty.c_cflag &= ~CSTOPB;
    tty.c_cflag &= ~CSIZE;
    tty.c_cflag |= CS8;
    tty.c_cflag |= CREAD | CLOCAL;
    tty.c_lflag &= ~(ICANON | ECHO | ECHOE | ISIG);
    tty.c_iflag &= ~(IXON | IXOFF | IXANY);
    tty.c_oflag &= ~OPOST;
    tty.c_cc[VMIN]  = 0;
    tty.c_cc[VTIME] = 10;
    tcsetattr(fd, TCSANOW, &tty);
    return fd;
}

void uart_send(const char *cmd) {
    write(uart_fd, cmd, strlen(cmd));
    write(uart_fd, "\r\n", 2);
}

int uart_recv(char *buf, int size, int timeout_ms) {
    int total = 0;
    int elapsed = 0;
    while (elapsed < timeout_ms && total < size - 1) {
        int n = read(uart_fd, buf + total, size - total - 1);
        if (n > 0) total += n;
        usleep(10000);
        elapsed += 10;
    }
    buf[total] = '\0';
    return total;
}

int at_cmd(const char *cmd, const char *expect, int timeout_ms) {
    char buf[1024];
    uart_send(cmd);
    uart_recv(buf, sizeof(buf), timeout_ms);
    log_debug("AT响应: %s", buf);
    if (expect && strstr(buf, expect)) return 0;
    return -1;
}

/*
 *  * at_send_bytes: 通过AT+CIPSEND发送二进制数据
 *   *
 *    * 返回值:
 *     *  0  = 发送成功
 *      * -1  = 普通失败（可重试）
 *       * -2  = TCP连接已断开（需要重连）
 *        *
 *         * 面试要点:
 *          * Q: 为什么要区分返回值 -1 和 -2?
 *           * A: -1 是临时性故障（比如busy），上层可以重试同一操作。
 *            *    -2 是连接断开，上层必须走重连流程，继续发数据没有意义。
 *             *    这种分级错误码设计在嵌入式驱动层很常见。
 *              */
int at_send_bytes(const char *data, int len) {
    char cmd[32];
    char buf[256];

    tcflush(uart_fd, TCIFLUSH);
    usleep(50000);
    while (read(uart_fd, buf, sizeof(buf)) > 0);

    snprintf(cmd, sizeof(cmd), "AT+CIPSEND=%d", len);

    int retry = 3;
    while (retry--) {
        uart_send(cmd);
        uart_recv(buf, sizeof(buf), 2000);
        log_debug("CIPSEND请求: %s", buf);
        if (strstr(buf, ">")) break;
        if (strstr(buf, "busy")) {
            log_warn("ESP忙，等待重试...");
            sleep(1);
            tcflush(uart_fd, TCIFLUSH);
            continue;
        }
        if (strstr(buf, "link is not valid") || strstr(buf, "CLOSED")) {
            log_error("TCP连接已断开");
            return -2;
        }
        return -1;
    }
    if (retry < 0) return -1;

    write(uart_fd, data, len);
    uart_recv(buf, sizeof(buf), 2000);
    log_debug("CIPSEND响应: %s", buf);

    if (strstr(buf, "CLOSED") || strstr(buf, "SEND FAIL")) {
        log_error("发送后连接断开");
        return -2;
    }
    return 0;
}

int send_msg(char *buf, int len, int type) {
    MsgHeader header;
    header.magic = htons(PROTO_MAGIC);
    header.crc16 = htons(crc16_modbus((uint8_t*)buf, len));
    header.len   = htonl(len);
    header.type  = htonl(type);

    int total_len = sizeof(MsgHeader) + len;
    char *packet = malloc(total_len);
    if (!packet) return -1;
    memcpy(packet, &header, sizeof(MsgHeader));
    if (len > 0) memcpy(packet + sizeof(MsgHeader), buf, len);

    int ret = at_send_bytes(packet, total_len);
    free(packet);
    return ret;
}

/* ---- 连接管理 ----
 *  *
 *   * 面试要点:
 *    * Q: 为什么把WiFi连接和TCP连接分成独立函数?
 *     * A: 单一职责。TCP断了只需调connect_tcp()，不用重新连WiFi。
 *      *    WiFi断了才需要先connect_wifi()再connect_tcp()。
 *       *    这样重连逻辑更清晰，也减少不必要的操作。
 *        */

int connect_wifi(void) {
    at_cmd("AT+CWMODE=1", "OK", 2000);
    log_info("连接WiFi...");
    char wifi_cmd[128];
    snprintf(wifi_cmd, sizeof(wifi_cmd),
             "AT+CWJAP=\"%s\",\"%s\"", WIFI_SSID, WIFI_PASS);
    if (at_cmd(wifi_cmd, "GOT IP", 15000) < 0) {
        log_error("WiFi连接失败");
        return -1;
    }
    log_info("WiFi连接成功");
    return 0;
}

int connect_tcp(void) {
    /* 先关闭可能残留的旧连接 */
    at_cmd("AT+CIPCLOSE", "OK", 1000);

    log_info("连接TCP服务器...");
    char tcp_cmd[128];
    snprintf(tcp_cmd, sizeof(tcp_cmd),
             "AT+CIPSTART=\"TCP\",\"%s\",%s", SERVER_IP, SERVER_PORT);
    if (at_cmd(tcp_cmd, "OK", 5000) < 0) {
        log_error("TCP连接失败");
        return -1;
    }
    log_info("TCP连接成功");
    return 0;
}

int do_login(void) {
    LoginMsg login;
    memset(&login, 0, sizeof(LoginMsg));
    strncpy(login.username, "sensor", 31);
    strncpy(login.password, "12345678", 31);
    int ret = send_msg((char*)&login, sizeof(LoginMsg), MSG_TYPE_LOGIN);
    if (ret < 0) return -1;
    log_info("已发送登录请求");
    sleep(2);
    return 0;
}

/*
 *  * reconnect: 断线重连核心逻辑
 *   *
 *    * 策略:
 *     * 1. 先尝试只重连TCP（WiFi可能还在）
 *      * 2. TCP连不上 -> 重连WiFi -> 再连TCP
 *       * 3. 每次失败后等待时间翻倍（指数退避）
 *        * 4. 等待时间上限60秒，防止设备长时间不响应
 *         * 5. 连上后重新登录
 *          *
 *           * 面试要点:
 *            * Q: 指数退避的实现方式?
 *             * A: backoff = min(backoff * 2, BACKOFF_MAX)
 *              *    初始2秒，依次4、8、16、32、60、60...
 *               *    成功后重置为初始值。
 *                *
 *                 * Q: 为什么不用随机抖动(jitter)?
 *                  * A: 单设备场景下不需要。如果是几百台设备同时重连，
 *                   *    应该加 random jitter 进一步错峰: sleep(backoff + rand()%backoff)
 *                    */
int reconnect(void) {
    int backoff = BACKOFF_INIT;

    while (1) {
        log_warn("开始重连 (等待%d秒后重试)...", backoff);
        sleep(backoff);

        /* 先尝试直接重连TCP */
        if (connect_tcp() == 0) {
            if (do_login() == 0) {
                log_info("重连成功 (TCP重连)");
                return 0;
            }
        }

        /* TCP连不上，可能WiFi也断了，重连WiFi */
        log_warn("TCP重连失败，尝试重连WiFi...");
        if (connect_wifi() == 0 && connect_tcp() == 0) {
            if (do_login() == 0) {
                log_info("重连成功 (WiFi+TCP重连)");
                return 0;
            }
        }

        /* 指数退避 */
        backoff = backoff * 2;
        if (backoff > BACKOFF_MAX) backoff = BACKOFF_MAX;
    }
}

/* ---- 主函数 ---- */

int main() {
    uart_fd = uart_open(UART_DEV);
    if (uart_fd < 0) return -1;
    log_info("串口打开成功");
    sleep(3);
    tcflush(uart_fd, TCIOFLUSH);

    /* AT握手 */
    int retry = 5;
    while (retry--) {
        if (at_cmd("AT", "OK", 3000) == 0) break;
        log_warn("AT无响应，重试...");
        tcflush(uart_fd, TCIOFLUSH);
        sleep(1);
    }
    if (retry < 0) {
        log_error("ESP8266无响应");
        return -1;
    }
    log_info("ESP8266正常");

    /* 首次连接 */
    if (connect_wifi() < 0) return -1;
    if (connect_tcp() < 0) return -1;
    if (do_login() < 0) return -1;
    log_info("系统就绪 (协议v2: magic=0x%04X)", PROTO_MAGIC);

    /* 数据上报循环 + 心跳 + 断线重连 */
    int seq = 0;
    time_t last_ping = time(NULL);

    while (1) {
        /* 心跳保活 */
        if (time(NULL) - last_ping >= 5) {
            int ret = send_msg("", 0, MSG_TYPE_PING);
            if (ret == -2) {
                log_warn("心跳发送失败，触发重连");
                reconnect();
                last_ping = time(NULL);
                continue;
            }
            last_ping = time(NULL);
            log_debug("发送心跳");
        }

        /* 模拟传感器数据上报 */
        char data[64];
        int temp = 20 + (seq % 10);
        snprintf(data, sizeof(data), "sensor|temp:%d", temp);

        int ret = send_msg(data, strlen(data), MSG_TYPE_DATA);
        if (ret == -2) {
            log_warn("数据发送失败，触发重连");
            reconnect();
            last_ping = time(NULL);
            continue;  /* 重连后重发当前数据 */
        }

        log_info("上报传感器数据: %s", data);
        seq++;
        sleep(3);
    }

    close(uart_fd);
    return 0;
}
