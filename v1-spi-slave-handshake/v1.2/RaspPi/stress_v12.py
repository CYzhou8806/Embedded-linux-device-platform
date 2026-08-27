#!/usr/bin/env python3
"""V1.2 压力测试：10000 帧随机读写，统计 echo 失配"""

import spidev
import time
import random

# ── 常量 ──
NOP         = 0x7F
WRITE_FLAG  = 0x80
INTER_FRAME = 0.0005

DEVICE_ID   = 0x00
FW_VERSION  = 0x01
STATUS      = 0x02
CONTROL     = 0x03
SAMPLE_RATE = 0x04

TOTAL_FRAMES = 10000

# ── SPI 初始化 ──
spi = spidev.SpiDev()
spi.open(0, 0)
spi.max_speed_hz = 1000000
spi.mode = 0

# ── 底层 ──
def xfer(cmd, data=0):
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
    xfer(addr)
    r = xfer(NOP)
    echo = r[0]
    val = (r[1] << 24) | (r[2] << 16) | (r[3] << 8) | r[4]
    return echo, addr, val

def write_reg(addr, val):
    cmd = addr | WRITE_FLAG
    xfer(cmd, val)
    r = xfer(NOP)
    echo = r[0]
    return echo, cmd

# ── 测试动作 ──
def random_read():
    """随机读一个寄存器，检查 echo"""
    addr = random.choice([DEVICE_ID, FW_VERSION, STATUS, CONTROL, SAMPLE_RATE])
    echo, expected, val = read_reg(addr)
    return echo == expected, "READ", expected, echo

def random_write():
    """随机写 SAMPLE_RATE 一个合法值，检查 echo"""
    val = random.randint(1, 10000)
    echo, expected = write_reg(SAMPLE_RATE, val)
    return echo == expected, "WRITE", expected, echo

# ── 主流程 ──
if __name__ == "__main__":
    print(f"=== V1.2 压力测试: {TOTAL_FRAMES} 帧 ===\n")

    # 冲洗流水线
    xfer(NOP)

    # 先清错误，停采集，把设备放到干净状态
    write_reg(CONTROL, 0x02)
    write_reg(CONTROL, 0x00)

    echo_mismatch = 0
    read_count = 0
    write_count = 0
    first_fail = None

    t0 = time.time()

    for i in range(TOTAL_FRAMES):
        if random.random() < 0.5:
            ok, op, expected, got = random_read()
            read_count += 1
        else:
            ok, op, expected, got = random_write()
            write_count += 1

        if not ok:
            echo_mismatch += 1
            if first_fail is None:
                first_fail = (i, op, expected, got)

        # 每 2000 帧报个进度
        if (i + 1) % 2000 == 0:
            print(f"  进度: {i+1}/{TOTAL_FRAMES}  失配: {echo_mismatch}")

    elapsed = time.time() - t0

    # ── 报告 ──
    print(f"\n=== 结果 ===")
    print(f"  总帧数:     {TOTAL_FRAMES}")
    print(f"  读操作:     {read_count}")
    print(f"  写操作:     {write_count}")
    print(f"  echo 失配:  {echo_mismatch}")
    print(f"  耗时:       {elapsed:.1f} 秒")
    print(f"  帧率:       {TOTAL_FRAMES / elapsed:.0f} 帧/秒")

    if first_fail:
        i, op, expected, got = first_fail
        print(f"\n  首次失败: 第 {i} 帧, {op}, 期望 {expected:#04x}, 收到 {got:#04x}")

    if echo_mismatch == 0:
        print(f"\n  ✅ 全部通过，协议层稳定")
    else:
        print(f"\n  ❌ 有 {echo_mismatch} 次失配，需要排查")

    spi.close()
    