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

## 一句话总结：这份代码里真正"这次业务专属"的只有这几块

1. 常量表（寄存器地址）
2. `custom_acq_xfer` / `custom_acq_reg_read` 的协议实现细节
3. `device_id_show` / `fw_version_show` 具体读哪个寄存器
4. `probe()` 里 SPI 参数（mode/bits_per_word）和"读 DEVICE_ID 自检"这个逻辑
5. 两张匹配表里的字符串（`"edp,custom-acq"` / `"custom-acq"`）

其余的结构（私有数据结构体模式、`devm_kzalloc`、sysfs 属性宏、driver
注册骨架、`module_spi_driver`）都是**换任何 SPI driver 都长这样**的固
定套路，理解一次，以后照抄结构、只换业务细节。
