#!/usr/bin/env python3
"""V1.2 寄存器协议验证脚本"""

import spidev
import time
import sys

# ── 常量 ──
NOP         = 0x7F
WRITE_FLAG  = 0x80
INTER_FRAME = 0.0005   # 500us 帧间隔

# 寄存器地址
DEVICE_ID   = 0x00
FW_VERSION  = 0x01
STATUS      = 0x02
CONTROL     = 0x03
SAMPLE_RATE = 0x04

# ── SPI 初始化 ──
spi = spidev.SpiDev()
spi.open(0, 0)          # bus=0, cs=0
spi.max_speed_hz = 1000000
spi.mode = 0

# ── 底层通信 ──
def xfer(cmd, data=0):
    """发一帧 5 字节，收一帧 5 字节"""
    tx = [
        cmd & 0xFF,
        (data >> 24) & 0xFF,
        (data >> 16) & 0xFF,
        (data >> 8)  & 0xFF,
         data        & 0xFF,
    ]
    rx = spi.xfer2(tx)
    time.sleep(INTER_FRAME)
    return rx

def read_reg(addr):
    """流水线读：第一帧发命令，第二帧取数据"""
    xfer(addr)                          # 第 N 帧：告诉 MCU 我要读什么
    r = xfer(NOP)                       # 第 N+1 帧：拿回结果
    echo = r[0]
    if echo != addr:
        print(f"  !! ECHO 不匹配: 期望 {addr:#04x}, 收到 {echo:#04x}")
    val = (r[1] << 24) | (r[2] << 16) | (r[3] << 8) | r[4]
    return val

def write_reg(addr, val):
    """写寄存器，同样需要第二帧确认 echo"""
    cmd = addr | WRITE_FLAG
    xfer(cmd, val)                      # 第 N 帧：写命令 + 数据
    r = xfer(NOP)                       # 第 N+1 帧：确认 echo
    echo = r[0]
    if echo != cmd:
        print(f"  !! ECHO 不匹配: 期望 {cmd:#04x}, 收到 {echo:#04x}")

# ── 测试 ──
def test_read_id():
    print("--- 测试 1: 读 DEVICE_ID ---")
    val = read_reg(DEVICE_ID)
    print(f"  DEVICE_ID = {val:#010x}  {'OK' if val == 0xAC00ACC0 else 'FAIL'}")

def test_read_fw():
    print("--- 测试 2: 读 FW_VERSION ---")
    val = read_reg(FW_VERSION)
    print(f"  FW_VERSION = {val:#010x}  {'OK' if val == 0x00010200 else 'FAIL'}")

def test_write_sample_rate():
    print("--- 测试 3: 写 SAMPLE_RATE = 5000 ---")
    write_reg(SAMPLE_RATE, 5000)
    val = read_reg(SAMPLE_RATE)
    print(f"  读回 SAMPLE_RATE = {val}  {'OK' if val == 5000 else 'FAIL'}")

def test_reject_bad_value():
    print("--- 测试 4: 写非法 SAMPLE_RATE = 99999 ---")
    write_reg(SAMPLE_RATE, 99999)
    val = read_reg(SAMPLE_RATE)
    print(f"  读回 SAMPLE_RATE = {val}  {'OK (被拒绝)' if val == 5000 else 'FAIL'}")
    st = read_reg(STATUS)
    print(f"  STATUS = {st:#010x}  RANGE_ERR={'是' if st & 0x04 else '否'}")

def test_clear_flags():
    print("--- 测试 5: 清除错误标志 ---")
    write_reg(CONTROL, 0x02)            # CLEAR_FLAGS
    st = read_reg(STATUS)
    print(f"  STATUS = {st:#010x}  {'OK (已清零)' if (st & 0x06) == 0 else 'FAIL'}")

def test_bad_address():
    print("--- 测试 6: 读非法地址 0x42 ---")
    val = read_reg(0x42)
    print(f"  返回值 = {val:#010x}  {'OK' if val == 0 else 'FAIL'}")
    st = read_reg(STATUS)
    print(f"  STATUS = {st:#010x}  CMD_ERR={'是' if st & 0x02 else '否'}")

def test_start_stop():
    print("--- 测试 7: START/STOP ---")
    write_reg(CONTROL, 0x02)            # 先清错误
    write_reg(CONTROL, 0x01)            # START
    st = read_reg(STATUS)
    print(f"  START 后 STATUS = {st:#010x}  RUNNING={'是' if st & 0x01 else '否'}")
    write_reg(CONTROL, 0x00)            # STOP
    st = read_reg(STATUS)
    print(f"  STOP 后  STATUS = {st:#010x}  RUNNING={'否' if not (st & 0x01) else '仍在跑'}")

# ── 主流程 ──
if __name__ == "__main__":
    print("=== V1.2 寄存器协议验证 ===\n")

    # 先发一帧 NOP 把流水线冲干净
    xfer(NOP)

    test_read_id()
    test_read_fw()
    test_write_sample_rate()
    test_reject_bad_value()
    test_clear_flags()
    test_bad_address()
    test_start_stop()

    print("\n=== 全部测试完成 ===")
    spi.close()
    