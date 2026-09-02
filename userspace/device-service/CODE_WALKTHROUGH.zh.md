# device-service 逐行讲解（中文，学习用）

配合 `README.md`（怎么编译运行）和
`driver/custom-acq/CODE_WALKTHROUGH.zh.md`（`/dev/acq0` 那一侧是怎么实
现的）一起看。第一部分是 Plan.md V4 Phase 1（能跑通的最小闭环）；第
二部分是 Phase 2 补上的 Configuration/Logging/Metrics/ErrorRecovery/
systemd/单元测试，在文件靠后的位置。

同样标注每一段代码是**固定套路**（这类"读设备文件 + 处理阻塞/信号"
的 C++ 程序基本都长这样，换个项目照样适用）还是**这次业务相关**（跟
`/dev/acq0` 的具体协议、这个采集场景绑定）。

---

## 整体结构

```
include/
├── device.hpp             Sample 结构体、Device 类声明
├── ring_buffer.hpp         RingBuffer（纯头文件模板类）
└── acquisition_worker.hpp  AcquisitionWorker 类声明
src/
├── device.cpp
├── acquisition_worker.cpp
└── main.cpp
```

数据流向：`main.cpp` 起一个 `AcquisitionWorker` 线程 → 这个线程不断调
用 `Device::read_sample()` 从 `/dev/acq0` 读 → 读到的每条样本 `push`
进 `RingBuffer` → `main.cpp` 的主线程从 `RingBuffer` 里 `pop` 出来打
印。两个线程之间唯一的桥梁就是这个 `RingBuffer`，没有别的共享状态。

## `device.hpp` / `device.cpp` —— 封装 `/dev/acq0` 和 sysfs

```cpp
struct Sample {
	uint32_t seq;
	uint32_t value;
};
```
**这次业务相关**，跟 driver 那边 `struct custom_acq_sample` 的字段顺
序（`seq` 在前、`value` 在后，都是 `u32`）必须完全对应——`read()` 从
`/dev/acq0` 拿到的就是这 8 个字节原样拷过来的，两边只要字段顺序或类
型对不上，读出来的数据就全错位，编译器和运行时都不会替你检查这件
事，这是纯靠约定对齐的（`docs/learning-qa.md` Q25 讲过 `/dev/acq0`
背后就是这样一份内核结构体，靠 `read()` 原样拷贝出来）。

```cpp
class Device {
public:
	void open();
	void start_acquisition();
	void stop_acquisition();
	Sample read_sample();
	bool wait_readable(int timeout_ms);
	uint32_t read_device_id();
	...
private:
	std::string write_sysfs(const std::string& name, const std::string& value);
	std::string read_sysfs(const std::string& name);
	int fd_ = -1;
};
```
类本身的形状（构造函数存路径、`open()`/`read_sample()` 这种"资源+操
作"的封装方式）是**固定套路**——任何包一层"设备文件"的 C++ 代码基本
都这么组织。`start_acquisition()`/`stop_acquisition()` 写的是
`control` 这个具体的 sysfs 文件名、`read_device_id()` 读的是
`device_id`——这些文件名和它们的读写语义（`control` 只认 bit0、
`device_id` 是只读的实时 SPI 读）是**业务相关**，来自
`driver/custom-acq/custom_acq.c` 定义的那张 sysfs 属性表。

```cpp
Sample Device::read_sample() {
	...
	while (total < sizeof(s)) {
		ssize_t n = ::read(fd_, buf + total, sizeof(s) - total);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			throw DeviceError(...);
		}
		...
		total += n;
	}
	return s;
}
```
**固定套路**：`read()` 系统调用不保证一次就把请求的字节数全读满（哪
怕是从一个"要么给你整条记录、要么阻塞"的设备读也一样，这是 POSIX
`read()` 的通用契约，不是这个设备特有的），所以要用一个循环读到够
为止；`EINTR`（读到一半被信号打断）要重试而不是当成错误——这是任何
调用阻塞式 `read()` 的 C/C++ 代码都要处理的标准写法，不是本项目发明
的。

```cpp
bool Device::wait_readable(int timeout_ms) {
	pollfd pfd{};
	pfd.fd = fd_;
	pfd.events = POLLIN;
	int ret = ::poll(&pfd, 1, timeout_ms);
	...
	return ret > 0 && (pfd.revents & POLLIN);
}
```
`poll()` 的调用方式是**固定套路**。**为什么需要这个方法、而不是直接
在 `AcquisitionWorker` 里一路阻塞 `read()` 下去——这是这次的设计决
定**：如果线程正阻塞在 `read()` 里，另一个线程没有安全的办法叫醒
它——`close()` 掉它在读的 fd 是 POSIX 里公认的 race（不保证会让
`read()` 干净地返回错误，行为因内核/glibc 版本而异）。改成"先 `poll`
一个较短的超时时间，超时了就回去检查一下要不要停止，没超时再真正去
读"，就能让 `AcquisitionWorker` 的停止请求最多等一个超时周期
（`kPollTimeoutMs = 200ms`）就生效，不用碰任何一个 unsafe 的手段。

## `ring_buffer.hpp` —— 生产者/消费者之间的缓冲区

```cpp
template <typename T>
class RingBuffer {
	void push(const T& item) {
		std::unique_lock<std::mutex> lock(mutex_);
		not_full_.wait(lock, [this] { return count_ < buf_.size() || stopped_; });
		...
	}
	bool pop(T& out) {
		std::unique_lock<std::mutex> lock(mutex_);
		not_empty_.wait(lock, [this] { return count_ > 0 || stopped_; });
		...
	}
	void stop() { ...; not_empty_.notify_all(); not_full_.notify_all(); }
};
```
这整个类都是**固定套路**——`std::mutex` + 两个 `std::condition_variable`
（一个表示"不空了可以读"，一个表示"不满了可以写"）实现单生产者/单消
费者环形缓冲区，是教科书写法，换任何项目要做"一个线程生产、另一个线
程消费"这种场景都可以直接照抄这个类，不需要改。

**业务相关的只有一个决定**：满了要不要阻塞、还是直接丢弃最老/最新的
数据？这里选的是"满了就阻塞"（`push()` 会等 `pop()` 腾出空间），理
由写在类开头的注释里——`/dev/acq0` 那边（内核的 `kfifo_overflow`）已
经有一套真实的丢包信号了，用户态这层没必要再发明第二套、语义还不一
样的丢包逻辑，让"到底哪层丢的"变得难以追查。`stop()` 这个方法也是业
务决定：给关闭流程用的，让阻塞中的 `push`/`pop` 都能被唤醒退出，不
然优雅关闭这件事根本做不到。

## `acquisition_worker.hpp` / `.cpp` —— 采集线程

```cpp
void AcquisitionWorker::run() {
	try {
		while (!stop_requested_) {
			if (!device_.wait_readable(kPollTimeoutMs))
				continue;

			Sample s = device_.read_sample();
			samples_read_.fetch_add(1);

			if (last_seq_.has_value() && s.seq != *last_seq_ + 1)
				gap_count_.fetch_add(1);
			last_seq_ = s.seq;

			buffer_.push(s);
		}
	} catch (...) {
		last_error_ = std::current_exception();
	}
}
```
线程本身的起停套路（`std::thread` + `std::atomic<bool> stop_requested_`
+ `join()`）是**固定套路**。`try/catch` 把异常存进
`std::exception_ptr` 而不是直接让线程崩溃退出，也是**固定套路**——
工作线程里抛出的异常不会自动传播到主线程，必须自己接住、存起来，让
主线程之后决定怎么处理（这里是 `main.cpp` 在 join 完之后重新
`rethrow` 出来打印）。

**业务相关的核心逻辑就两行**：`s.seq != *last_seq_ + 1` 这个跳号检测
（照抄 `testv13.py` 的判断方式——序列号应该逐条 `+1`，32 位自然回
绕，跳号就代表中间丢了数据），以及"读到的每条样本要不要塞进
buffer"这个业务本身。`std::optional<uint32_t> last_seq_` 用
`optional` 而不是直接一个 `uint32_t` 初始化成 0，是为了让"这是本次
运行收到的第一条样本，还没有上一条可比较"这个状态能被准确表达出来，
不会把"真的收到了 seq=0"和"还没收到过任何东西"这两种情况搞混。

## `main.cpp` —— 信号处理和关闭流程

```cpp
void block_shutdown_signals(sigset_t& set) {
	sigaddset(&set, SIGINT);
	sigaddset(&set, SIGTERM);
	pthread_sigmask(SIG_BLOCK, &set, nullptr);
}
...
std::thread signal_thread([&] {
	int sig = 0;
	sigwait(&shutdown_set, &sig);
	...
	device.stop_acquisition();
	buffer.stop();
	worker.stop();
});
```
**这是这份代码里最容易被误解、但其实是标准做法的一段，值得记一下原
理**：为什么不直接用 `signal(SIGINT, handler)` 写一个经典的信号处理
函数？因为真正的异步信号处理函数能做的事极其有限（只能碰一小撮"信
号安全"的函数，不能碰互斥锁、不能做文件 I/O、不能 join 线程）——但
这里关闭服务要做的事（写 sysfs、`join()` 线程、给 mutex 加锁）没有
一件是信号安全的。标准解法（**固定套路**，POSIX 多线程程序常见写
法）是反过来：先用 `pthread_sigmask` 把这两个信号在所有线程里都屏蔽
掉，然后专门起一个线程用 `sigwait()` 去"排队等"这个信号——`sigwait`
返回的时候，代码已经是在普通线程上下文里运行，不再是信号上下文，想
调用什么都可以。

**业务相关的是关闭的顺序**：先 `stop_acquisition()`（让 MCU 停止产生
新数据），再 `buffer.stop()`，最后才 `worker.stop()`——`buffer.stop()`
要放在 `worker.stop()` 前面，是因为如果这时候 worker 线程恰好卡在
`buffer_.push()`里（缓冲区满了），`worker.stop()` 内部的 `join()`
会一直等这个线程退出，但线程退不出来是因为它在等 `buffer` 腾地方；
先调用 `buffer.stop()` 能保证任何卡在 `push`/`pop` 里的调用都会立刻
返回，不会死锁在关闭流程里。这是新代码里唯一一处需要小心顺序的地
方，其余的关闭步骤顺序怎么摆都不影响正确性。

## CMake 文件

`CMakeLists.txt`/`CMakePresets.json`/`build.sh` 都是**固定套路**，照
抄了仓库里 MCU 项目那一套"`CMakePresets.json` 定义 `Debug`/`Release`
两个 preset + `target_*` 风格"的习惯，唯一区别是这边编译的是跑在树
莓派本机的普通可执行文件，不需要交叉编译工具链、也不需要 OpenOCD 烧
录这一步。

---

---

# 第二部分：Phase 2 新加的代码

## `SequenceTracker`（`include/sequence_tracker.hpp`）—— 从 `AcquisitionWorker` 里拆出来的纯逻辑

```cpp
class SequenceTracker {
public:
	bool observe(uint32_t seq) {
		bool is_gap = last_seq_.has_value() && seq != *last_seq_ + 1;
		if (is_gap)
			gap_count_.fetch_add(1, std::memory_order_relaxed);
		last_seq_ = seq;
		return is_gap;
	}
	uint64_t gap_count() const { return gap_count_.load(std::memory_order_relaxed); }
private:
	std::optional<uint32_t> last_seq_;
	std::atomic<uint64_t> gap_count_{0};
};
```
这次改动本身是**固定套路**：把散落在某个类内部、跟其他状态混在一起
的一小段纯逻辑（不碰硬件、不碰 I/O）抽成独立的小类，换来的是能直接
写单元测试（`tests/test_sequence_tracker.cpp`），不用启动整个
`AcquisitionWorker` 线程、更不用真的接硬件才能测这条判断逻辑对不对。
**业务相关的还是那条判断规则本身**（`seq != last_seq + 1`，
32 位回绕不算跳号）。

`gap_count_` 用 `std::atomic` 而不是普通 `uint64_t`，是因为
`observe()` 只会被 `AcquisitionWorker` 自己的线程调用，但
`gap_count()` 会被别的线程读（`MetricsReporter` 定时读它）——两个不
同线程碰同一块内存，不加同步就是数据竞争，`atomic` 是这里最简单的解
法（`last_seq_` 不需要 `atomic`，因为它只在 `observe()` 内部被同一
个线程读写，从来不会被外部直接读取）。

## `Config`（`include/config.hpp` + `src/config.cpp`）—— nlohmann-json 配置文件

```cpp
struct Config {
	std::string dev_path = "/dev/acq0";
	...
	static Config load(const std::string& path);
};
```
用一个"字段自带默认值的结构体 + 一个 `load()` 静态方法"来做配置，是
**固定套路**——`load()` 内部的写法（`j.value("key", cfg.key)`）也是
`nlohmann::json` 这个库的标准用法：`.value(key, default)` 意思是"文
件里有这个字段就用文件的，没有就用我传的默认值"，天然支持"部分字段
覆盖、其余用默认值"，不用自己手写一堆 `if (j.contains("key"))`。

**业务相关的只是这六个字段具体是什么**（`dev_path`/`sysfs_dir`/
`log_level`/`buffer_capacity`/`liveness_timeout_ms`/
`metrics_interval_ms`）——换个项目要配置化的东西完全不同，但"用一个
默认值结构体 + json 覆盖"这个做法可以照搬。

`main.cpp` 里 `(argc > 1) ? Config::load(argv[1]) : Config{}` 这一
行是业务决定：没传配置文件路径就直接用全默认值跑，不去猜一个默认文
件名、也不强制要求配置文件必须存在——图的是本地跑起来方便。

## Logging（spdlog 替换 `printf`）

```cpp
void init_logging(const std::string& log_level) {
	spdlog::set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] %v");
	spdlog::set_level(spdlog::level::from_str(log_level));
}
```
这几行是**固定套路**——`spdlog::set_pattern`/`set_level` 是这个库
初始化时的标准写法，`spdlog::info(...)`/`spdlog::error(...)` 跟
`printf` 用法很像（`{}` 占位符而不是 `%d`/`%s`，背后用的是
`{fmt}` 这个格式化库），但自带时间戳、日志级别、线程安全（多个线程
同时调用 `spdlog::info` 不会互相打断输出）。**业务相关的只是把原来
每一处 `printf`/`fprintf(stderr, ...)` 按"这条信息该是 info 还是
error 级别"分类替换掉**——这个分类判断本身没有固定规则，凭经验：正
常运行过程中的信息用 `info`，操作失败但程序能继续用 `warn`，操作失
败且这次操作彻底放弃用 `error`。

## `MetricsReporter`（`include/metrics.hpp` + `src/metrics.cpp`）—— 定时读现有状态，不是重新计数

```cpp
void MetricsReporter::run() {
	std::unique_lock<std::mutex> lock(mutex_);
	while (!cv_.wait_for(lock, interval_, [this] { return stop_requested_; })) {
		lock.unlock();
		report_once();
		if (on_tick_)
			on_tick_();
		lock.lock();
	}
}
```
线程起停的骨架（`std::thread` + `stop_requested_` + `join()`）**还
是那套固定套路**（`docs/systems-programming-patterns.md` 有通用版
本）。这里第一次用到 `condition_variable::wait_for` 带超时的等待方
式，而不是 `AcquisitionWorker`/`Watchdog` 用的"短超时 `poll`/
`wait_for` 循环检查标志"——效果是一样的（能被 `stop()` 提前叫醒，也
能到点自动醒），只是这里用条件变量的超时等待，直接把"等多久"和"被
提前唤醒"这两件事交给标准库处理，不用自己写额外的短轮询循环。

**业务相关、也是这次设计上唯一值得强调的点**：`report_once()` 里没
有引入任何新的计数器，全部是"问一遍已经存在的状态"——
`worker_.samples_read()`、`worker_.gap_count()`（`SequenceTracker`
早就在维护）、`buffer_.size()`、`device_.read_kfifo_overflow()`（真
实的 sysfs 读）。唯一算"新状态"的是 `last_samples_read_`，只是为了
算"这次汇报距上次汇报涨了多少"这个速率，本身也是从已有状态派生出来
的，不是重新发明一套独立的计数系统。

`on_tick_` 这个 `std::function<void()>` 回调是特意留的钩子——
`main.cpp` 用它把 systemd 的 `sd_notify(WATCHDOG=1)` 接到同一个定时
心跳上，不用为了 systemd 单独再起一个定时器。

## `Watchdog`（`include/watchdog.hpp` + `src/watchdog.cpp`）—— ErrorRecovery

```cpp
void Watchdog::check_once() {
	auto since_last_sample = std::chrono::steady_clock::now() - worker_.last_sample_time();
	if (since_last_sample < timeout_)
		return;
	...
	uint32_t device_id;
	try {
		device_id = device_.read_device_id();
	} catch (const std::exception& e) {
		spdlog::error("watchdog: liveness probe failed ...");
		return;
	}
	device_.stop_acquisition();
	device_.start_acquisition();
}
```
线程起停还是固定套路。**这次真正的业务/设计决定，是"探活失败该怎么
分级处理"这套逻辑本身**：`/dev/acq0` 和 sysfs 都不会主动告诉用户空
间"MCU 掉线了"（`docs/learning-qa.md` Q25 讨论过这个限制），所以只
能靠"该来数据的时候超时没来"这个间接信号去猜。分两级处理：
1. **探活成功（`device_id` sysfs 读没抛异常）**：说明 SPI 链路本身
   还通，只是 MCU 那边可能停了或者卡住了——尝试"软复位"：重新写一遍
   `control`（先 `0` 再 `1`）。这招管用是因为固件在写 `control=1`
   时会重置自己的 `seq_counter`/FIFO（`case-05` 调查时发现的固件行
   为），相当于间接让 MCU"回到刚开始采集的状态"。
2. **探活本身失败（`read_device_id()` 抛异常）**：说明 SPI 链路已经
   不通了，软复位这招用的还是同一条 SPI 总线，大概率也发不出去——直
   接放弃自动恢复，把 `tools/mcu-reset.sh`（走调试器 SWD，物理复位，
   不依赖 SPI）作为日志里指向的人工兜底手段。

`last_recovery_attempt_` 这个字段是另一个业务决定：同一个超时窗口内
最多尝试一次软复位，不然如果链路真的坏了，会每个 check 周期都发一遍
`control` 写入，没有意义还占用 SPI 总线。

`AcquisitionWorker::last_sample_time()` 为什么存成
`std::atomic<int64_t>`（毫秒数）而不是直接存一个
`std::chrono::steady_clock::time_point`：`time_point` 不是能直接塞
进 `std::atomic` 的"平凡"类型（取决于具体实现，不保证是无锁的），转
成一个普通整数（自 `steady_clock` 纪元以来的毫秒数）就能用最简单的
`std::atomic<int64_t>`，读的时候再转换回 `time_point`。

## systemd 集成（`systemd/device-service.service` + `main.cpp` 里的 `sd_notify`）

```cpp
sd_notify(0, "READY=1");
...
metrics.set_on_tick([] { sd_notify(0, "WATCHDOG=1"); });
```
`sd_notify()` 的调用方式是**固定套路**——systemd 定义的标准协议，
`"READY=1"` 意思是"我已经完成启动，可以认为我正常运行了"，
`"WATCHDOG=1"` 意思是"我还活着，别把我杀了重启"，不需要自己实现这
套协议，`libsystemd` 已经包好了。**这个函数在没有被 systemd 启动的
情况下调用是安全的空操作**（内部检测有没有 `NOTIFY_SOCKET` 这个环境
变量，没有就什么都不做），所以不需要额外判断"我是不是被 systemd 启
动的"才调用它。

`systemd/device-service.service` 里 `Type=notify` + `WatchdogSec=15`
是**业务/部署决定**：告诉 systemd "等我主动发 `READY=1` 才算启动成
功"（而不是"进程一 fork 出来就算启动成功"），以及"如果超过 15 秒没
收到我的心跳，就当我卡死了，杀掉重启"。`WatchdogSec` 的值要明显大于
`config.json` 里的 `metrics_interval_ms`（这里是 5000ms 对 15000ms，
留了 3 倍余量），不然网络抖动/系统繁忙导致偶尔错过一两次心跳就会被
误杀重启。

---

## 一句话总结：这份代码里真正"这次业务专属"的只有这几块

**Phase 1：**
1. `Sample` 结构体的字段顺序/类型，必须跟 driver 的
   `struct custom_acq_sample` 完全对齐
2. `Device` 里各个 sysfs 文件名（`control`/`device_id`/`fw_version`/
   `kfifo_overflow`）和它们各自的读写语义
3. `wait_readable()` 存在的原因——避免用不安全的方式打断阻塞中的
   `read()`
4. `RingBuffer` 满了选择阻塞、而不是丢数据（因为丢包信号已经在内核
   那层有了）
5. `AcquisitionWorker` 里的跳号检测逻辑（`seq != last_seq + 1`）
6. `main.cpp` 里"先 `stop_acquisition`，再 `buffer.stop`，最后
   `worker.stop`"这个具体顺序

**Phase 2：**
7. `Config` 六个字段具体是什么、各自的默认值
8. `Watchdog` 探活失败后"软复位 vs 彻底放弃"这套两级分级逻辑，以及
   为什么软复位有效（固件在 `control=1` 时会重置自身状态）
9. `WatchdogSec`（systemd）要明显大于 `metrics_interval_ms`（心跳间
   隔）这个数值关系

其余的结构（`Device` 类怎么组织、`RingBuffer` 怎么用条件变量实现、
`AcquisitionWorker`/`MetricsReporter`/`Watchdog` 怎么起停线程、用
`sigwait` 做优雅关闭、`read()`/`EINTR` 的循环写法、CMake 的搭法、
`nlohmann-json`/`spdlog`/`sd_notify` 各自的标准用法）都是**换任何
"读一个设备文件 + 多线程 + 需要优雅关闭 + 需要配置/日志/监控"的
C++ 服务都长这样**的固定套路，理解一次，以后照抄结构、只换业务细
节。
