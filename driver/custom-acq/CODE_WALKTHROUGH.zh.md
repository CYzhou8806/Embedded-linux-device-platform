# custom_acq.c 逐行讲解（中文，学习用）

配合 `device-tree/README.zh.md` 一起看。这份文档标注每一段代码是**固定
模板**（换任何 SPI driver 基本都长这样，理解一次终身受用，不用记具体
写法）还是**这次业务相关**（跟我们自己的寄存器协议、FIFO 设计绑定，别
的项目会不一样）。

---

## 1–16 行：文件头注释

```c
// SPDX-License-Identifier: GPL-2.0-only
```
**固定模板。** 内核要求每个源文件声明许可证，这行是内核工具链认识的标
准格式，直接抄，所有 in-tree/out-of-tree 内核代码都这么写。

后面的 `/* ... */` 大注释是我们自己写的说明，不是模板，纯粹帮自己和别
人理解这个文件是干什么的。

## 18–21 行：`#include`

```c
#include <linux/module.h>   // 让代码能编译成"内核模块"（.ko），提供 module_init 这类机制
#include <linux/spi/spi.h>  // SPI 子系统的核心头文件：struct spi_device、spi_sync_transfer 等都来自这里
#include <linux/delay.h>    // usleep_range() 在这里声明
#include <linux/of.h>       // struct of_device_id、Device Tree 匹配相关的类型
```
**半固定。** 写 SPI driver 这四个头文件是标准搭配（这是"这类 driver 通
用的模板"），但具体引哪几个头文件永远取决于你用了哪些 API——不是死记
硬背这四行，而是理解"用了什么功能，就要引对应头文件"这个原则。

## 23–31 行：常量定义

```c
#define REG_DEVICE_ID	0x00
#define REG_FW_VERSION	0x01
#define CMD_NOP		0x7F
#define INTER_FRAME_US	500
```
**这次业务相关。** 这几个数字是我们自己的寄存器协议定义的（跟 MCU 固
件 `main.c` 里的寄存器地址、`testv13.py` 里的常量必须完全对应），换一
个协议这几个值就全变了。

## 33–35 行：私有数据结构

```c
struct custom_acq {
	struct spi_device *spi;
};
```
**半固定，会随功能增长。** 每个 driver 通常都有一个"私有数据结构体"，
用来存这个设备实例相关的所有状态（现在只存了 `spi` 指针，因为目前只有
"读寄存器"这一个功能）。以后加 FIFO/IRQ 处理时，这个结构体会长出更多
字段（比如 kfifo、锁、字符设备号）。**这个"每个设备一个私有结构体"的
模式本身是固定套路**，但里面装什么字段是这次业务相关的。

## 37–53 行：`custom_acq_xfer()` —— 单帧收发

```c
static int custom_acq_xfer(struct spi_device *spi, u8 cmd, u32 data, u8 *rx)
{
	u8 tx[5];
	struct spi_transfer t = {
		.tx_buf = tx,
		.rx_buf = rx,
		.len = 5,
	};
	...
	return spi_sync_transfer(spi, &t, 1);
}
```
**`struct spi_transfer` 这个写法是固定模板**：SPI 是全双工总线，一次
传输要同时指定"发什么"（`tx_buf`）和"收到的放哪"（`rx_buf`），加上长
度 `len`。`spi_sync_transfer(spi, &t, 1)` 意思是"同步（阻塞等结果）执
行这一个 transfer"——这三行结构体 + 一行调用，是内核里做一次 SPI 收发
的标准写法，任何 SPI driver 单次收发基本都长这样。

**函数体内把 5 字节塞成 `[cmd, 4字节大端数据]` 是我们自己的协议**，跟
`testv13.py` 里 `xfer()` 函数的 `tx = [cmd, ...]` 是同一个东西的两种实
现（一个跑在 userspace Python 里，一个跑在内核里）。

## 55–78 行：`custom_acq_reg_read()` —— 读一个寄存器

**这整个函数是这次业务相关**，直接对应 `testv13.py` 的 `read_reg()`：
发地址帧 → 等 500us（`INTER_FRAME_US`，配合固件 ISR 处理节奏）→ 发 NOP
帧拿到上一帧的回应 → 检查 echo 字节对不对（协议自带的错误检测）→ 拼出
32 位值。这是把 Python 版协议逻辑原样搬到内核里。

## 80–106 行：`device_id_show()` / `fw_version_show()` + `DEVICE_ATTR_RO`

```c
static ssize_t device_id_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct spi_device *spi = to_spi_device(dev);
	...
	return sysfs_emit(buf, "0x%08x\n", val);
}
static DEVICE_ATTR_RO(device_id);
```
**函数签名和 `DEVICE_ATTR_RO` 宏是固定模板。** 内核规定："想在 sysfs
下暴露一个只读文件"，就必须提供一个长这个签名的 `xxx_show()` 函数
（`struct device *dev, struct device_attribute *attr, char *buf` 参数
顺序不能变），然后用 `DEVICE_ATTR_RO(名字)` 宏把它包装成系统认识的属
性对象——这个宏会自动生成一个叫 `dev_attr_device_id` 的变量，命名规则
是 `dev_attr_` + 你传给宏的名字，这也是固定的。

`to_spi_device(dev)` 这行是固定套路：sysfs 回调只给你一个通用的
`struct device *`，要转换回具体类型（这里是 `spi_device`）才能用得上
`custom_acq_reg_read()`，`to_spi_device()` 就是干这个转换的标准宏。

函数体里"调用 `custom_acq_reg_read` 读哪个寄存器、按什么格式打印"是这
次业务相关。

**效果**：每次有人 `cat /sys/bus/spi/devices/spi0.0/device_id`，就会
触发一次真正的 SPI 通信去问 MCU 要最新值——不是读一个缓存的静态数字。

## 108–113 行：属性组

```c
static struct attribute *custom_acq_attrs[] = {
	&dev_attr_device_id.attr,
	&dev_attr_fw_version.attr,
	NULL,
};
ATTRIBUTE_GROUPS(custom_acq);
```
**固定模板。** "把多个 `DEVICE_ATTR_RO` 生成的属性收集成一个数组、用
`NULL` 结尾、再用 `ATTRIBUTE_GROUPS` 包一层"是内核暴露一组 sysfs 文件
的标准写法。`ATTRIBUTE_GROUPS(custom_acq)` 会自动生成一个叫
`custom_acq_groups` 的变量（规则：你传的名字 + `_groups`），下面
`struct spi_driver` 里会用到。数组里具体列了哪两个属性是业务相关。

## 115–140 行：`custom_acq_probe()` —— 驱动入口

这是内核匹配成功后**自动调用**的函数，整个函数体现了这次业务要做的初
始化工作，但里面几个具体调用是固定套路：

```c
priv = devm_kzalloc(&spi->dev, sizeof(*priv), GFP_KERNEL);
```
**固定套路，但概念值得真正理解（不是死记）**：`devm_` 前缀的分配函数
（"device-managed"）意思是"这块内存跟设备的生命周期绑定，设备被移除
（`remove()` 或 probe 失败）时内核自动帮你释放，不用自己手写清理代
码"。这是现代内核 driver 推荐的写法，比手动 `kzalloc` + 在 `remove()`
里 `kfree` 更不容易出错（少一次"忘记释放"或"重复释放"的机会）。

```c
spi_set_drvdata(spi, priv);
```
**固定套路**：把我们的私有结构体指针"挂"在这个 spi_device 对象上，以
后任何拿到这个 `spi_device` 指针的地方（比如以后写 IRQ handler），都
能用 `spi_get_drvdata(spi)` 把它取回来。这是内核里"每个 device 对象自
带一个指针空间，driver 可以用来存自己的私有数据"这个通用机制。

```c
spi->mode = SPI_MODE_0;
spi->bits_per_word = 8;
ret = spi_setup(spi);
```
**这次业务相关**：Mode 0（时钟空闲低电平、上升沿采样）、8 位一帧，这
是跟 MCU 固件 SPI 外设配置对应的参数，必须匹配硬件实际配置，换个协议
这两个值可能就不一样。`spi_setup()` 是固定要调用的函数——把这些参数
真正下发给 SPI 控制器硬件。

```c
ret = custom_acq_reg_read(spi, REG_DEVICE_ID, &device_id);
...
dev_info(&spi->dev, "custom-acq bound, DEVICE_ID=0x%08x\n", device_id);
```
**这次业务相关**：这是我们自己加的"验证一下真的连上了"的动作——
probe 阶段就顺手读一次 DEVICE_ID，读不到就直接失败（`dev_err_probe`
返回错误，内核会认为这个设备绑定失败），读到了就打进内核日志，等下
`dmesg` 就能看到。这不是每个 driver 必须做的事，是我们选择在 probe
里加的一个自检。

## 142–152 行：两张匹配表

```c
static const struct of_device_id custom_acq_of_match[] = {
	{ .compatible = "edp,custom-acq" },
	{}
};
MODULE_DEVICE_TABLE(of, custom_acq_of_match);

static const struct spi_device_id custom_acq_spi_id[] = {
	{ "custom-acq", 0 },
	{}
};
MODULE_DEVICE_TABLE(spi, custom_acq_spi_id);
```
**表的结构是固定模板**（数组 + 空结构体 `{}` 结尾表示"到此为止" + 用
`MODULE_DEVICE_TABLE` 宏登记），**表里的内容是业务相关**。

为什么有两张表，不是一张？
- `custom_acq_of_match`：Device Tree 匹配用的，`compatible` 字符串要
  跟 overlay 里 `custom-acq@0` 节点的 `compatible = "edp,custom-acq"`
  完全一致——这就是我们前面讲的"暗号"匹配机制的另一半。
- `custom_acq_spi_id`：给**没有 Device Tree**的场景准备的备用匹配方式
  （比如老式 x86 平台用板级代码手工注册设备，或者某些 ACPI 系统）。
  在树莓派这种纯 Device Tree 平台上，这张表理论上用不到，但写上是
  SPI driver 的标准惯例（防御性、也让代码更完整），不写也不会出错。

`MODULE_DEVICE_TABLE` 这个宏还有一个隐藏作用：让 `depmod`/`modprobe`
能根据这些暗号自动建索引，是"开机自动加载对应 driver"那套机制依赖的
元信息（我们现在手动 `insmod`，暂时用不上，但表还是要写对）。

## 154–163 行：driver 主结构体 + 注册

```c
static struct spi_driver custom_acq_driver = {
	.driver = {
		.name = "custom-acq",
		.of_match_table = custom_acq_of_match,
		.dev_groups = custom_acq_groups,
	},
	.probe = custom_acq_probe,
	.id_table = custom_acq_spi_id,
};
module_spi_driver(custom_acq_driver);
```
**整块都是固定模板**——这是"一个 SPI driver 长什么样"的标准骨架，把
前面定义的所有东西（probe 函数、两张匹配表、sysfs 属性组）串起来注册
成一个 `spi_driver` 对象。字段名（`.probe`、`.id_table`、
`.of_match_table`）是内核规定好的，必须用这些名字。

`module_spi_driver(...)` 是一个"偷懒宏"：正常情况你需要自己写
`module_init()`/`module_exit()` 两个函数，分别调用
`spi_register_driver()`/`spi_unregister_driver()`。这个宏帮你把这套样
板代码自动生成好了，只要传入你的 `spi_driver` 结构体变量名。**这是几
乎所有 SPI driver 的最后一步，写法完全固定**。

## 165–167 行：模块元信息

```c
MODULE_AUTHOR("...");
MODULE_DESCRIPTION("...");
MODULE_LICENSE("GPL");
```
**固定模板**（内容除外）。`MODULE_LICENSE("GPL")` 这行比较关键：内核
里很多符号（函数）只导出给声明了 GPL 兼容许可证的模块用，不写这行或
写错，编译/加载时会报 "unknown symbol" 或污染内核（taint）警告更严
重。这三行内容可以随便改，但这一行的值和格式是有讲究的，不能瞎写。

---

# 第二部分：V3"第二版/第三版"新加的代码（GPIO 中断 + kfifo）

**先说一句话总结这部分做了什么**：让 driver 从"只能被动等用户 `cat`
才去读一次寄存器"，变成"MCU 一有新数据，driver 自己主动被硬件叫醒、
把数据全部搬进内核内部的一个缓冲区里存好"——这是往"数据自动流进内核"
迈的一步，`/dev/acq0` 以后要做的，只是把这个缓冲区里的东西倒给用户态。

下面按代码里出现的顺序细讲。

## 新增常量、私有结构体字段

```c
#define REG_CONTROL	0x03
#define REG_FIFO_LEVEL	0x05
#define REG_DATA_SEQ	0x06
#define REG_DATA_VAL	0x07
#define CMD_WRITE_FLAG	0x80
#define SAMPLE_KFIFO_SIZE	128

struct custom_acq_sample {
	u32 seq;
	u32 value;
};
```
**这次业务相关。** 跟第一部分的寄存器常量是一回事——都是照抄固件
`main.c` 里定义好的地址。`struct custom_acq_sample` 是我们自己定义的
"一条采样长什么样"（序号 + 值），对应 MCU 协议里 `REG_DATA_SEQ`/
`REG_DATA_VAL` 这一对寄存器的组合语义。

```c
struct custom_acq {
	struct spi_device *spi;
	struct gpio_desc *data_ready;
	DECLARE_KFIFO(samples, struct custom_acq_sample, SAMPLE_KFIFO_SIZE);
	struct mutex fifo_lock;
	u32 kfifo_overflow;
	wait_queue_head_t data_wq;
	struct miscdevice miscdev;
	struct mutex spi_lock;
};
```
第一部分说过"这个结构体会随功能增长"，这次正好印证了——多了好几个字
段。`struct gpio_desc *` 是**固定类型**（内核对"一根 GPIO 线"的标准
抽象，任何用到 GPIO 的 driver 都用这个类型存）；`DECLARE_KFIFO(...)`
是**固定宏**（内核 `kfifo` 的标准用法，任何想要环形缓冲区的 driver
都这么声明）；哪些字段、为什么需要——`data_ready` 存哪根线、
`kfifo_overflow` 记丢了几条数据、两把锁分别保护什么——这是这次业务
相关的设计决定。

`fifo_lock` 一开始写的是 `spinlock_t`，后来改成了 `struct mutex`——
不是想清楚就一步到位的，是加 `/dev/acq0` 时才发现的问题：
`custom_acq_read()` 要用 `kfifo_to_user()` 把数据拷到用户空间，这个
函数内部会调 `copy_to_user()`，而 `copy_to_user()` 可能触发缺页中断
（page fault，可以睡眠）——这在持有 spinlock 的时候是不允许的。生产
者（IRQ 线程）和消费者（`read()`）两边其实都只跑在 process context
（进程上下文，允许睡眠），从来不在 hard IRQ 里碰这把锁，所以换成
mutex 没有任何副作用，还顺便解决了 `kfifo_to_user()` 的睡眠问题。
`wait_queue_head_t data_wq` 和 `struct miscdevice miscdev` 是这次
`/dev/acq0` 新加的，作用见下面单独一节。

## `custom_acq_reg_write()` —— 跟 `reg_read` 对称的写操作

跟第一部分的 `custom_acq_reg_read()` 结构完全一样（地址帧 → 等
`INTER_FRAME_US` → NOP 帧收回显 → 校验回显字节），唯一区别是命令字节
要加上 `CMD_WRITE_FLAG`（`0x80`），而且校验的回显目标是"命令字节本
身"（`cmd = addr | CMD_WRITE_FLAG`），不是单纯的地址——因为写操作和
读操作在协议里用同一个字节表达"我刚才干了什么"，读的回显是地址，写
的回显是"地址+写标志位"。**协议细节是业务相关，函数结构照抄
`reg_read` 是固定套路。**

## `control` / `fifo_level` / `data_val` 三个新 sysfs 接口

`fifo_level_show()`/`data_val_show()` 跟第一部分的
`device_id_show()`/`fw_version_show()` 是一模一样的写法（`xxx_show`
签名 + `DEVICE_ATTR_RO` 宏），**固定模板**，业务区别只在于读的是哪个
寄存器。

`control_store()` 是这份代码第一次用到"可写"的 sysfs 属性：
```c
static ssize_t control_store(struct device *dev, struct device_attribute *attr,
			      const char *buf, size_t count)
{
	...
	ret = kstrtou32(buf, 0, &val);
	...
	ret = custom_acq_reg_write(spi, REG_CONTROL, val & 0x01u);
	...
	return count;
}
static DEVICE_ATTR_WO(control);
```
**函数签名（多了 `const char *buf, size_t count` 两个参数）和
`DEVICE_ATTR_WO` 宏是固定模板**——内核规定"想要一个只写的 sysfs 文
件"必须提供 `xxx_store()` 这个签名的函数，命名规则跟 `_show` 完全对
称（Q5 笔记里讲过的宏机制）。`kstrtou32(buf, 0, &val)` 也是固定套
路：sysfs 传进来的永远是一段文本（`buf` 是字符串，比如 `"1\n"`），
必须先转换成数字才能用，`kstrtou32` 是内核提供的标准字符串转数字函
数。**业务相关的只是"转换成功后拿这个值去写哪个寄存器"这一行。**

## `kfifo_level_show()` / `kfifo_overflow_show()` —— 观测内核内部缓冲区

```c
static ssize_t kfifo_level_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct custom_acq *priv = dev_get_drvdata(dev);
	return sysfs_emit(buf, "%u\n", kfifo_len(&priv->samples));
}
```
跟前面几个 `_show` 函数结构一样，**区别只是这次不发 SPI 通信**——
直接读我们自己维护的 `kfifo` 现在存了多少条（`kfifo_len()`，内核
`kfifo` 自带的查询函数）。这两个接口纯粹是给我们自己调试用的"观察窗
口"，`/dev/acq0` 做好之后，用户态原本就应该直接 `read()` 拿数据，不
需要通过这两个接口。

## `custom_acq_read_sample()` —— 读一条完整采样

```c
static int custom_acq_read_sample(struct spi_device *spi, struct custom_acq_sample *s)
{
	ret = custom_acq_reg_read(spi, REG_DATA_SEQ, &s->seq);
	...
	return custom_acq_reg_read(spi, REG_DATA_VAL, &s->value);
}
```
**这次业务相关**，直接对应固件 `main.c` 里 `REG_DATA_SEQ`（"看一眼"
队首序号，不删除）+ `REG_DATA_VAL`（真正弹出、拿到值）这套"先peek再
pop"协议——这是 MCU 固件自己设计的取数据方式，driver 这边只是照着协
议规则调用两次已有的 `custom_acq_reg_read()`，没有发明新东西。

## `custom_acq_irq_thread()` —— 这次改动的核心

```c
static irqreturn_t custom_acq_irq_thread(int irq, void *data)
{
	...
	for (;;) {
		ret = custom_acq_reg_read(priv->spi, REG_FIFO_LEVEL, &level);
		...
		if (level == 0)
			break;

		ret = custom_acq_read_sample(priv->spi, &s);
		...
		mutex_lock(&priv->fifo_lock);
		if (!kfifo_put(&priv->samples, s))
			priv->kfifo_overflow++;
		mutex_unlock(&priv->fifo_lock);
		...
	}
	if (drained)
		wake_up_interruptible(&priv->data_wq);
	return IRQ_HANDLED;
}
```
函数签名 `irqreturn_t xxx(int irq, void *data)`、返回 `IRQ_HANDLED`
是**固定模板**——内核规定中断处理函数必须长这样。`mutex_lock`/
`mutex_unlock` 包住 `kfifo_put` 也是**固定套路**（内核 `kfifo` 文档
就是这么要求的：多个执行流会碰这个队列时，用锁保护每一次存取；这里
用 mutex 而不是 spinlock 的原因见上一节）。`wake_up_interruptible`
是这次 `/dev/acq0` 加的一行——排空循环结束后，只要真的搬了新数据
（`drained > 0`），就把等待队列上睡着的 `read()`/`poll()` 都叫醒。

**业务相关、也是这次最值得记的设计**：为什么要在循环里**每次都重新
读一遍** `REG_FIFO_LEVEL`，而不是像最初写的那样，进循环前读一次、之
后靠本地变量递减？——这里之前确实是这么写的（`while (level > 0) {
... level--; }`），V4 阶段真拿一个持续采集的场景去测试才发现了问题
（`docs/debugging/case-05-irq-thread-stale-fifo-level-snapshot.md`
完整记录）：DATA_READY 是"电平"信号（case-03 验证过：有数据=高、没
数据=低），但 GPIO 中断是"边沿"触发（`IRQF_TRIGGER_RISING`）——只要
电平持续保持高，就只会在最开始触发**一次**中断。如果 MCU 产生数据的
速度比我们读取的速度快（协议本身每条数据要好几次两帧 SPI 操作，有实
打实的开销），电平会一直保持高、不会再回落，那"进循环前读一次"这个
快照就会很快过时——循环按快照读到的数量读完就退出了，但 MCU 其实还
在不停产生新数据，而且再也不会有第二次中断把我们叫醒去读它们，这些
数据只能在 MCU 自己的硬件 FIFO 里悄悄溢出（`fifo_overflow` 计数器，
MCU 侧的，这份 driver 目前完全没有读取/暴露它）。改成循环里每次都重
新问一遍 MCU"你现在真的还有数据吗"，才能保证只要数据还在持续产生，
这个线程就会一直排空下去，直到 MCU 真正报告"空了"为止——这才是跟
DATA_READY 的电平语义匹配的正确写法。`kfifo_put()` 返回 `false` 表
示我们自己的内核缓冲区满了（跟 MCU 硬件那个 FIFO 是两回事，是我们自
己 128 条深度的队列），这时候用 `kfifo_overflow` 计数记录丢了多少
条，思路跟固件自己那个 `fifo_overflow` 计数器是同一个套路（MCU 满了
也这么记）。

## `probe()` 里新增的部分——GPIO + 中断注册

```c
priv->data_ready = devm_gpiod_get(&spi->dev, "data-ready", GPIOD_IN);
...
ret = gpiod_to_irq(priv->data_ready);
...
ret = devm_request_threaded_irq(&spi->dev, ret, NULL, custom_acq_irq_thread,
				 IRQF_TRIGGER_RISING | IRQF_ONESHOT,
				 "custom-acq", priv);
```
**这三步的调用方式是固定套路**（Q9/Q10/Q11 笔记详细记过原理：
`"data-ready"` 这个字符串对应 DT 里的 `data-ready-gpios` 属性，
`gpiod_to_irq` 把 GPIO 线翻译成中断号，`devm_request_threaded_irq`
注册处理函数,`NULL` 表示不用硬中断上下文的那一层）。**业务相关的
是选择了 `IRQF_TRIGGER_RISING`**（只在上升沿触发,对应我们的信号语
义）。

同一批还加了：
```c
INIT_KFIFO(priv->samples);
mutex_init(&priv->fifo_lock);
mutex_init(&priv->spi_lock);
init_waitqueue_head(&priv->data_wq);
```
四个初始化调用都是**固定套路**（任何用到这些内核基础设施的 driver,
用之前都要这样初始化一次），顺序上要放在"这些字段第一次可能被用到
之前"——`mutex_init` 必须在第一次调用 `custom_acq_reg_read()` 之前
（因为读写函数内部现在会去拿这把锁），这一点是这次改动里容易踩坑的
细节。

## `/dev/acq0`（misc device）—— 让用户空间能读到 kfifo 里的数据

这是这次改动新加的一整块，`custom_acq_open/release/read/poll` 四个
函数 + `custom_acq_fops` 这张表 + `probe()` 里的注册代码。原理详见
`docs/learning-qa.md` Q25，这里按"固定模板 vs 业务相关"补一下代码层
面的细节。

```c
static int custom_acq_open(struct inode *inode, struct file *file)
{
	struct miscdevice *mdev = file->private_data;
	struct custom_acq *priv = container_of(mdev, struct custom_acq, miscdev);
	file->private_data = priv;
	return 0;
}
```
`open()` 的签名是**固定模板**。函数体这几行是**固定套路**：misc
device 框架在调用我们的 `open()` 之前，会先把 `file->private_data`
设成指向 `struct miscdevice` 的指针（这是框架自己的行为，不是我们写
的），所以第一次进来时它指向的是 `priv->miscdev` 这个内嵌字段,不是
整个 `priv`。用 `container_of()` 从"内嵌字段的地址"反推出"外层结构
体的地址"（这个宏在第一部分的 `spi_get_drvdata`/`dev_get_drvdata`
链路里没直接出现过，但原理是一回事——都是"已知一个东西在结构体里的
相对位置，反着算出结构体首地址"），然后把 `private_data` 换成
`priv`，这样后面 `read()`/`poll()` 就能直接拿到完整的 `priv`，不用
每次都再 `container_of` 一遍。

```c
static ssize_t custom_acq_read(struct file *file, char __user *buf, size_t count, loff_t *ppos)
{
	struct custom_acq *priv = file->private_data;
	count -= count % sizeof(struct custom_acq_sample);
	if (count == 0)
		return -EINVAL;

	if (kfifo_is_empty(&priv->samples)) {
		if (file->f_flags & O_NONBLOCK)
			return -EAGAIN;
		ret = wait_event_interruptible(priv->data_wq, !kfifo_is_empty(&priv->samples));
		if (ret)
			return ret;
	}

	mutex_lock(&priv->fifo_lock);
	ret = kfifo_to_user(&priv->samples, buf, count, &copied);
	mutex_unlock(&priv->fifo_lock);
	...
	return copied;
}
```
`read()` 的签名 `(struct file *, char __user *, size_t, loff_t *)`
是**固定模板**（内核规定字符设备的 read 必须长这样，`__user` 只是
个标注，提醒这是用户空间指针不能直接解引用，得用专门的拷贝函数）。
`wait_event_interruptible(等待队列, 条件)` 也是**固定套路**：给它一
个等待队列和一个条件表达式，它自己处理"条件已经成立就不睡直接返回、
不成立就挂起、被唤醒后重新检查条件、收到信号就提前返回"这一整套逻
辑，不用自己写循环。**业务相关的只是"条件"这一项**（这里是
`!kfifo_is_empty(...)`）和"数据怎么拷出去"（`kfifo_to_user()`，
`kfifo` 自带的、专门对接用户空间的接口，内部做的就是
`copy_to_user()`）。`count -= count % sizeof(...)` 这行是业务决定：
只返回整条整条的采样，不返回半条。

```c
static __poll_t custom_acq_poll(struct file *file, poll_table *wait)
{
	struct custom_acq *priv = file->private_data;
	__poll_t mask = 0;
	poll_wait(file, &priv->data_wq, wait);
	if (!kfifo_is_empty(&priv->samples))
		mask |= EPOLLIN | EPOLLRDNORM;
	return mask;
}
```
`poll()` 的签名和 `poll_wait()` 的调用方式是**固定模板**——
`poll_wait()` 不会真的让进程睡眠，只是把当前进程"登记"到这个等待队
列上（跟 `read()` 用的是同一个 `data_wq`），真正的睡眠和被唤醒是
`select`/`poll`/`epoll` 系统调用自己在更外层处理的。**业务相关的只
是"用什么条件判断可读"**（`!kfifo_is_empty(...)`，跟 `read()` 里判
断要不要阻塞用的是同一个条件）和返回哪个标志位（`EPOLLIN` 表示"有数
据可读"）。

```c
priv->miscdev.minor = MISC_DYNAMIC_MINOR;
priv->miscdev.name = "acq0";
priv->miscdev.fops = &custom_acq_fops;
ret = misc_register(&priv->miscdev);
...
ret = devm_add_action_or_reset(&spi->dev, custom_acq_misc_deregister, &priv->miscdev);
```
`MISC_DYNAMIC_MINOR`（让内核自动分配次设备号，不用自己管理）和
`misc_register()` 的调用方式是**固定套路**。`"acq0"` 这个名字是业务
相关（决定了设备节点叫 `/dev/acq0`）。踩过的一个坑：一开始想当然地
写了 `devm_misc_register()`，以为跟 `devm_gpiod_get`、
`devm_request_threaded_irq` 一样有个自动清理版本——实际上内核里根本
没有这个函数，编译直接报 `implicit declaration of function`。正确
做法是普通的 `misc_register()`，再手动用 `devm_add_action_or_reset()`
把"卸载时要调用 `misc_deregister()`"这个清理动作登记成一个 devres
资源，效果和其他 `devm_*` 一样——`probe()` 出错或者 driver 被卸载
时自动执行，不用自己在 `.remove` 里写。

## 并发 bug（case-04）为什么会发生、怎么修的

已经详细写在
`docs/debugging/case-04-spi-transaction-race-two-frame-protocol.md`
里，这里只留一句话摘要：加中断之前，同一时刻只会有"用户主动 `cat`"
这一种方式在跟 MCU 说话，天然不会撞车；加了中断之后，`echo 1 >
control` 触发采集的同时,中断线程几乎立刻就会被数据到达唤醒、两边同
时想跟 MCU 通信,而"发地址帧 → 等 500us → 发 NOP 帧"这个两帧一组的
协议中间那段空隙没有加锁保护，会被另一边插队、打乱帧序。修复是给
`custom_acq_reg_read()`/`custom_acq_reg_write()` 整个套上
`mutex_lock`/`mutex_unlock`，把"一次完整的寄存器操作"变成不可分割
的临界区。

---

## 一句话总结：这份代码里真正"这次业务专属"的只有这几块

1. 常量表（寄存器地址）
2. `custom_acq_xfer` / `custom_acq_reg_read` 的协议实现细节
3. `device_id_show` / `fw_version_show` 具体读哪个寄存器
4. `probe()` 里 SPI 参数（mode/bits_per_word）和"读 DEVICE_ID 自检"这个逻辑
5. 两张匹配表里的字符串（`"edp,custom-acq"` / `"custom-acq"`）
6. `custom_acq_read_sample()` 的 peek+pop 协议细节
7. `custom_acq_irq_thread()` 里"为什么要循环排空"这个设计决定
8. `control`/`fifo_level`/`data_val`/`kfifo_level`/`kfifo_overflow`
   具体读写哪个寄存器/哪个内部状态
9. `IRQF_TRIGGER_RISING` 这个触发方式的选择
10. `custom_acq_read()`/`custom_acq_poll()` 里判断"可读"用的条件
    （`!kfifo_is_empty(...)`）和 `count` 按整条采样对齐这条业务规则
11. `"acq0"` 这个设备名，决定了节点叫 `/dev/acq0`

其余的结构（私有数据结构体模式、`devm_kzalloc`、sysfs 属性宏、driver
注册骨架、`module_spi_driver`、`gpiod_get`/`gpiod_to_irq`/
`request_threaded_irq` 的调用方式、`kfifo`/`mutex`/等待队列的初始化
和加锁写法、misc device 的 `open`/`read`/`poll` 固定签名和
`file_operations` 表的搭法）都是**换任何类似的中断驱动采集类
driver 都长这样**的固定套路，理解一次，以后照抄结构、只换业务细节。

---

# 第三部分：V7 新加的代码（hard-IRQ 时间戳）

**一句话总结**：Plan.md V7 要测"MCU 产生数据到 userspace 收到"这条链
路的延迟，第一步是让 driver 能报告"内核什么时候第一次反应到这次中
断"，作为这条延迟链的起点之一。

```c
static irqreturn_t custom_acq_irq_hard(int irq, void *data)
{
	struct custom_acq *priv = data;
	priv->irq_ts_ns = ktime_get_ns();
	return IRQ_WAKE_THREAD;
}
```
**函数签名、返回 `IRQ_WAKE_THREAD` 是固定模板**——这是内核"两段式中
断处理"（threaded IRQ）机制规定的：`devm_request_threaded_irq()` 可
以同时注册一个硬中断上半部（"hard handler"，第三个参数）和一个线程
下半部（第四个参数，也就是 `custom_acq_irq_thread`）。之前这个项目只
注册了下半部（第三个参数传 `NULL`）——原因是原来没有任何逻辑需要在硬
中断上下文（不能睡眠）里跑。这次新加硬中断上半部**唯一的目的就是尽
早拿一个时间戳**：`ktime_get_ns()` 是固定 API（内核标准的"现在几纳
秒"查询,底层是 CLOCK_MONOTONIC 语义,不受系统时间被手动调整影响),
**放在硬中断里调用是业务决定**——硬中断上下文是内核对这次中断反应最
早的时刻,比线程下半部（要等内核调度器真的把这个线程排上 CPU 才会执
行,中间可能有调度延迟)更接近"真实中断到达时间"。返回 `IRQ_WAKE_THREAD`
是固定套路：告诉内核"硬中断部分做完了,去唤醒线程部分接着跑"。

```c
struct custom_acq_sample {
	u32 seq;
	u32 value;
	s64 irq_ts_ns;
};
```
在原来 `seq`/`value` 后面加了 `irq_ts_ns` 字段，**这是这次业务决定**：
让每条从内核传给用户态的采样都自带"这次中断是什么时候到的"，`/dev/acq0`
的调用方本来就在读这个结构体，加一个字段不用另开一条新通道。字段放在
最后、用 `s64` 是为了不引入内存对齐的空隙（两个 `u32` 共 8 字节，
`s64` 天然对齐在 8 字节边界上，凑巧不需要 padding）。

```c
if (drained)
	...
s.irq_ts_ns = priv->irq_ts_ns;
```
在 `custom_acq_irq_thread()` 排空循环里，每读到一条样本就把
`priv->irq_ts_ns`（硬中断里存的那个时间戳）复制进去，再 `kfifo_put()`。
**业务决定，值得记住的限制**：DATA_READY 是电平信号，一次硬中断触发
之后，线程可能在循环里一口气排空 MCU 好几条数据（前面案例 05 讲过的
"为什么要循环重读 FIFO_LEVEL"）——这些数据实际上不是同一时刻产生的，
但目前只有"这次中断什么时候到"这一个时间戳，同一批里的每条样本都会
共享同一个值。这只能测出"IRQ 到 userspace"这一段延迟的上界近似，不
是逐条样本的精确产生时刻——要做到逐条精确，需要 MCU 侧每产生一条数据
就翻转一次专用测量引脚，配合逻辑分析仪单独测（这部分还没做，是 V7
后续阶段的工作）。

**真机验证过这个限制的严重程度，比预想的大得多**：抓了一段真实运行
的 CSV，发现设备持续满载运行时，`irq_ts_ns` 可以连续近一万条样本共
享同一个值（对应线程下半部一次排空循环几十秒没退出过，因为
`DATA_READY` 电平持续保持高——本质是 case-05 记录过的吞吐瓶颈：
device-service 大约 520 samples/sec，追不上 MCU 产生速度）。也就是
说这个指标在设备持续满载时基本不可用，得先解决吞吐问题（或者换一种
不依赖"同一批样本是否共享一次中断"的测量口径），这个延迟指标才有
意义——详见 `docs/session-log.md` 2026-09-04 第三轮的记录。

**2026-09-07 更新：上面说的"这部分还没做"现在做了。** `struct
custom_acq` 加了个可选字段：

```c
struct gpio_desc *irq_marker;
bool irq_marker_state;
```

`probe()` 里用 `devm_gpiod_get_optional()`（不是 `devm_gpiod_get()`）
拿这个 GPIO——"optional" 意味着如果设备树里没配这个属性，
`priv->irq_marker` 就是 `NULL`，驱动照常工作，不会报错，老的 overlay
不会因为这个改动被破坏。`custom_acq_irq_hard()` 里紧跟着打时间戳之后
多了几行：

```c
if (priv->irq_marker) {
	priv->irq_marker_state = !priv->irq_marker_state;
	gpiod_set_value(priv->irq_marker, priv->irq_marker_state);
}
```

每次硬中断触发就翻转一次这个引脚（`device-tree/custom-acq-overlay.dts`
里配的是树莓派 `GPIO27`，物理引脚 13）。配合 MCU 固件那边新加的
`PA9`（每产生一条样本翻转一次，`v1-spi-slave-handshake/v1.3`），用逻
辑分析仪同时抓两个引脚——**两个事件现在落在分析仪自己同一个时钟上，
不需要跨时钟域对齐**，直接量出"MCU 产生样本"到"树莓派硬中断响应"这
一段真实延迟。真机测出来中位数 3.5us、最大 10.4us（`docs/
performance.md`"MCU-produced to hard-IRQ"一节），证实这一段极小、几
乎可以忽略，瓶颈完全在硬中断之后的软件路径上。

```c
ret = devm_request_threaded_irq(&spi->dev, ret, custom_acq_irq_hard,
				 custom_acq_irq_thread,
				 IRQF_TRIGGER_RISING | IRQF_ONESHOT,
				 "custom-acq", priv);
```
跟原来的区别只是把第三个参数从 `NULL` 换成了 `custom_acq_irq_hard`，
其余不变——**固定的注册方式，换的只是"要不要提供硬中断上半部"这个业
务选择**。

## V7 顺带加的两样东西：`inter_frame_us` 模块参数 + Case 06 诊断计数器

这两样跟延迟时间戳没有直接关系，是趁着这一轮改动，把 case-06（SPI 控
制器卡死，还没根因定位）"Next steps" 清单里两个可以不用上真机就先做
好的准备工作提前做掉：

```c
static unsigned int inter_frame_us = 500;
module_param(inter_frame_us, uint, 0644);
MODULE_PARM_DESC(inter_frame_us, "...");
```
把原来写死的 `#define INTER_FRAME_US 500` 换成了 `module_param`。
**`module_param` 宏本身是固定套路**——内核标准的"允许 `insmod`/
`modprobe` 时传参数，或者事后通过 `/sys/module/custom_acq/parameters/
inter_frame_us` 读写"这套机制。**改成可调参数是业务决定**：case-06
文档里怀疑协议这 500us 帧间隔可能是导致 RP1 SPI 控制器卡死的一个诱因，
之前想验证这个假设得改代码重新编译，现在只要
`echo 800 > /sys/module/custom_acq/parameters/inter_frame_us` 就能试
不同的值，不用重新烧模块。

**这个参数后来真的在真机上扫过一遍，而且直接改变了默认值**：
2026-09-04 用这个参数在真机上系统性地测了 500/300/250/200/150/100/
50/20/10/0 这几个值，发现关系完全不是线性的——200-300us 反而是个"坑"
（比 500us 和更激进的值都更容易丢数据/kfifo 溢出），继续调低到
100us 以下才真正追上 MCU 的产生速度（吞吐从 ~520/s 冲到 ~1000/s，
`gap_count`/`kfifo_overflow` 大多数时候是 0）。默认值改成了
`100`（原来的 `500` 从没被真机数据验证过，代码里那句"经验上固件需要
的余量"其实只是抄自 `testv13.py` 的旧假设，这次实测直接推翻了）。完
整数据表和方法论见 `docs/session-log.md` 2026-09-04 第四轮的记录。
这次顺带还发现，吞吐追上之后，V7 那个 IRQ 时间戳延迟指标也终于变得
有意义了——之前吞吐跟不上时，`irq_ts_ns` 会连续上万条样本共享同一个
值（前面那节讲过的批处理污染问题），现在同一段时间能看到一万多个不
同的时间戳值，测出来的延迟稳定在 1ms 以内，这才是这个指标本来该有的
样子。

```c
static ssize_t spi_rearm_fail_show(...) { ... REG_SPI_REARM_FAIL ... }
static ssize_t spi_error_count_show(...) { ... REG_SPI_ERROR_COUNT ... }
```
跟 `device_id_show`/`fw_version_show` 完全一样的写法（**固定模板**），
读的是 MCU 固件里本来就有、但这份 driver 之前从来没读过的两个寄存器
（`0x09`/`0x0A`）——固件自己的 SPI 错误恢复逻辑（`HAL_SPI_ErrorCallback`
等）会累加这两个计数器，之前只是没人从 Linux 这边去看。**暴露成 sysfs
是业务决定**：下次真的复现 case-06 的卡死，可以顺手看一眼这两个数字有
没有涨，帮助判断问题是不是出在 MCU 固件的重新武装（re-arm）逻辑这一
侧，而不是只能瞎猜。这两个计数器本身不会主动修复或影响任何行为，纯粹
是诊断用的观察窗口，跟 `kfifo_level`/`kfifo_overflow` 是同一个定位。
