import serial
import time

ser = serial.Serial("/dev/ttyUSB0",115200,timeout=2)

def send(cmd,wait=2):
    print("\nSEND:",cmd)
    ser.write((cmd+"\r\n").encode())
    time.sleep(wait)

    data = ser.read_all()
    if data:
        print(data.decode(errors="ignore"))

# 基本通信测试
send("AT")

# 重启ESP
send("AT+RST",5)

# 设置为station模式
send("AT+CWMODE=1")

# 开启DHCP
send("AT+CWDHCP=1,1")

# 扫描WiFi
send("AT+CWLAP",5)

# 连接热点（你的热点）
send('AT+CWJAP="gddxzy","12345678"',20)

# 查看IP地址
send("AT+CIFSR")

print("\n测试结束")
