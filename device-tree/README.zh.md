# custom-acq Device Tree overlay —— 中文版（学习用）

英文版 `README.md` 是给外部读者看的工程记录，这份是给自己学习用的，尽量
把"为什么这么做"讲清楚，不只是翻译命令。

目标设备：树莓派 5（`RaspberryPi5`，192.168.178.172），Raspberry Pi OS
（Debian 13 "trixie"），内核 `6.18.39+rpt-rpi-2712`。

---

## 一、这个 overlay 到底在解决什么问题

先回忆一下背景：Linux 怎么知道某个 SPI 从设备该用哪个 driver？靠的是
**Device Tree**——一份描述硬件长什么样的静态数据（这个 SPI 总线上挂了什
么芯片、用第几个片选、时钟多快）。内核启动时读这份数据，找到匹配的
driver，调用它的 `probe()` 函数。

树莓派出厂默认在 `config.txt` 里写了 `dtparam=spi=on`，这行配置会让
`spi0` 总线的两个片选（CS0、CS1）都自动挂载**官方通用 driver
`spidev`**。`spidev` 的作用就是"什么协议都不懂，纯粹把 SPI 总线开放给
userspace"——这正是我们 V1/V2 阶段用 Python `spidev.SpiDev()` 读写 MCU
寄存器时依赖的东西。

现在要写自己的 kernel driver 了，问题来了：**CS0 现在已经被 `spidev`
占用**，如果我们的 driver 也想绑到 CS0 上，两个 driver 会打架（Device
Tree 里同一个片选只能对应一个"设备节点"，也就只能绑一个 driver）。

所以 overlay 要做两件事：
1. 把 `spidev` 在 CS0 上的节点关掉（`status = "disabled"`），腾出位置。
2. 在 `spi0` 下面新建一个节点，写上 `compatible = "edp,custom-acq"`——
   这个字符串就是"暗号"，我们自己写的 driver 里声明同样的暗号
   （`of_device_id` 表），内核启动匹配的时候，就会把这个节点交给我们的
   driver 处理，自动调用它的 `probe()`。

`compatible` 属性是标准约定，格式是 `"厂商,型号"`。因为这是我们自己设
计的板子，不是买来的现成芯片，`edp`（embedded-linux-device-platform 的
缩写）就是自己起的"厂商名"，不需要真的注册，纯粹是个唯一标识。

## 二、overlay 源码 `custom-acq-overlay.dts` 逐段讲解

```dts
/dts-v1/;
/plugin/;
```
声明这是一份"插件式" overlay，不是完整的设备树，而是打补丁用的（在运
行时合并进基础 dtb）。

```dts
fragment@0 {
    target = <&spidev0>;
    __overlay__ {
        status = "disabled";
    };
};
```
`fragment@0` 是补丁的第一块。`target = <&spidev0>` 意思是"找到基础设备
树里标签叫 `spidev0` 的那个节点"（这个标签是树莓派官方 dtb 自带的，可
以在 Pi 上用 `ls /proc/device-tree/__symbols__/` 看到）。找到之后往里
面塞一个属性：`status = "disabled"`，相当于告诉内核"这个节点先别用"。

```dts
fragment@1 {
    target = <&spi0>;
    __overlay__ {
        #address-cells = <1>;
        #size-cells = <0>;
        status = "okay";

        custom_acq: custom-acq@0 {
            compatible = "edp,custom-acq";
            reg = <0>;
            spi-max-frequency = <1000000>;
            status = "okay";
        };
    };
};
```
`fragment@1` 是第二块补丁，目标是 `spi0` 这条总线本身（不是某个片选节
点，是总线控制器）。在总线下面新增一个子节点 `custom-acq@0`：
- `reg = <0>` —— 片选号 0，也就是 CS0，跟被我们禁用的 `spidev0` 是同一
  个物理引脚。
- `compatible = "edp,custom-acq"` —— 上面说的"暗号"。
- `spi-max-frequency = <1000000>` —— 1MHz，跟 `testv13.py` 里
  `spi.max_speed_hz = 1000000` 保持一致，这是目前验证过稳定工作的速率。

## 三、编译

```bash
dtc -@ -I dts -O dtb -o custom-acq.dtbo custom-acq-overlay.dts
```
`dtc` 是 Device Tree Compiler，把人能读的 `.dts` 文本编译成内核认的二
进制 `.dtbo`（overlay 版本的 `.dtb`）。`-@` 这个参数很关键：它让编译输
出保留 `__symbols__` 信息，overlay 里 `target = <&spi0>` 这种"按标签找
节点"的写法，正是靠运行时把这份 `__symbols__` 跟基础 dtb 的
`__symbols__` 对上号才能工作的。

已经在 Pi 上编译过一次，生成了 678 字节的 `custom-acq.dtbo`，没有报错
或警告。

## 四、安装步骤（还没做，等你确认）

```bash
# 1. 先备份 config.txt —— 这样万一出问题，恢复只需要把备份文件拷回去
sudo cp /boot/firmware/config.txt /boot/firmware/config.txt.bak-pre-custom-acq

# 2. 把编译好的 overlay 放到系统认的目录
sudo cp ~/device-tree/custom-acq.dtbo /boot/firmware/overlays/

# 3. 告诉引导程序：启动时要加载这个 overlay
echo 'dtoverlay=custom-acq' | sudo tee -a /boot/firmware/config.txt

# 4. overlay 只在启动时被加载合并进设备树，改完必须重启才生效
sudo reboot
```

**这一步会实际改变什么**：重启后 `/dev/spidev0.0` 会消失（因为它对应
的节点被我们禁用了），如果你手头还有脚本在用 `spidev.SpiDev().open(0,
0)`，会跑不起来，直到把 overlay 撤掉。`/dev/spidev0.1`（CS1）不受影
响，因为我们完全没碰它。

## 五、重启后怎么验证

```bash
# 确认 spi0.0 这个设备节点还在（只是换了主人）
ls /sys/bus/spi/devices/

# 确认它现在的 compatible 字符串是我们自己的
cat /sys/bus/spi/devices/spi0.0/of_node/compatible
# 期望输出：edp,custom-acq

# 看内核日志，确认 spidev0 节点确实被禁用了，
# 同时因为 driver 还没手动加载，这时候还不会看到 probe 成功的信息
sudo dmesg | tail -20

# 手动加载我们写的 driver（现在是 out-of-tree 模块，开发阶段先手动 insmod，
# 不设成开机自动加载）
cd ~/custom-acq && sudo insmod custom_acq.ko

# 期望在 dmesg 里看到类似：custom-acq bound, DEVICE_ID=0x...
sudo dmesg | tail -5

# 通过 sysfs 属性读寄存器 —— 这两个文件是 driver 里 DEVICE_ATTR_RO 建的
cat /sys/bus/spi/devices/spi0.0/device_id
cat /sys/bus/spi/devices/spi0.0/fw_version
```

如果 `device_id` 读出来的值跟之前 Python 脚本读到的 `DEVICE_ID` 一致，
就说明 kernel driver 这条链路——**Device Tree 匹配 → probe() 被调用 →
SPI 全双工传输 → 解析寄存器协议**——全部走通了，这是 V3 的第一个里程
碑。

## 六、怎么撤销（如果哪一步出问题）

```bash
sudo cp /boot/firmware/config.txt.bak-pre-custom-acq /boot/firmware/config.txt
sudo rm /boot/firmware/overlays/custom-acq.dtbo
sudo reboot
```
重启后 `/dev/spidev0.0` 会恢复，`driver/custom-acq` 编译出来的模块就没
有节点可以绑定了（但模块文件本身还在，不影响下次重新启用）。

## 七、进度记录

- 2026-08-28：`custom_acq.ko` 在 Pi 上编译通过（内核头文件版本跟运行内
  核完全一致，不需要交叉编译），`insmod`/`rmmod` 测试过装卸载干净。
  `custom-acq.dtbo` 编译通过，无警告。**overlay 和 config.txt 还没有实
  际安装，等确认后再执行第四节的重启步骤。**
- 2026-08-31：overlay 装上、Pi 重启，`spi0.0` 的 `compatible` 变成
  `edp,custom-acq`，`/dev/spidev0.0` 消失，`/dev/spidev0.1` 不受影响。
  第一次 `insmod` 时 MCU 还没通电：`probe()` 报告成功，但
  `DEVICE_ID` 读出来是 `0x00000000`，读 `fw_version` 直接 I/O 错误——
  这是"MISO 悬空"造成的假阳性：`DEVICE_ID` 地址正好是 `0x00`，悬空读
  回的 `0x00` 恰好跟期望的 echo 字节撞上了，看起来"成功"实则是假数
  据；`fw_version` 地址是 `0x01`，悬空读回的还是 `0x00`，跟期望的
  echo 对不上，被协议里的 echo 校验正确地抓出来报错了。给 MCU 通电后
  不重新加载模块、直接再读一次：
  ```
  $ cat /sys/bus/spi/devices/spi0.0/device_id
  0xac00acc0
  $ cat /sys/bus/spi/devices/spi0.0/fw_version
  0x00010300
  ```
  两个值都跟固件源码 `main.c` 里写死的常量完全一致
  （`DEVICE_ID` = `0xAC00ACC0`，V1.3 固件版本号 = `0x00010300`）。这说
  明整条链路——Device Tree overlay → 内核 `probe()` → 真实 SPI 通信 →
  正确的 MCU 寄存器数据——完整走通了，V3 的第一个里程碑（Plan.md"第一
  版"）完成。

- 2026-08-31（同一天，另一段）：开始 V3"第二版"（GPIO 中断）之前，先接
  好 DATA_READY 线（MCU `PA8` → Pi `GPIO17`，排针第 11 脚），验证这根新
  线到底通不通。完整的排查过程（英文正式版见
  `docs/debugging/case-03-data-ready-gpio-verification.md`）值得记一下
  思路，因为这是个很典型的"一次只能验证一个变量"的调试案例：

  **一开始的测试为什么没用**：想偷懒复用固件里一行遗留的调试代码（每次
  处理一帧 SPI 就翻转一下 `PA8`，之前某次调试留下的），结果 `gpiomon`
  一次边沿都没抓到。这个"没有信号"的结果本身是没法下结论的——它可能是
  接线真的断了，可能是那行调试代码压根没编译进现在跑的固件（源码里有
  不代表 `.elf` 里有），也可能是测试脚本本身有问题（后来确实发现一次：
  通过 SSH 后台启动的 `gpiomon` 进程，`nohup` 之后本以为能扛住会话结
  束，结果还是被杀了——Raspberry Pi OS 的 `systemd-logind` 默认开着
  `KillUserProcesses`，SSH 会话一断，这个用户名下的所有进程都会被清
  掉，`nohup` 单独用不管用）。三个变量混在一起，测不出来根本不知道该
  怀疑谁。

  **手动摸排针的教训**：为了绕开固件这个不确定因素，想直接手动把这根
  拔下来的线去碰 MCU 板子上的 `3V3` 脚，注入一个已知电压，这样测试就
  完全不依赖固件逻辑对不对了——思路是对的，但排针太密，操作时不小心
  碰到别的地方，导致树莓派网络掉线、电源灯变色。事后用 `vcgencmd
  get_throttled` 查过，返回 `0x0`，说明没有欠压/过热记录，板子没事，
  但这种测法风险和操作难度都偏高，改用了更安全的版本：**同样是"绕开固
  件、注入已知电压"的思路，但改成用另一根杜邦线，把这根线接到树莓派自
  己的 `3V3` 脚（排针第 1 或第 17 脚，位置宽松好操作），完全不用再碰
  MCU 那边密集的排针**——同一个思路，换一个风险更低的执行方式。

  **最后真正解决问题的方法**：与其继续想办法"手动验证"，不如让固件真
  的干活。给 `driver/custom-acq/custom_acq.c` 加了三个新的 sysfs 接口
  （`control`/`fifo_level`/`data_val`），让 Pi 能通过真实的采集流程去
  触发 DATA_READY，而不是那行遗留的调试代码。这样做一举两得：既真正测
  试了这根线，又是 V3 后面 `/dev/acq0` 阶段本来就要写的寄存器写入功能，
  不算白做。

  测试过程中还发现调试器连不上 VM 了（`usbip attach` 报 "Device not
  found"），查下来是 Windows 那边给调试器分的 USB `BUSID` 变了（`2-7`
  变成 `2-8`，没重启电脑也会变），这是环境层面的偶发问题，已经记进
  `tools/debug-connect.sh` 和 `tools/README.md`，以后不用重新排查。

  用新加的接口测试后，真相大白：`PA8` 的边沿变化次数精确对应"我们跟
  MCU 通信了几次"，而不是"MCU 什么时候真的有新数据"——说明①遗留的调
  试翻转代码确实还在跑，盖过了真正的逻辑；②真正该负责的
  `update_data_ready_gpio()`，在数据真正入队的那一刻（`main.c` 的
  `HAL_TIM_PeriodElapsedCallback` 里，`fifo_push()` 之后）被注释掉了，
  根本没生效。两处都不是接线问题。修复固件（删掉调试翻转、取消注释真
  正的更新调用）、重新编译烧录后，用"启动采集→停止→读空 FIFO"这套真
  实流程重测，`gpiomon` 精确抓到 1 次上升沿 + 1 次下降沿，跟真实的"有
  数据=高、没数据=低"语义完全对上，接线和固件逻辑都确认没问题，可以开
  始写真正的中断 driver 代码了。

- 2026-08-31（第三段）：V3"第二版"（GPIO threaded IRQ）+"第三版"
  （kfifo）写完并在硬件上验证通过。overlay 里给 `custom-acq` 节点加了
  `data-ready-gpios`（MCU `PA8` → Pi `GPIO17`），driver 用 `gpiod_get`
  + `gpiod_to_irq` + `devm_request_threaded_irq` 注册中断，中断触发时
  把 MCU 硬件 FIFO 里的数据全部读空、塞进一个内核 `kfifo`（新增
  `kfifo_level`/`kfifo_overflow` 两个 sysfs 接口方便观察，`/dev/acq0`
  还没写，暂时没法从用户态直接读到这些数据）。

  中途发现并修复了一个真实的并发 bug（详见
  `docs/debugging/case-04-spi-transaction-race-two-frame-protocol.md`）：
  加了中断之后，`echo 1 > control` 触发启动采集的同时，中断线程几乎立
  刻就会因为第一条数据到达而被唤醒、开始读 `FIFO_LEVEL`——这两个操作
  会同时抢占同一条 SPI 总线，而我们的寄存器读写协议是"两帧一组"的（地
  址帧 + 隔 500us 的 NOP 帧），中间那 500us 的空隙如果被另一个操作插
  队，两边的帧序就全乱了（`dmesg` 里看到期望收到写操作的回显却收到了
  中断线程在读的地址）。加一把互斥锁，把"两帧一组"当成一个不可分割
  的临界区，问题解决。

  用真实的启动/停止测试验证：`kfifo_level` 从 `0` 变成 `1`，没有再出
  现帧错位报错。至此 V3 前三个子里程碑（overlay+基础 driver、GPIO 中
  断、kfifo）全部完成，只剩最后的 `/dev/acq0` 字符设备。

- 2026-09-02：V3"第四版"（`/dev/acq0`）写完并在硬件上验证通过，至此
  V3 四个子里程碑全部完成。用内核的 misc device 框架注册了一个字符设
  备节点，核心是一张 `file_operations` 函数指针表（`open`/`read`/
  `poll`/`release`），把用户空间的系统调用跟驱动里管理 `kfifo` 的代
  码连了起来。新增 `wait_queue_head_t` 等待队列：IRQ 线程每次往
  `kfifo` 塞完数据就 `wake_up_interruptible()` 一下，`read()` 没数据
  时用 `wait_event_interruptible()` 睡眠等待（`O_NONBLOCK` 时改为直
  接返回 `-EAGAIN`），`poll()` 用 `poll_wait()` 登记到同一个等待队
  列，非空时报 `EPOLLIN`。`fifo_lock` 顺带从 `spinlock_t` 换成了
  `struct mutex`——因为 `kfifo_to_user()` 内部会 `copy_to_user()`，可
  能触发缺页中断（可以睡眠），这在自旋锁里是不允许的；生产者和消费
  者都只跑在进程上下文，换成 mutex 没有副作用。

  实现过程中踩了一个坑：一开始写的 `devm_misc_register()` 这个函数根
  本不存在（记混了），编译报 `implicit declaration of function`。改
  用标准的 `misc_register()` + `devm_add_action_or_reset()` 手动登记
  一个自动清理动作，效果跟其他 `devm_*` 资源一样。

  硬件测试全部通过：`insmod`/`rmmod` 全程 `dmesg` 干净无报错，
  `/dev/acq0` 正确出现/消失；`read()` 在数据到达前正确阻塞、到达后正
  确返回，`timeout` 杀死阻塞中的 `dd` 验证了阻塞路径没有死循环空转；
  `poll()` 在采集开始前超时、开始后立刻收到 `EPOLLIN` 事件；
  `kfifo_overflow` 全程为 0。Plan.md V3 的完成标准（`modprobe` 干净、
  `/dev/acq0` 出现、`read()` 拿到数据、`poll()` 正确阻塞唤醒）全部
  满足。

- 2026-09-02（后段）：V4 第一阶段（`userspace/device-service/`，最小
  闭环的 C++ 采集服务）写完，测试过程中意外发现并修复了一个真实的 V3
  驱动 bug，详见
  `docs/debugging/case-05-irq-thread-stale-fifo-level-snapshot.md`。
  简述：`custom_acq_irq_thread()` 排空循环原来只在进入循环前读一次
  `REG_FIFO_LEVEL`，之后用本地变量递减，而不是每次循环都重新问 MCU
  真实状态——DATA_READY 是电平信号但 GPIO 中断是边沿触发，只要电平
  持续保持高就只会触发一次中断；MCU 产生数据的速度（约 1kHz）比协议
  读取速度快，导致快照很快过时、循环提前退出，之后 MCU 自己的硬件
  FIFO 悄悄溢出、再也没有新中断把我们叫醒。V3 自己的测试从没测出来，
  是因为每次都是"起一条、停"这种短平快场景，从没跑够长时间让快照真
  正过时。改成循环里每次都重新读 `REG_FIFO_LEVEL`、只在 MCU 真的报告
  0 时才退出，修复后同样场景吞吐量从 1 条/次暴涨到 2176 条/5秒，序列
  号连续无跳号，`kfifo_overflow` 和 `dmesg` 都正常。

  顺带做了个小工具 `tools/mcu-reset.sh`：纯软件触发的 MCU 硬件复位
  （走调试器 SWD，不经过 SPI），解决"测试完 SPI 还在跑、蜂鸣声关不
  掉"的问题——不用再摸物理复位键。实测验证过：复位前 `kfifo_overflow`
  还在每秒涨几百，复位后 2 秒内完全不再变化。

- 2026-09-02（更后段）：V4 Phase 2 完成——`userspace/device-service/`
  补上 Configuration（nlohmann-json）、Logging（spdlog）、
  MetricsReporter（定时读现有状态打日志，不引入新计数器）、Watchdog
  （ErrorRecovery：超时探活 + 软复位，探活本身失败就转指
  `tools/mcu-reset.sh`）、systemd 集成（`Type=notify` +
  `sd_notify`）、14 个 GoogleTest 单元测试。故障恢复链路实测验证过：
  手动模拟外部关闭采集后，watchdog 探活成功、自动软复位、采集真的恢
  复。3 分钟稳定性测试跑了 81179 条样本无异常。至此 V4 达到 Plan.md
  的完成标准。

- 2026-09-02（再后段）：V5 自动化测试三层都有产出——C++ 单元测试拆出
  独立的 `read_exact()` 拼帧逻辑并补了单测，新增 `tests/integration/`
  （Python/pytest，4 个测试）和 `tests/hardware/stress_test.py`（压测
  脚本）。但过程中发现一个还没根因定位的真实问题：持续读取会导致 Pi
  的 SPI 控制器卡死（IRQ 线程进 `D` 状态，需要物理复位 MCU 才能恢
  复），而且即使没卡死，实测吞吐也比 V4 Phase 2 那天低了一个数量级、
  还是阵发性的（停顿几秒再一次性来一批）。已经排查排除了驱动锁逻辑
  和 MCU 固件本身的问题，怀疑指向树莓派 5 的 RP1 SPI 控制器，但没有
  `ftrace`/逻辑分析仪进一步验证——详见
  `docs/debugging/case-06-spi-controller-stall-under-sustained-load.md`，
  标记为留给 V7（正好是那个阶段该用的工具），V5 这边只做到"测试能检
  测到、优雅报告，不会跟着一起卡死"。

- 2026-09-03（更后段）：V6 第二轮——Plan.md 说的四类自定义 Yocto
  recipe（`yocto/meta-device-platform/`）全部写完、单独验证过，组装
  成 `device-platform-image`，写卡到真实 Pi 5、真实 MCU 通电，端到端
  验证通过：驱动自动加载、`/dev/acq0` 存在、`device-service` 在
  systemd 下是 `active`，SSH 上去能看到真实递增的样本数据（dropbear
  + WiFi，走 wpa_supplicant/systemd-networkd，真实密码完全不在仓库
  里）。踩了两个只有上真机才暴露的坑：`RPI_EXTRA_CONFIG` 必须放在这
  层的全局 `conf/layer.conf` 里，放镜像 recipe 里完全没用（
  `rpi-bootfiles` 是独立于任何具体镜像的共享 recipe）；
  `core-image-minimal` 根本不装 `packagegroup-base`，所以 WiFi 固件/
  内核模块虽然编译出来了但从没进镜像，得在 `IMAGE_INSTALL` 里手动
  加。完整排查过程见 `docs/session-log.md`。留了一个没修的已知问题：
  MCU 没通电时 `device-service` 会疯狂崩溃重启，报错信息只有一个裸
  的"stoul"看不出所以然——这是 `device-service`（V4 范围）自身的健
  壮性问题，不是这次 Yocto 打包的问题。

- 2026-09-03：V6 的止损线检查点（"两周内跑不通最基础的 helloworld
  image 就暂停 V6"）一次通过。`/opt/yocto` 下配好了 poky +
  meta-openembedded + meta-raspberrypi（Scarthgap 5.0 LTS 分支），
  `MACHINE = "raspberrypi5"`，`BB_NUMBER_THREADS`/`PARALLEL_MAKE` 保
  守设置（先把这台 VM 的资源扩了一遍：内存 5.4GB→14GB，磁盘可用
  61GB→193GB——Hyper-V 那边扩过 VHDX 之后 Linux 这边的分区表/LVM/文件
  系统没跟着扩，得手动补）。`bitbake core-image-minimal` 编译干净
  （3729 个任务全过，0 报错）。写进一张空 SD 卡、接实机树莓派 5 开
  机验证：看到 `raspberrypi5 login:`，`root` 空密码（Yocto 默认
  `debug-tweaks` 特性）登录成功。这个镜像完全是"素的"——没有
  `custom_acq` 驱动、没有 `device-service`、没有 Device Tree
  overlay、没有 WiFi/SSH（只装了 `busybox-udhcpc`），纯粹用来验证工
  具链和 `meta-raspberrypi` 认得 Pi 5 这一件事。现在树莓派正在用的
  那张卡（跑着 SSH/驱动/device-service 的那套开发环境）完全没动，这
  次单独用了一张空卡。下一轮要规划 Plan.md V6 说的四类自定义
  recipe（内核模块、`device-service`、配置/WiFi/SSH 使能、镜像组
  装）——目前一个都还没写。
