
wnTCP

跟着学 Linux 网络编程顺手写的一个 TCP 通信项目，从 socket 一路写到 epoll，
后来又拿 ESP8266 做了个串口透传，算是把嵌入式那头也串起来了。
代码肯定还有不少问题，欢迎指出。

## 能干啥

- 多客户端同时连接，基于 epoll 的单线程事件驱动
- 自定义二进制协议（Magic + CRC16 校验 + 长度 + 类型），解决粘包和数据校验
- 用户注册登录，密码 MD5 哈希后存文件
- 客户端之间点对点发消息，服务器负责转发
- 心跳检测，客户端 3s 发一次 PING，服务端超时自动踢
- 重复登录会把之前的踢下线，并通知对方
- ESP8266 通过串口 AT 指令连 WiFi，把传感器数据透传到服务器
- ESP 端断线自动重连，重连间隔逐步拉长，避免一直疯狂重试

## 跑起来

需要先装依赖：
```bash
yum install readline-devel openssl-devel   # CentOS
apt install libreadline-dev libssl-dev     # Ubuntu
```

然后：
```bash
make
./server        # 终端1，先改 config.txt 配端口
./client        # 终端2，可以多开
```

ESP 端需要接好串口（默认 /dev/ttyUSB0），改一下 esp_client.c 里的 WiFi 和服务器 IP：
```bash
./esp_client
```

## 协议结构

自己设计的一个简单二进制协议，Header 固定 12 字节：

| 字段  | 大小   | 说明                          |
|-------|--------|-------------------------------|
| magic | 2 字节 | 固定 0xABCD，标识帧起始       |
| crc16 | 2 字节 | 对 payload 做 CRC-16/MODBUS   |
| len   | 4 字节 | payload 长度，网络字节序      |
| type  | 4 字节 | 消息类型，网络字节序          |

Body 根据 type 不同有不同结构体，登录是 LoginMsg（64B），聊天是 ChatMsg（1024B）。

加 Magic 是为了在字节流里快速定位帧头，加 CRC 是防止应用层的数据出错（TCP 校验管不到程序 bug 导致的错误）。

## 文件结构

```
├── server.c       # epoll 服务端
├── client.c       # PC 客户端，多线程收发 + 心跳
├── esp_client.c   # ESP8266 AT 固件客户端，串口透传 + 断线重连
├── protocol.h     # 协议头定义、CRC 查表、消息结构体
├── log.h          # 几个宏封装的简单日志
├── config.h       # 服务端配置读取
├── config.txt     # 端口、心跳超时等配置
├── Makefile
└── users.txt      # 运行时生成，存用户名:MD5密码
```

## 已知问题

- 服务端 epoll 用的阻塞 IO，严格来说应该配合非阻塞 + 状态机
- ESP 端没处理服务器下行的 +IPD 数据，目前只能上报不能接收
- 没做优雅退出，Ctrl+C 不会通知客户端

## 后续想做的

- [ ] socket 改非阻塞，recv 用状态机攒包
- [ ] ESP 端解析 +IPD，支持服务器下发指令
- [ ] 群聊
- [ ] 离线消息

水平有限，代码写得不好的地方欢迎批评指教！
