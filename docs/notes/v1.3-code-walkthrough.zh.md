# V1.3 代码详解报告

本报告逐块讲解两份代码：
- MCU 固件：`MCU_v1-MCU-device-control/Core/Src/main.c`（以及 CubeMX 生成的 `spi.c`/`tim.c` 中跟本次问题相关的部分）
- 树莓派测试脚本：`RaspPi/testv13.py`

目的是让你自己检查里面有没有功能/设计问题，而不是我替你下结论。文末列了一份"请重点检查"的疑点清单。

---

## 一、整体架构，先建立一个心智模型

```
树莓派（主机 Master）  <---SPI 5字节固定帧--->  STM32（从机 Slave）
                                                      |
                                                  TIM2 定时器
                                                      |
                                                  FIFO 环形缓冲区
```

STM32 扮演一个"数据采集卡"：
1. `TIM2` 定时器按设定频率产生"样本"（其实就是一个自增序号的低16位），塞进 `FIFO`
2. 树莓派通过 SPI 隔一段时间来"问"：现在有几个样本？给我看看序号，给我数据
3. 中间靠一套 5 字节的寄存器读写协议（V1.2 就定义好的）来通信

---

## 二、MCU 固件 `main.c` 逐块讲解

### 2.1 协议常量

```c
#define FRAME_LEN        5      // 每一帧固定 5 字节：1字节命令 + 4字节数据
#define CMD_WRITE_FLAG   0x80   // 命令字节最高位=1表示"写"，=0表示"读"
#define CMD_ADDR_MASK    0x7F   // 命令字节低7位是寄存器地址
```

### 2.2 寄存器地址表

```c
REG_DEVICE_ID       0x00  // 只读，固定返回 0xAC00ACC0，用来验证通信通不通
REG_FW_VERSION      0x01  // 只读，固定返回 0x00010300
REG_STATUS          0x02  // 只读，状态/错误标志位
REG_CONTROL         0x03  // 可写，bit0=启动/停止采集，bit1=清除错误标志
REG_SAMPLE_RATE     0x04  // 可写，1-10000 Hz
REG_FIFO_LEVEL      0x05  // 只读，FIFO 里还有几个样本没取走
REG_DATA_SEQ        0x06  // 只读，"看一眼"最老样本的序号（不弹出）
REG_DATA_VAL        0x07  // 只读，"取走"最老样本的数值（会弹出）
REG_OVERFLOW_COUNT  0x08  // 只读，FIFO 满了丢弃了几次
REG_SPI_REARM_FAIL  0x09  // （调试用）HAL 重新挂起接收失败次数
REG_SPI_ERROR_COUNT 0x0A  // （调试用）SPI 错误回调被触发次数
REG_NOP             0x7F  // 空操作，纯粹用来"占一帧"取回上一次的结果
```

### 2.3 STATUS 位定义

```c
ST_RUNNING    (1u << 0)  // 正在采集
ST_CMD_ERR    (1u << 1)  // 收到了非法命令/地址
ST_RANGE_ERR  (1u << 2)  // SAMPLE_RATE 写了超出范围的值
ST_SPI_RESYNC (1u << 3)  // 发生过一次 SPI 错误恢复
```

### 2.4 缓冲区和全局状态

```c
static uint8_t rx_buf[FRAME_LEN];   // 本次收到的 5 字节
static uint8_t tx_buf[FRAME_LEN];   // 下一次要发出去的 5 字节

static volatile uint32_t reg_status      = 0;
static volatile uint32_t reg_control     = 0;
static volatile uint32_t reg_sample_rate = 1000;
```

`volatile` 的意思是"告诉编译器不要优化掉对这个变量的读写"，因为它们会在中断里被改、在中断外被读（或者反过来），编译器如果不知道这一点，可能会把某次读操作优化成"用上次读到的缓存值"，导致读到过期数据。

### 2.5 FIFO：环形缓冲区

```c
#define FIFO_DEPTH 32

typedef struct {
    uint32_t sequence;  // 这个样本的全局序号
    uint32_t value;     // 样本的值
} FifoItem;

static FifoItem          fifo_buf[FIFO_DEPTH];
static volatile uint16_t fifo_head = 0;  // 下一个"写入"位置（生产者用）
static volatile uint16_t fifo_tail = 0;  // 下一个"读出"位置（消费者用）
static volatile uint32_t fifo_overflow = 0;
static volatile uint32_t seq_counter   = 0;
```

**生产者**是定时器中断（`fifo_push`），**消费者**是 SPI 中断里的读寄存器逻辑（`fifo_pop`）。这是经典的"单生产者单消费者环形队列"：`head` 只由生产者写、`tail` 只由消费者写，两边只读对方的变量，天然不需要加锁。

```c
static uint16_t fifo_level(void) {
    uint16_t h = fifo_head;
    uint16_t t = fifo_tail;
    if (h >= t) return h - t;
    return FIFO_DEPTH - t + h;
}
```
算出 FIFO 里还有几个有效样本——如果 `head` 绕了一圈回到比 `tail` 小的位置，就要加上 `FIFO_DEPTH` 修正。

```c
static void fifo_push(uint32_t value) {
    uint16_t next = (fifo_head + 1) % FIFO_DEPTH;
    if (next == fifo_tail) {
        fifo_overflow++;   // 满了，本次样本直接丢弃
        return;
    }
    fifo_buf[fifo_head].sequence = seq_counter++;
    fifo_buf[fifo_head].value    = value;
    fifo_head = next;   // 最后才更新 head，这样消费者不会看到"写了一半"的数据
}
```
注意：`fifo_head = next` 放在**最后一行**，这是有意为之——只有整个 `fifo_buf[fifo_head]` 都填好之后，才让消费者"看得见"这个新槽位（消费者只根据 `head != tail` 来判断有没有新数据）。

```c
static uint8_t fifo_peek(uint32_t *seq_out, uint32_t *val_out) {
    if (fifo_head == fifo_tail) return 0;   // 空的
    *seq_out = fifo_buf[fifo_tail].sequence;
    *val_out = fifo_buf[fifo_tail].value;
    return 1;
}

static uint8_t fifo_pop(void) {
    if (fifo_head == fifo_tail) return 0;
    fifo_tail = (fifo_tail + 1) % FIFO_DEPTH;
    return 1;
}
```
`peek` 只看不动，`pop` 才真正把 `tail` 往前挪一格（丢弃这个槽位）。

### 2.6 DATA_READY GPIO（目前没接线，为未来内核驱动准备）

```c
static void update_data_ready_gpio(void) {
    if (fifo_head != fifo_tail) {
        HAL_GPIO_WritePin(GPIOA, GPIO_PIN_8, GPIO_PIN_SET);
    } else {
        HAL_GPIO_WritePin(GPIOA, GPIO_PIN_8, GPIO_PIN_RESET);
    }
}
```
FIFO 里有数据就把 PA8 拉高，没有就拉低。**这个引脚现在没有接到树莓派任何引脚上**，纯粹是为下一阶段"Linux 内核驱动监听 GPIO 中断"做准备，目前完全不影响协议本身。

### 2.7 定时器频率计算

```c
static void update_timer_rate(uint32_t rate_hz) {
    uint32_t period = 10000 / rate_hz;   // TIM2 基频是 10000 Hz（下面会讲怎么来的）
    if (period < 1) period = 1;
    __HAL_TIM_SET_AUTORELOAD(&htim2, period - 1);
    __HAL_TIM_SET_COUNTER(&htim2, 0);
}
```
`10000 / rate_hz`：比如要 1000Hz，就是 10000/1000=10，重装载值设成 9（因为计数器是从 0 数到重装载值，一共数 `period` 次才算一个周期）。

**TIM2 基频从哪来**（对照 `tim.c`）：
```c
htim2.Init.Prescaler = 7199;   // 72MHz / (7199+1) = 10000 Hz 的计数频率
htim2.Init.Period    = 9;      // 这是 CubeMX 里的默认值，运行时会被 update_timer_rate 覆盖
```

### 2.8 SPI 错误恢复

```c
static void spi_resync(void) {
    volatile uint32_t tmp;
    __HAL_SPI_DISABLE(&hspi2);          // 先关掉 SPI 外设
    tmp = hspi2.Instance->DR;           // 读 DR
    tmp = hspi2.Instance->SR;           // 读 SR —— 这两步是标准的"清除 OVR 错误标志"手法
    (void)tmp;
    __HAL_SPI_CLEAR_OVRFLAG(&hspi2);    // 用 HAL 宏再清一次（内部做的事情和上面一样，是双保险）

    hspi2.State     = HAL_SPI_STATE_READY;   // 手动把 HAL 内部状态机改回"就绪"
    hspi2.ErrorCode = HAL_SPI_ERROR_NONE;
    hspi2.Lock      = HAL_UNLOCKED;          // 防止上次错误没释放锁，导致以后所有请求都被拒绝

    reg_status |= ST_SPI_RESYNC;
    __HAL_SPI_ENABLE(&hspi2);            // 重新打开 SPI
}
```
这个函数在发生 SPI 硬件错误（比如 OVR，接收溢出）时被调用，试图让 SPI 外设"重启"回到能正常工作的状态。**注意**：这里是直接修改 HAL 库内部的私有字段（`State`/`ErrorCode`/`Lock`），不是通过官方 API（比如 `HAL_SPI_DeInit`+`HAL_SPI_Init`）做的，属于"能跑但不严谨"的写法。

### 2.9 寄存器读

```c
static uint32_t reg_read(uint8_t addr) {
    switch (addr) {
        case REG_DEVICE_ID:   return 0xAC00ACC0u;
        case REG_FW_VERSION:  return 0x00010300u;
        case REG_STATUS:      return reg_status;
        case REG_CONTROL:     return reg_control;
        case REG_SAMPLE_RATE: return reg_sample_rate;
        case REG_NOP:         return 0;

        case REG_FIFO_LEVEL:
            return fifo_level();

        case REG_DATA_SEQ:
            if (fifo_peek(&last_seq, &last_val)) {
                item_peeked = 1;      // 记住"已经 peek 过了"
                return last_seq;
            }
            return 0;

        case REG_DATA_VAL:
            if (item_peeked) {
                item_peeked = 0;
                fifo_pop();                  // 真正弹出
                update_data_ready_gpio();
                return last_val;             // 用的是刚才 peek 缓存的值，不重新读
            }
            if (fifo_peek(&last_seq, &last_val)) {  // 万一没有先 peek 过 DATA_SEQ，直接读 DATA_VAL
                fifo_pop();
                update_data_ready_gpio();
                return last_val;
            }
            return 0;

        case REG_OVERFLOW_COUNT:  return fifo_overflow;
        case REG_SPI_REARM_FAIL:  return spi_rearm_fail;
        case REG_SPI_ERROR_COUNT: return spi_error_count;

        default:
            reg_status |= ST_CMD_ERR;   // 未知地址，标记一个错误位
            return 0;
    }
}
```

**这里有一个值得注意的设计**：`DATA_SEQ` 和 `DATA_VAL` 是配对使用的——协议设计的初衷是"先读 DATA_SEQ 看一眼序号，再读 DATA_VAL 真正取走数据，两次读到的应该是同一个样本"。`item_peeked` 这个标志就是用来保证这一点的：如果你先读了 DATA_SEQ，紧接着读 DATA_VAL，会拿到"跟刚才 peek 到的同一个"数据，而不是重新去看 FIFO 最新状态（因为期间 FIFO 可能已经变了）。

### 2.10 寄存器写

```c
static void reg_write(uint8_t addr, uint32_t val) {
    switch (addr) {
    case REG_CONTROL:
        if (val & 0x02u) {   // CLEAR_FLAGS
            reg_status &= ~(ST_CMD_ERR | ST_RANGE_ERR | ST_SPI_RESYNC);
            val &= ~0x02u;
        }
        reg_control = val & 0x01u;

        if (reg_control & 0x01u) {
            // START：把采集相关的一切状态清零、重新开始
            reg_status |= ST_RUNNING;
            seq_counter   = 0;
            fifo_head     = 0;
            fifo_tail     = 0;
            fifo_overflow = 0;
            item_peeked   = 0;
            update_data_ready_gpio();
            update_timer_rate(reg_sample_rate);
            HAL_TIM_Base_Start_IT(&htim2);
        } else {
            // STOP
            reg_status &= ~ST_RUNNING;
            HAL_TIM_Base_Stop_IT(&htim2);
            update_data_ready_gpio();
        }
        break;

    case REG_SAMPLE_RATE:
        if (val >= 1u && val <= 10000u) {
            reg_sample_rate = val;
            if (reg_control & 0x01u) {
                update_timer_rate(val);   // 如果正在采集，立刻用新频率
            }
        } else {
            reg_status |= ST_RANGE_ERR;
        }
        break;

    default:
        reg_status |= ST_CMD_ERR;   // 写只读寄存器或未知地址
        break;
    }
}
```

**这里有一个之前提到过的潜在竞态**：START 分支里 `fifo_head = 0; fifo_tail = 0;` 这几行跑在 SPI 中断里（优先级更高），如果这个瞬间正好打断了定时器中断（优先级更低）正在执行的 `fifo_push()`，可能导致复位后 `fifo_head` 被写成一个基于"清零前"状态算出来的错误值。这是一个真实存在但触发窗口很窄的问题（只会在 START 命令那一帧、且恰好撞上定时器中断时发生）。

### 2.11 帧处理（协议流水线的核心）

```c
static void handle_frame(void) {
  HAL_GPIO_TogglePin(GPIOA, GPIO_PIN_8);   // 调试用：每次真正执行就翻转一下引脚

  uint8_t  cmd  = rx_buf[0];
  uint8_t  addr = cmd & CMD_ADDR_MASK;
  uint32_t resp = 0;

  if (cmd & CMD_WRITE_FLAG) {
    reg_write(addr, unpack_u32(&rx_buf[1]));
    resp = 0;
  } else {
    resp = reg_read(addr);
  }

  tx_buf[0] = cmd;              // 回显命令字节，用于校验
  pack_u32(&tx_buf[1], resp);   // 把结果打包进 tx_buf，下一帧才会真正发出去
}
```
这是"流水线协议"的关键：**这一帧收到的命令，要等到下一帧才能把结果传回去**，因为 SPI 全双工——发送和接收是同一个时钟节拍完成的，从机不可能"边解析命令边把结果塞进同一帧"。

### 2.12 `main()` 初始化

```c
tx_buf[0] = REG_NOP;
pack_u32(&tx_buf[1], 0);
HAL_GPIO_WritePin(GPIOA, GPIO_PIN_8, GPIO_PIN_RESET);
HAL_SPI_TransmitReceive_IT(&hspi2, tx_buf, rx_buf, FRAME_LEN);   // 启动第一次中断接收，之后立刻返回

while (1) { }   // 主循环什么都不做，所有工作都在中断里
```

### 2.13 中断回调

```c
void HAL_SPI_TxRxCpltCallback(SPI_HandleTypeDef *hspi) {
  if (hspi->Instance == SPI2) {
    handle_frame();                                             // 处理这一帧
    if (HAL_SPI_TransmitReceive_IT(&hspi2, tx_buf, rx_buf, FRAME_LEN) != HAL_OK) {
      spi_rearm_fail++;                                          // 记录重新挂起失败
    }
  }
}

void HAL_SPI_ErrorCallback(SPI_HandleTypeDef *hspi) {
  if (hspi->Instance == SPI2) {
    spi_error_count++;
    spi_resync();
    if (HAL_SPI_TransmitReceive_IT(&hspi2, tx_buf, rx_buf, FRAME_LEN) != HAL_OK) {
      spi_rearm_fail++;
    }
  }
}

void HAL_TIM_PeriodElapsedCallback(TIM_HandleTypeDef *htim) {
  if (htim->Instance == TIM2) {
    if (reg_control & 0x01u) {
      uint32_t sample_value = seq_counter & 0xFFFFu;
      fifo_push(sample_value);
    }
  }
}
```

这三个是整个固件真正"干活"的地方：
- 每收发完一帧 SPI，`TxRxCpltCallback` 处理这一帧、马上挂起下一次接收
- 出错就 `ErrorCallback` 走恢复流程
- 定时器每到时间就往 FIFO 塞一个样本

---

## 三、树莓派测试脚本 `testv13.py` 逐块讲解

### 3.1 底层通信

```python
INTER_FRAME = 0.0005  # 每次 SPI 传输后强制睡 500us

def xfer(cmd, data=0):
    tx = [cmd & 0xFF, (data>>24)&0xFF, (data>>16)&0xFF, (data>>8)&0xFF, data&0xFF]
    rx = spi.xfer2(tx)     # 一次性发出 5 字节、同时收回 5 字节（真正的硬件 SPI 传输）
    time.sleep(INTER_FRAME)
    return rx
```

```python
def read_reg(addr):
    xfer(addr)              # 第一帧：发命令
    r = xfer(NOP)            # 第二帧：借 NOP 把结果取回来（对应固件的"流水线"设计）
    echo = r[0]
    if echo != addr:
        echo_mismatch_count += 1
        print(f"  !! ECHO 不匹配: ...")   # 只是打印警告，不会中断/抛异常
    val = (r[1]<<24)|(r[2]<<16)|(r[3]<<8)|r[4]
    return val
```
**关键点**：`echo` 校验失败只是打印一行字，**函数照样把 `val` 算出来并返回**，调用者拿到的是"可能是垃圾"的数据，但脚本流程完全不受影响，会继续往下跑。

### 3.2 Test 1：回归测试 `test_regression()`

依次测试：读 DEVICE_ID、读 FW_VERSION、写读 SAMPLE_RATE、写非法值验证拒绝、清除错误标志、写非法地址验证报错、START/STOP。全部是"单次读写 + 立刻检查结果"，**不涉及任何循环等待**，这是它最稳定的原因之一。

### 3.3 Test 2：100Hz 采集 `test_acquisition_slow()`

```python
write_reg(SAMPLE_RATE, 100)
write_reg(CONTROL, 0x01)     # START

t0 = time.time()
duration = 10.0
while time.time() - t0 < duration:      # 外层：按墙钟时间控制总时长
    level = read_reg(FIFO_LEVEL)
    if level == 0:
        time.sleep(0.001)
        continue

    while level > 0:                    # 内层：把当前 FIFO 里的都读完
        seq = read_reg(DATA_SEQ)
        val = read_reg(DATA_VAL)
        ...
        level = read_reg(FIFO_LEVEL)    # 每读一个就重新查一次剩余量
```

### 3.4 Test 3：1000Hz 采集 `test_acquisition_fast()` —— 结构和 Test 2 完全一样，只是 `SAMPLE_RATE=1000`、`duration=5.0`

**这就是你反复强调的"卡住 10+ 秒"现象的关键所在，我这次讲清楚机制：**

```python
while time.time() - t0 < duration:      # ← 只有这一行在检查"是不是已经跑够 5 秒了"
    level = read_reg(FIFO_LEVEL)
    if level == 0:
        time.sleep(0.001)
        continue

    while level > 0:                    # ← 这个内层循环，从进入到退出之前，完全不检查时间！
        seq = read_reg(DATA_SEQ)
        val = read_reg(DATA_VAL)
        ...
        level = read_reg(FIFO_LEVEL)    # 每次都用"刚读到的 level"决定还要不要继续循环
```

**这是一个真实的设计缺陷**：如果 `read_reg(FIFO_LEVEL)` 某一次因为通信问题读到了一个错误的巨大数值（比如本该是 3，结果因为字节错位读成了 30000），内层 `while level > 0` 就会老老实实地把这个"30000"当真，试图读 30000 次"样本"——每次读取（`DATA_SEQ` + `DATA_VAL` + 下一次 `FIFO_LEVEL`）大约需要 3 次 SPI 传输 ×（40us 传输 + 500us 睡眠）≈ 1.6ms，30000 次就是接近 **50 秒**，而且这整个过程中**外层的 5 秒时间判断完全没有机会被检查到**，因为程序卡在内层循环里出不来。

**这完美解释了你说的"本来 5 秒、结果跑了十几秒"**——不一定是 MCU 真的"卡死"了那么久，很可能是**某一次 `FIFO_LEVEL` 读到了错误的大数值，Python 脚本自己在傻乎乎地空转，试图读取一堆根本不存在的"样本"**，直到把这个错误数值对应的循环次数走完才会退出。这是纯粹的**脚本设计问题**：内层循环缺少一个时间上限或者"最大迭代次数"保护。

### 3.5 扫频测试 `test_frequency_sweep()`

依次在多个频率下各跑几秒，记录每个频率下的 echo 错误次数，用来找到问题出现的临界频率。结构和 Test 3 完全一样（同样有上面说的内层循环没有时间保护的问题），只是外层多加了一层"换频率"的循环。

### 3.6 Test 4：协议压力测试 `test_stress()`

```python
for i in range(10000):
    addr = random.choice([DEVICE_ID, FW_VERSION, STATUS, SAMPLE_RATE])
    xfer(addr)
    r = xfer(NOP)
    if r[0] != addr:
        echo_mismatch += 1
```
**注意：这个测试从头到尾没有调用 `write_reg(CONTROL, 0x01)`，TIM2 定时器从来没有启动过**。它测的是"纯协议层"在高帧率（约 792 帧/秒）下稳不稳定，跟 FIFO/定时器完全无关——这也是为什么它一直很稳定：它根本没有触发那个只在"定时器运行时"才出现的问题。

---

## 四、请你重点检查的疑点清单

按我认为"值得你自己再确认一遍"的优先级排列：

1. **【脚本设计问题，几乎可以确定】** `test_acquisition_fast()` 和 `test_frequency_sweep()` 里的内层 `while level > 0` 循环没有任何超时/最大次数保护——这大概率是"整个测试卡住变成 10+ 秒"这个现象的直接原因，跟 MCU 是否真的"死机"是两回事。建议给这个内层循环也加上时间检查，比如 `while level > 0 and time.time() - t0 < duration:`，这样即使某次读到错误的巨大 `level` 值，也不会拖着整个测试跑出好几倍的时间。

2. **【真实存在但触发窗口很窄】** `reg_write()` 里 `REG_CONTROL` 的 START 分支直接复位 `fifo_head`/`fifo_tail`，没有先停掉定时器中断，理论上存在竞态（前面 2.10 节讲过）。

3. **【已知的不严谨写法，但目前没证据是主因】** `spi_resync()` 直接改写 HAL 库私有字段而不是走标准的 DeInit/Init 流程。

4. **【尚未验证的核心谜团】** 为什么只有"定时器以 1000Hz 运行"这一个条件会导致 SPI 内容错乱、而 800fps 纯协议测试完全稳定——这个我们已经通过逻辑分析仪证实了**不是电气毛刺**（每一帧时钟边沿数量都对），也证实了**不是 HAL 状态机卡死或错误回调死循环**（两个计数器都是 0），根本机制目前仍未最终定位，需要靠调试器现场抓 CPU 状态才能进一步缩小范围。

你可以先按第 1 条把脚本改一下、重新跑一次，看看"卡住变成 10+ 秒"这个现象是不是就消失了（哪怕 echo 错误还在，至少不会再"失控"地跑很久）。这样能把"脚本设计缺陷"和"MCU 真实行为异常"这两件事彻底分开来看。
