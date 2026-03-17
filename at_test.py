import serial
import time

for baud in [115200, 9600, 57600, 38400, 19200, 74880]:
    try:
        s = serial.Serial('/dev/ttyUSB0', baud, timeout=2)
        time.sleep(0.5)
        s.write(b'AT\r\n')
        time.sleep(1)
        resp = s.read_all()
        print(f"波特率 {baud}: {repr(resp)}")
        s.close()
        time.sleep(0.5)
    except Exception as e:
        print(f"波特率 {baud} 失败: {e}")
