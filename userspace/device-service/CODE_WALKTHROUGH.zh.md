# device-service 逐行讲解（中文，学习用）

配合 `README.md`（怎么编译运行）和
`driver/custom-acq/CODE_WALKTHROUGH.zh.md`（`/dev/acq0` 那一侧是怎么实
现的）一起看。这是 Plan.md V4 Phase 1——目标只是"能跑通的最小闭环"，
不是最终形态（Configuration/Logging/Metrics/ErrorRecovery/systemd/单
元测试都留到 Phase 2）。

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

## 一句话总结：这份代码里真正"这次业务专属"的只有这几块

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

其余的结构（`Device` 类怎么组织、`RingBuffer` 怎么用条件变量实现、
`AcquisitionWorker` 怎么起停线程、用 `sigwait` 做优雅关闭、
`read()`/`EINTR` 的循环写法、CMake 的搭法）都是**换任何"读一个设备
文件 + 多线程 + 需要优雅关闭"的 C++ 程序都长这样**的固定套路，理解
一次，以后照抄结构、只换业务细节。
