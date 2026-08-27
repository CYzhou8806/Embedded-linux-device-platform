#!/usr/bin/env python3
"""
V1.3 完整测试脚本
包含：
  1. V1.2 回归测试（确认旧功能没坏）
  2. FIFO 采集测试（100 Hz，检查序列号连续性）
  3. 高速压力测试（1000 Hz，观察丢数据情况）
  4. 10000 帧协议压力测试
"""

import spidev
import time
import random
import sys

# ── 常量 ──
NOP         = 0x7F
WRITE_FLAG  = 0x80
INTER_FRAME = 0.0005  # 500us

# 寄存器地址
DEVICE_ID       = 0x00
FW_VERSION      = 0x01
STATUS          = 0x02
CONTROL         = 0x03
SAMPLE_RATE     = 0x04
FIFO_LEVEL      = 0x05
DATA_SEQ        = 0x06
DATA_VAL        = 0x07
OVERFLOW_COUNT  = 0x08
SPI_REARM_FAIL  = 0x09
SPI_ERROR_COUNT = 0x0A

# ── SPI 初始化 ──
spi = spidev.SpiDev()
spi.open(0, 0)
spi.max_speed_hz = 1000000
spi.mode = 0

# ── 底层通信 ──
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

echo_mismatch_count = 0

def read_reg(addr):
    global echo_mismatch_count
    xfer(addr)
    r = xfer(NOP)
    echo = r[0]
    if echo != addr:
        echo_mismatch_count += 1
        print(f"  !! ECHO 不匹配: 期望 {addr:#04x}, 收到 {echo:#04x}")
    val = (r[1] << 24) | (r[2] << 16) | (r[3] << 8) | r[4]
    return val

def write_reg(addr, val):
    global echo_mismatch_count
    cmd = addr | WRITE_FLAG
    xfer(cmd, val)
    r = xfer(NOP)
    echo = r[0]
    if echo != cmd:
        echo_mismatch_count += 1
        print(f"  !! ECHO 不匹配: 期望 {cmd:#04x}, 收到 {echo:#04x}")

def stop_and_clear():
    """停止采集并清除错误标志"""
    write_reg(CONTROL, 0x00)
    write_reg(CONTROL, 0x02)  # CLEAR_FLAGS

# ══════════════════════════════════════════════
#  第一部分：V1.2 回归测试
# ══════════════════════════════════════════════
def test_regression():
    print("=" * 50)
    print("第一部分：V1.2 回归测试")
    print("=" * 50)

    xfer(NOP)  # 冲洗流水线
    stop_and_clear()

    # 测试 1: 读 DEVICE_ID
    val = read_reg(DEVICE_ID)
    ok = val == 0xAC00ACC0
    print(f"  读 DEVICE_ID = {val:#010x}  {'OK' if ok else 'FAIL'}")

    # 测试 2: 读 FW_VERSION（V1.3 应该是 0x00010300）
    val = read_reg(FW_VERSION)
    print(f"  读 FW_VERSION = {val:#010x}")

    # 测试 3: 写读 SAMPLE_RATE
    write_reg(SAMPLE_RATE, 5000)
    val = read_reg(SAMPLE_RATE)
    ok = val == 5000
    print(f"  写 5000 读回 {val}  {'OK' if ok else 'FAIL'}")

    # 测试 4: 非法值被拒绝
    write_reg(SAMPLE_RATE, 99999)
    val = read_reg(SAMPLE_RATE)
    ok = val == 5000
    print(f"  写 99999 读回 {val}  {'OK (被拒绝)' if ok else 'FAIL'}")
    st = read_reg(STATUS)
    print(f"  STATUS = {st:#010x}  RANGE_ERR={'是' if st & 0x04 else '否'}")

    # 测试 5: 清除标志
    write_reg(CONTROL, 0x02)
    st = read_reg(STATUS)
    ok = (st & 0x06) == 0
    print(f"  清除后 STATUS = {st:#010x}  {'OK' if ok else 'FAIL'}")

    # 测试 6: 非法地址
    val = read_reg(0x42)
    st = read_reg(STATUS)
    ok = (st & 0x02) != 0
    print(f"  读 0x42, CMD_ERR={'是' if ok else '否'}  {'OK' if ok else 'FAIL'}")

    # 测试 7: START/STOP
    stop_and_clear()
    write_reg(CONTROL, 0x01)
    st = read_reg(STATUS)
    running = (st & 0x01) != 0
    print(f"  START 后 RUNNING={'是' if running else '否'}  {'OK' if running else 'FAIL'}")
    write_reg(CONTROL, 0x00)
    st = read_reg(STATUS)
    stopped = (st & 0x01) == 0
    print(f"  STOP 后 RUNNING={'否' if stopped else '仍在跑'}  {'OK' if stopped else 'FAIL'}")

    stop_and_clear()
    print()

# ══════════════════════════════════════════════
#  第二部分：FIFO 采集测试（低速，应该无丢失）
# ══════════════════════════════════════════════
def test_acquisition_slow():
    print("=" * 50)
    print("第二部分：FIFO 采集测试 (100 Hz, 10 秒)")
    print("=" * 50)

    stop_and_clear()

    # 设采样率 100 Hz
    write_reg(SAMPLE_RATE, 100)
    val = read_reg(SAMPLE_RATE)
    print(f"  SAMPLE_RATE 设为 {val} Hz")

    # # 启动采集
    # write_reg(CONTROL, 0x01)
    # print("  采集已启动，等待 1 秒让 FIFO 积累...")
    # time.sleep(1.0)

    # # 读 FIFO_LEVEL
    # level = read_reg(FIFO_LEVEL)
    # print(f"  1 秒后 FIFO_LEVEL = {level}")


    # START 后立刻开始读，不再等 1 秒
    write_reg(CONTROL, 0x01)
    print("  采集已启动，立刻开始读取...")

    # 开始连续读取
    total_samples = 0
    seq_gaps = 0
    last_sequence = -1
    t0 = time.time()
    duration = 10.0

    while time.time() - t0 < duration:
        level = read_reg(FIFO_LEVEL)
        if level == 0:
            time.sleep(0.001)  # FIFO 空，等 1ms
            continue

        # 读出所有可用的 sample（同时受外层的总时长限制，
        # 防止某次 FIFO_LEVEL 读到错误的巨大数值时，脚本卡在这里失控地跑很久）
        while level > 0 and time.time() - t0 < duration:
            seq = read_reg(DATA_SEQ)
            val = read_reg(DATA_VAL)
            total_samples += 1

            if last_sequence >= 0:
                expected = (last_sequence + 1) & 0xFFFFFFFF
                if seq != expected:
                    seq_gaps += 1
                    if seq_gaps <= 5:  # 只打印前 5 个跳号
                        print(f"    跳号: 期望 {expected}, 收到 {seq}")
            last_sequence = seq

            level = read_reg(FIFO_LEVEL)

    elapsed = time.time() - t0

    # 停止采集
    write_reg(CONTROL, 0x00)
    overflow = read_reg(OVERFLOW_COUNT)

    print(f"\n  --- 结果 ---")
    print(f"  持续时间:   {elapsed:.1f} 秒")
    print(f"  收到 sample: {total_samples}")
    print(f"  序列号跳号: {seq_gaps}")
    print(f"  FIFO 溢出:  {overflow}")
    print(f"  实际速率:   {total_samples / elapsed:.1f} samples/sec")

    if seq_gaps == 0 and overflow == 0:
        print(f"  ✅ 100 Hz 零丢失")
    elif overflow > 0 and seq_gaps == 0:
        print(f"  ⚠️  有溢出但无跳号")
    else:
        print(f"  ❌ 有跳号，需要排查")

    stop_and_clear()
    print()

# ══════════════════════════════════════════════
#  第三部分：高速采集测试（1000 Hz，预期会丢数据）
# ══════════════════════════════════════════════
def test_acquisition_fast():
    print("=" * 50)
    print("第三部分：高速采集测试 (1000 Hz, 5 秒)")
    print("=" * 50)

    stop_and_clear()

    write_reg(SAMPLE_RATE, 1000)
    val = read_reg(SAMPLE_RATE)
    print(f"  SAMPLE_RATE 设为 {val} Hz")

    write_reg(CONTROL, 0x01)
    print("  采集已启动...")

    total_samples = 0
    seq_gaps = 0
    last_sequence = -1
    t0 = time.time()
    duration = 5.0

    while time.time() - t0 < duration:
        level = read_reg(FIFO_LEVEL)
        if level == 0:
            time.sleep(0.001)
            continue

        while level > 0 and time.time() - t0 < duration:
            seq = read_reg(DATA_SEQ)
            val = read_reg(DATA_VAL)
            total_samples += 1

            if last_sequence >= 0:
                expected = (last_sequence + 1) & 0xFFFFFFFF
                if seq != expected:
                    seq_gaps += 1
            last_sequence = seq

            level = read_reg(FIFO_LEVEL)

    elapsed = time.time() - t0
    write_reg(CONTROL, 0x00)
    overflow = read_reg(OVERFLOW_COUNT)
    rearm_fail = read_reg(SPI_REARM_FAIL)
    error_count = read_reg(SPI_ERROR_COUNT)

    print(f"\n  --- 结果 ---")
    print(f"  持续时间:   {elapsed:.1f} 秒")
    print(f"  收到 sample: {total_samples}")
    print(f"  序列号跳号: {seq_gaps}")
    print(f"  FIFO 溢出:  {overflow}")
    print(f"  SPI 重挂起失败次数: {rearm_fail}")
    print(f"  SPI 错误回调次数:   {error_count}")
    print(f"  实际速率:   {total_samples / elapsed:.1f} samples/sec")

    if seq_gaps > 0 or overflow > 0:
        print(f"  ℹ️  1000 Hz 下 Pi 轮询跟不上，这是预期行为")
        print(f"  ℹ️  V3 kernel driver + IRQ 会解决这个问题")
    else:
        print(f"  ✅ 1000 Hz 也没丢！MCU 和 Pi 都够快")

    stop_and_clear()
    print()

# ══════════════════════════════════════════════
#  第三.5部分：扫频测试——找到问题从哪个频率开始出现
# ══════════════════════════════════════════════
def test_frequency_sweep(rates=(50, 100, 200, 300, 500, 700, 800, 900, 1000),
                          duration_per_rate=3.0):
    global echo_mismatch_count
    print("=" * 50)
    print("扫频测试：找到 echo 错误开始出现的临界频率")
    print("=" * 50)

    results = []

    for rate in rates:
        stop_and_clear()
        echo_mismatch_count = 0

        write_reg(SAMPLE_RATE, rate)
        write_reg(CONTROL, 0x01)

        total_samples = 0
        seq_gaps = 0
        last_sequence = -1
        t0 = time.time()

        while time.time() - t0 < duration_per_rate:
            level = read_reg(FIFO_LEVEL)
            if level == 0:
                time.sleep(0.001)
                continue
            while level > 0 and time.time() - t0 < duration_per_rate:
                seq = read_reg(DATA_SEQ)
                val = read_reg(DATA_VAL)
                total_samples += 1
                if last_sequence >= 0:
                    expected = (last_sequence + 1) & 0xFFFFFFFF
                    if seq != expected:
                        seq_gaps += 1
                last_sequence = seq
                level = read_reg(FIFO_LEVEL)

        elapsed = time.time() - t0
        write_reg(CONTROL, 0x00)
        mismatches = echo_mismatch_count

        status = "OK" if mismatches == 0 else f"坏 ({mismatches} 次 echo 错误)"
        print(f"  {rate:5d} Hz | 耗时 {elapsed:5.1f}s | samples={total_samples:5d} | 跳号={seq_gaps:4d} | {status}")
        results.append((rate, mismatches, seq_gaps, elapsed))

    stop_and_clear()

    print("\n  --- 汇总 ---")
    first_bad = None
    for rate, mismatches, seq_gaps, elapsed in results:
        if mismatches > 0 and first_bad is None:
            first_bad = rate
    if first_bad:
        print(f"  从 {first_bad} Hz 开始出现 echo 错误")
    else:
        print(f"  所有测试频率都没有 echo 错误")
    print()

# ══════════════════════════════════════════════
#  第四部分：10000 帧协议压力测试（不采集）
# ══════════════════════════════════════════════
def test_stress():
    print("=" * 50)
    print("第四部分：10000 帧协议压力测试")
    print("=" * 50)

    stop_and_clear()
    xfer(NOP)

    echo_mismatch = 0
    total = 10000

    t0 = time.time()
    for i in range(total):
        addr = random.choice([DEVICE_ID, FW_VERSION, STATUS, SAMPLE_RATE])
        xfer(addr)
        r = xfer(NOP)
        if r[0] != addr:
            echo_mismatch += 1

        if (i + 1) % 2000 == 0:
            print(f"  进度: {i+1}/{total}  失配: {echo_mismatch}")

    elapsed = time.time() - t0

    # 检查 SPI_RESYNC 计数
    st = read_reg(STATUS)
    resync = "是" if (st & 0x08) else "否"

    print(f"\n  --- 结果 ---")
    print(f"  echo 失配:    {echo_mismatch}")
    print(f"  SPI_RESYNC:   {resync}")
    print(f"  耗时:         {elapsed:.1f} 秒")
    print(f"  帧率:         {total / elapsed:.0f} 帧/秒")

    if echo_mismatch == 0:
        print(f"  ✅ 协议层稳定")
    else:
        print(f"  ❌ 有失配，需排查")

    print()

# ══════════════════════════════════════════════
#  主流程
# ══════════════════════════════════════════════
if __name__ == "__main__":
    print("\n" + "=" * 50)
    print("  V1.3 完整测试")
    print("=" * 50 + "\n")

    test_regression()
    test_acquisition_slow()
    test_acquisition_fast()
    test_stress()
    #test_frequency_sweep()


    print("=" * 50)
    print("  全部测试完成")
    print("=" * 50)

    spi.close()
    