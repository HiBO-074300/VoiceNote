# -*- coding: utf-8 -*-
# 临时诊断脚本：打开 COM3 抓 40 秒启动日志（打开端口会触发板子复位）
import serial
import time

PORT = 'COM3'
BAUD = 115200
SECONDS = 40

s = serial.Serial(PORT, BAUD, timeout=1)
# 复位板子重新启动，捕获完整开机日志
s.dtr = False
s.rts = True
time.sleep(0.1)
s.rts = False

t0 = time.time()
buf = b''
while time.time() - t0 < SECONDS:
    buf += s.read(4096)
s.close()

text = buf.decode('utf-8', 'replace')
with open('monitor_log.txt', 'w', encoding='utf-8') as f:
    f.write(text)
print('saved', len(buf), 'bytes to monitor_log.txt')
