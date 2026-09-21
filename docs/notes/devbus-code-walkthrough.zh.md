# devbus 逐行讲解（中文，学习用）

`userspace/devbus/` 是这个项目里**唯一一处认真写的无锁并发代码**，也是
唯一一处"写错了不会崩、只会偶尔丢一条数据"的代码。所以这份讲解的重点不
是"这行在干什么"（那个读代码就知道），而是**"为什么必须这么写，写成别
的样子会在什么情况下坏掉"**。

配合看：`userspace/devbus/README.md`（英文，对外）、
`docs/devbus-experiments.md`（实测数据）、
`docs/notes/systems-programming-patterns.md`（通用模式，内存序那一节）。

文件清单：

| 文件 | 内容 |
| --- | --- |
| `include/devbus/config.hpp` | 用户可配的枚举和结构体（溢出策略、等待方式） |
| `include/devbus/devbus.hpp` | 对外 API：`Publisher<T>` / `Subscriber<T>` / `Loan<T>` / `Sample<T>` |
| `include/devbus/detail/layout.hpp` | **共享内存布局**，最核心的一份 |
| `include/devbus/detail/core.hpp` | 去类型化的 `PublisherCore` / `SubscriberCore` 声明 |
| `src/core.cpp` | **所有无锁逻辑都在这里**，535 行 |
| `src/os.cpp` | shm / futex / pidfd 这些 Linux 系统调用的薄封装 |

---

## 零、先想清楚：为什么不能用普通的写法

先把"零拷贝"这件事说清楚，因为后面所有别扭的地方都是从这一个决定里长出来的。

普通的进程间通信（Unix socket、pipe、消息队列）是这样：发送方把数据
`write()` 进内核，内核复制一份，接收方 `read()` 再复制一份出来。4 MiB
的数据就是两次 4 MiB 的 memcpy 加两次系统调用。实测在 Pi 5 上是
**1.3–1.6 ms**。

零拷贝是这样：发送方和接收方**映射同一块物理内存**。发送方直接把数据写
进那块内存，然后只传一个"第几号格子"的整数给接收方。接收方直接读那块内
存。实测 4 MiB 和 64 B **一样都是约 5 µs**——因为传的东西大小一样，都是
一个整数。

代价就是：**你失去了内核给你的所有保护**。

1. **不能放指针。** 同一块共享内存，在发布者进程里 `mmap` 到地址 A，在
   订阅者进程里可能 `mmap` 到地址 B（内核不保证给你同一个地址，ASLR 还
   会故意错开）。所以段里只能存**偏移量**（相对段开头多少字节）和**索引**
   （第几号 chunk），任何一个绝对地址存进去，到另一个进程里就是野指针。
   这条是 `layout.hpp` 开头注释的第一句，也是整个文件结构的根本原因。
2. **不能放需要构造/析构的类型。** `std::string`、`std::vector` 内部就是
   指针，同理作废。所以 payload 类型被 `concept ShmPayload` 卡死：
   ```cpp
   template <typename T>
   concept ShmPayload = std::is_trivially_copyable_v<T> && std::is_standard_layout_v<T>;
   ```
   `trivially_copyable` = 可以按字节 memcpy；`standard_layout` = 内存布局
   在不同编译器/不同编译单元下一致。
3. **对方随时可能崩溃。** 订阅者在持有一块内存的过程中被 `kill -9`，发
   布者必须能发现并把那块内存收回来，否则跑几个小时就没 chunk 可用了。
4. **对方随时可能是恶意/有 bug 的。** 共享内存里的任何数值都是**另一个进
   程能任意改写的**，所以从共享内存读出来的每一个索引都必须当成"不可信
   输入"做边界检查。这一条在代码里出现了三次，后面会标出来。

---

## 一、`layout.hpp`：共享内存长什么样

### 1.1 整体布局

```
[SegmentHeader]                          段头：所有配置参数 + 全局计数器
[SubscriberSlot 0][data ring][done ring] 订阅者 0 的槽位 + 它的两个环
[SubscriberSlot 1][data ring][done ring]
...
[ChunkHeader x chunk_count]              每块数据的元信息（序号、发布时间）
[payload chunk 0][payload chunk 1]...    真正的数据，每块按 cache line 对齐
```

一次 `mmap`，一整块，之后**再也不分配任何内存**。这是实时系统的硬要求：
热路径上不能有 `malloc`（可能触发 `brk`/`mmap` 系统调用、可能触发页错误、
最坏情况延迟无界）。

`Layout compute_layout(...)` 这个纯函数负责算出每一段从哪开始、多长。它是
`constexpr` 友好的普通函数，发布者建段时算一次，订阅者不用算——订阅者是
从段头里**读**这些偏移量的：

```cpp
header_->slots_offset, header_->chunk_headers_offset, header_->payloads_offset
```

为什么不让订阅者自己也算一遍？因为那样两边的计算必须永远一致，任何一边
改了公式或者编译器不同导致 padding 不同，就会静默读到错位的内存。存在段
里读出来，只有一个真相来源。

### 1.2 `kMagic` 和 `kLayoutVersion`

```cpp
inline constexpr uint64_t kMagic = 0x3153554256454431ull; // "1DEVBUS1"
inline constexpr uint32_t kLayoutVersion = 1;
```

共享内存段在 `/dev/shm/` 下就是个普通文件，**重启前一直存在**。所以有可能
遇到：旧版本程序留下的段、别的程序恰好重名的段、被截断的段。订阅者打开时
逐项检查：

```cpp
if (header_->magic != kMagic || header_->layout_version != kLayoutVersion)
        throw Error("... is not a compatible devbus segment");
```

这是防**版本偏移**的标准做法。没有它的话，改了 `SegmentHeader` 结构再跑，
新程序会用新的字段偏移去读旧的段，读出来全是垃圾但不会崩。

### 1.3 `static_assert` 三连

```cpp
static_assert(std::atomic<uint64_t>::is_always_lock_free, ...);
```

`std::atomic<T>` 在标准里**不保证**是无锁的——如果硬件不支持，编译器会偷偷
给它配一把互斥锁。在共享内存里这是灾难：那把锁在两个进程里是**两个不同的
锁**，完全不互斥，代码看起来正确但实际上没有任何同步。所以这三行不是形式
主义，是必须有的。`is_always_lock_free` 是编译期常量（`is_lock_free()` 是
运行期的），所以能放进 `static_assert`。

### 1.4 `SlotState` 四态

```cpp
enum class SlotState : uint32_t {
        Free = 0,    // 没人用
        Claimed = 1, // 订阅者正在初始化它，发布者不许碰
        Active = 2,  // 发布者往这里投递
        Closing = 3, // 订阅者走了（或死了），发布者回收完再置 Free
};
```

**为什么需要 `Claimed` 这个中间态**：订阅者抢到槽位之后，还要往里写 pid、
溢出策略、重置四个环指针。如果直接从 `Free` 跳到 `Active`，发布者可能在这
些字段还没写完的时候就开始往里投递，读到的是上一个订阅者残留的配置。
`Claimed` 的意思就是"我占了，但还没准备好，别看"。发布者的 `scan_slots()`
只认 `Active` 和 `Closing`，看到 `Claimed` 什么都不做。

有了 `Claimed`，订阅者初始化那几行就可以用最普通的 `relaxed` 写：

```cpp
slot_->pid.store(getpid(), std::memory_order_relaxed);
slot_->overflow = static_cast<uint32_t>(cfg.overflow);
...
slot_->state.store(static_cast<uint32_t>(SlotState::Active), std::memory_order_release);
```

最后那个 `release` 是**一道栅栏**：它保证上面所有的写在别人看到 `Active`
之前都已经可见。发布者那边配对的是 `acquire`：

```cpp
auto st = static_cast<SlotState>(sl->state.load(std::memory_order_acquire));
```

**这就是 acquire/release 配对的标准用法**：一边用 release 发布一组数据，
另一边用 acquire 观察到那个标志，就能看到那组数据。记住一个直觉：
**release 之前的写不会被重排到 release 之后；acquire 之后的读不会被重排到
acquire 之前。** 两边像一个门闩一样扣上。

### 1.5 `SubscriberSlot` 里满地的 `alignas(kCacheLine)`

```cpp
alignas(kCacheLine) std::atomic<uint64_t> data_head; // 发布者写
alignas(kCacheLine) std::atomic<uint64_t> data_tail; // 订阅者和发布者都 CAS
alignas(kCacheLine) std::atomic<uint64_t> done_head; // 订阅者写
alignas(kCacheLine) std::atomic<uint64_t> done_tail; // 发布者写
```

每个索引单独占一整条 cache line（64 字节），**浪费内存是故意的**。

原因是 **false sharing（伪共享）**。CPU 缓存的最小单位是 cache line，不是
字节。如果 `data_head` 和 `data_tail` 挨在一起落进同一条 line，那么发布者
写 `data_head` 会让这条 line 在订阅者的核上失效；订阅者读 `data_tail`（明明
是另一个变量）就得重新从内存/其他核拉一次。两个核以 1 kHz 甚至 10 kHz 互
相写，这条 line 就在两个核之间来回弹（cache line ping-pong）。逻辑完全正
确，性能掉一个数量级。

判断要不要 `alignas` 的标准很简单：**两个变量是不是被不同的核高频写**。是
就分开。这里四个索引各有各的写者，所以四个都分开。

### 1.6 chunk 预算公式——`loan()` 为什么能保证不失败

```cpp
l.chunk_count = max_subscribers * (l.queue_capacity + max_borrowed) + max_loans;
```

这个公式是整个设计里最需要想明白的一行。

问：一块 chunk 在任意时刻可能待在哪些地方？穷举一遍：

1. 在某个订阅者的 **data ring 里排队**（已投递、还没被取走）→ 每个订阅者
   最多 `queue_capacity` 块；
2. 被某个订阅者 **取走并正在使用**（`Sample` 还活着）→ 每个订阅者最多
   `max_borrowed` 块（`try_receive()` 里 `if (borrowed_ >= max_borrowed) return BorrowLimit;` 卡住）；
3. 被发布者 **借出但还没 send**（`Loan` 还活着）→ 最多 `max_loans` 块
   （`loan()` 里 `if (loans_out_ >= max_loans)` 卡住）。

把所有位置的上限加起来，就是最坏情况下同时被占用的 chunk 总数。只要分配
这么多，`loan()` 就**在协议被遵守的前提下永远不会因为没有 chunk 而失败**。

这才是真正的价值所在：`loan()` 变成了一个**有界、确定、无需等待**的操作。
在实时系统里，"通常很快，偶尔要等"是不可接受的；"永远在 X 微秒内返回，或
者立刻告诉你失败"才可以。

所以 `loan()` 里那段代码才写成这样：

```cpp
if (free_.empty()) {
        // 按 compute_layout() 的预算，走到这里说明某个订阅者违反了协议
        // （或者它死了、还没被回收）。
        scan_slots();
        if (free_.empty()) { ... OutOfChunks ... }
}
```

注释里那句话是关键：**free 列表空掉本身就是一个 bug 信号**，不是正常流
程。所以处理方式是"扫一遍看看是不是有人死了没回收"，而不是"等一会儿再试"。

### 1.7 `completion_capacity` 为什么要 ≥ chunk_count

```cpp
l.completion_capacity = next_pow2(l.chunk_count);
```

done ring（订阅者还给发布者的"我用完了"队列）的容量取成 chunk 总数向上取
整到 2 的幂。为什么不能小？因为最坏情况下，一个订阅者可能在发布者一次都没
来得及 drain 的期间，归还很多块。如果 done ring 会满，订阅者的 `release()`
就得处理"满了怎么办"——而 `release()` 是在 `Sample` 的析构函数里调的，析构
函数里没法失败。把容量开到绝不可能满，`release()` 就永远是几行无分支的代
码：

```cpp
void SubscriberCore::release(uint32_t chunk) noexcept {
        std::atomic<uint32_t>* ring = slot_->done_ring(header_->queue_capacity);
        const uint64_t head = slot_->done_head.load(std::memory_order_relaxed);
        ring[head & done_mask_].store(chunk, std::memory_order_relaxed);
        slot_->done_head.store(head + 1, std::memory_order_release);
        --borrowed_;
}
```

**这是一个反复出现的设计手法：用空间换掉一整类错误路径。**

### 1.8 环形缓冲的索引：为什么用单调递增的 64 位数

注意 `data_head`/`data_tail` 都是 `uint64_t` 且**只增不减**，取模是在用的
时候做：

```cpp
ring[head & mask].store(chunk, ...);   // mask = capacity - 1，capacity 是 2 的幂
```

如果直接存"环里的位置"（0..capacity-1），那 `head == tail` 就同时代表"空"
和"满"，分不开，必须再加一个计数器或者浪费一个格子。用单调递增的数：

- 空：`head == tail`
- 满：`head - tail == capacity`
- 已用：`head - tail`

一行判断搞定，而且**天然免疫 ABA**（下面 1.9 会用到）。64 位在 1 MHz 的
发布速率下要跑 58 万年才溢出，不用管。

`capacity` 强制取 2 的幂（`next_pow2`）是为了 `& mask` 代替 `%`——取模是除
法，在 ARM 上要十几个周期，位与是一个周期。

---

## 二、两个环：为什么是两个，不是一个

每个"发布者→订阅者"这一对，有两个**单生产者单消费者（SPSC）**队列：

```
data ring:  发布者 --写--> 订阅者     "这里有一条新数据，第 c 号格子"
done ring:  订阅者 --写--> 发布者     "第 c 号格子我用完了"
```

为什么要分开两个方向？因为**只有发布者能释放 chunk**（owner-driven
reclaim，跟 iceoryx2 一样）。订阅者从来不碰 free 列表、不碰引用计数，它只
能说一句"我用完了"。

这样做的好处是崩溃安全：引用计数和 free 列表**根本不在共享内存里**，在发
布者的私有堆内存里：

```cpp
// 发布者私有簿记，故意不放共享内存：订阅者崩溃/乱写也污染不了它
std::vector<uint32_t> free_;          // 空闲 chunk 栈
std::vector<uint16_t> refcount_;      // 每块 chunk 还有几个订阅者持有
std::vector<uint16_t> outstanding_;   // [订阅者 * chunk_count + chunk]
```

订阅者哪怕被 `kill -9`，最坏结果也只是"几块 chunk 一直被算作占用"，发布者
发现它死了之后能精确地全部收回来。如果引用计数放在共享内存，一个订阅者在
`--refcount` 的中途被杀，计数就永久错了，而且**没有任何办法修复**——因为你
不知道它是在减之前还是减之后死的。

### 2.1 `outstanding_` 这张二维表是干什么的

```cpp
outstanding_.assign(chunk_count * max_subscribers, 0);
```

`refcount_[c]` 只告诉你"第 c 块被几个人拿着"，但订阅者 3 死了的时候，你需
要知道"其中有几个是订阅者 3 拿的"。`outstanding_[s * chunk_count + c]` 就
是这个：**订阅者 s 手上有几份第 c 块**（可能 >1，因为它可以同时持有同一块
的多个引用？实际上不会，但计数写成通用的更安全）。

回收的时候就是一次精确的减法：

```cpp
void PublisherCore::reclaim_slot(uint32_t s) noexcept {
        drain_done(s);                       // 先把它临死前归还的收掉
        for (uint32_t c = 0; c < n; ++c) {
                uint16_t& out = outstanding_[s * n + c];
                if (out == 0) continue;
                refcount_[c] = refcount_[c] - out;   // 精确扣掉它欠的那部分
                out = 0;
                if (refcount_[c] == 0) free_chunk(c);
        }
        ...
}
```

注意顺序：**先 `drain_done(s)` 再扫表**。订阅者可能在死之前已经归还了一部
分，那部分 `drain_done` 会走正常路径减掉、`outstanding_` 也会同步减。剩下
的才是真正"带走"的。反过来先扫表再 drain 就会把同一块减两次。

### 2.2 `release_ref()` 里的边界检查

```cpp
void PublisherCore::release_ref(uint32_t s, uint32_t chunk) noexcept {
        // 从 done ring 读出来的东西是另一个进程给的不可信输入：
        // 一个我们从没给过这个订阅者的索引，要忽略掉，而不是让它破坏引用计数。
        if (chunk >= header_->chunk_count)
                return;
        uint16_t& out = outstanding_[s * header_->chunk_count + chunk];
        if (out == 0)
                return;
        --out;
        if (--refcount_[chunk] == 0)
                free_chunk(chunk);
}
```

两道检查：**范围**（防止越界写 `outstanding_`/`refcount_`，那是真正的内存
破坏）和 **`out == 0`**（防止订阅者重复归还同一块，把引用计数减成负数，进
而把一块还在被别人用的 chunk 放回 free 列表——那会导致数据被悄悄覆盖）。

这两行是"共享内存里的一切都是不可信输入"这条原则的具体落地。同样的检查在
`try_receive()` 里也有一份（`if (c >= header_->chunk_count) continue;`）。

---

## 三、DropOldest：发布者和订阅者抢同一个格子

这是整份代码里唯一一处**两个进程 CAS 同一个变量**的地方，也是最容易写错
的地方。

`deliver()` 里，队列满了且策略是 DropOldest 时：

```cpp
case Overflow::DropOldest: {
        // 跟订阅者抢最老的那条：谁 CAS 赢了 data_tail 谁拥有它。
        // 如果订阅者先取走了，那现在就有空位了，循环自然会退出。
        uint32_t victim = ring[tail & mask].load(std::memory_order_relaxed);
        if (sl->data_tail.compare_exchange_strong(tail, tail + 1, std::memory_order_acq_rel)) {
                release_ref(s, victim);
                sl->dropped_oldest.fetch_add(1, std::memory_order_relaxed);
                ++report.evicted;
        }
        break;
}
```

订阅者那边 `try_receive()`：

```cpp
const uint32_t c = ring[tail & mask_].load(std::memory_order_acquire);
// 输给了 DropOldest 的驱逐：用新的 tail 重试
if (!slot_->data_tail.compare_exchange_strong(tail, tail + 1, std::memory_order_acq_rel))
        continue;
```

两边的模式完全一样：**先读出候选值，再 CAS 推进 tail，只有 CAS 成功的那一
方才真正拥有这条数据。** 输的一方重新读 tail 再来。

### 3.1 为什么没有 ABA 问题

ABA 问题的经典形态是：线程 A 读到值 X，准备 CAS；期间别人把它改成 Y 又改
回 X；A 的 CAS 成功了，但它基于的前提其实已经变了。

这里不会发生，原因就是 **1.8 提到的单调递增**：`data_tail` 只增不减，
**永远不会回到一个旧值**。CAS 成功严格意味着"从我读到它到现在，没有任何
人动过它"。这不是运气好，是选了单调计数器这个表示法的直接结果。

如果当初存的是"环里的位置 0..15"，tail 就会绕回，A-B-A 立刻成立：订阅者读
到 tail=3，发布者驱逐了 16 条转了一圈回到 3，订阅者的 CAS 会成功，然后它
拿到的是一条完全不同的数据。**这是选择表示法就能消灭一整类 bug 的例子。**

### 3.2 CAS 失败时为什么 `break` 而不是重试

发布者那边 CAS 失败后是 `break`，回到 `for(;;)` 的开头重新 `load(data_tail)`
判断 `head - tail < cap`。因为 CAS 失败只可能是订阅者刚取走了一条——那队列
现在就有空位了，压根不需要再驱逐。循环条件自己会处理。

`compare_exchange_strong` 而不是 `weak`：weak 允许**伪失败**（在 LL/SC 架
构比如 ARM 上，CAS 可能因为无关的缓存事件而假失败）。在这里伪失败会被误解
成"订阅者取走了"，导致少驱逐一次、多转一圈，逻辑上不致命但白费力气；而且
这里的循环体不平凡，strong 更好推理。一般规则：**循环里且循环体很轻 → weak；
否则 → strong。**

---

## 四、内存序：每一处为什么是那个

这部分是最值得反复看的。先给一个判断框架：

- **`relaxed`**：只保证这个变量本身是原子的（不撕裂），**不保证任何顺序**。
  用于纯计数器（谁先谁后无所谓）。
- **`release`（写）/ `acquire`（读）**：成对使用，构成"发布-观察"关系。
  release 之前的所有写，在配对的 acquire 观察到之后都可见。
- **`seq_cst`**：额外保证所有 seq_cst 操作存在一个**全局单一顺序**，所有核
  看到的顺序一致。最贵，只在真的需要"全局顺序"的地方用。

### 4.1 段初始化：一个 release 封住一切

```cpp
header_->publisher_pid.store(getpid(), std::memory_order_relaxed);
header_->ready.store(1, std::memory_order_release); // 把上面所有东西一起发布
```

段头几十个字段、所有 slot 的构造、所有 ChunkHeader 的构造，全部用普通写。
最后一个 `ready = 1` 用 release。订阅者：

```cpp
if (header_->ready.load(std::memory_order_acquire) != 1)
        throw Error("... is still being created");
```

看到 `ready == 1` 就保证上面几百行的初始化全部可见。**一个 release/acquire
对可以"发布"任意多的普通数据**，不需要每个字段都是原子的——这是最常被误解
的一点。

### 4.2 data ring 的写：为什么数据用 relaxed、head 用 seq_cst

```cpp
ring[head & mask].store(chunk, std::memory_order_relaxed);   // ← relaxed
...
sl->data_head.store(head + 1, std::memory_order_seq_cst);    // ← seq_cst
++refcount_[chunk];
```

格子里的内容用 `relaxed` 写，因为它被后面那个更强的 `data_head` 写"罩
住"了：订阅者要先看到 `data_head` 变大，才会去读那个格子。而
`data_head` 的写至少是 release 级别，保证 ring 的写排在它前面。

订阅者读格子时用的是 `acquire`：

```cpp
const uint32_t c = ring[tail & mask_].load(std::memory_order_acquire);
```

严格说，有了 `data_head` 的 acquire 加载在前，这个 `acquire` 已经是多余
的——但它是**便宜的保险**：ARM 上 acquire load 就是一条 `ldar`，x86 上普通
load 本来就是 acquire 语义、零成本。在正确性关键的代码里，为省一条指令而
依赖一个微妙的间接推理，不划算。

### 4.3 为什么 `data_head` 必须是 `seq_cst`——Dekker 握手

这是全文最难的一处，代码里的注释也最长：

```cpp
// seq_cst，不只是 release：这个 store 和下面那个 sleeping 的 load，跟订阅者
// wait() 里对 sleeping 的 seq_cst store 和对 data_head 的 load 配对
//（Dekker 式握手）。在 seq_cst 操作的单一全局顺序里，要么订阅者在睡下去之
// 前看到了新的 head，要么我们看到了它的 sleeping 标志并唤醒它——不可能两
// 边都没有。（独立的 fence 也能做到同样的事，但 ThreadSanitizer 不支持
// fence，而测试是在它下面跑的。）
sl->data_head.store(head + 1, std::memory_order_seq_cst);
...
if (sl->sleeping.load(std::memory_order_seq_cst)) {
        sl->futex_word.fetch_add(1, std::memory_order_release);
        futex_wake_all(&sl->futex_word);
}
```

订阅者那边：

```cpp
case WaitMode::Futex:
        for (;;) {
                const uint32_t word = slot_->futex_word.load(std::memory_order_acquire);
                slot_->sleeping.store(1, std::memory_order_seq_cst);   // ← 先宣告"我要睡了"
                if (slot_->data_tail.load(std::memory_order_acquire) !=
                            slot_->data_head.load(std::memory_order_seq_cst) ||   // ← 再检查一次
                    publisher_gone()) {
                        slot_->sleeping.store(0, std::memory_order_relaxed);
                        return has_data();
                }
                ...
                futex_wait(&slot_->futex_word, word, left);
```

**要防的是什么**：经典的 lost wakeup（丢失唤醒）。

```
订阅者：检查队列 → 空 → 决定睡 →            → futex_wait  （永远睡下去）
发布者：              → 投递数据 → 检查 sleeping（还是 0）→ 不唤醒
```

两个操作交错，数据到了但订阅者永远醒不过来，直到下一条数据。在 1 Hz 的低
速流上这就是一秒的延迟；如果这是最后一条数据，就是永久挂起。

**Dekker 算法的形态**是：两边都先写自己的标志，再读对方的标志。

| | 发布者 | 订阅者 |
| --- | --- | --- |
| 先写 | `data_head = head+1` | `sleeping = 1` |
| 后读 | `sleeping` | `data_head` |

`seq_cst` 保证这四个操作存在一个**所有核公认的全局顺序**。在任何一个这样
的顺序里，"发布者写 head" 和 "订阅者写 sleeping" 必有先后：

- 如果发布者的写在前 → 订阅者后面的读一定看到新 head → 它不睡，直接返回。
- 如果订阅者的写在前 → 发布者后面的读一定看到 `sleeping == 1` → 它发 wake。

**不可能两边都错过。** 这就是 Dekker 式互斥的核心论证。

**为什么 release/acquire 不够**：release/acquire 只约束**同一个变量上**的
前后关系（以及被它罩住的普通读写），**不提供跨变量的全局顺序**。具体说，
"我写 X 然后读 Y" 和 "你写 Y 然后读 X"，在 acquire/release 下，两边的
StoreLoad 都可以被硬件重排成 "先读后写"，于是两边都读到旧值。**StoreLoad
是唯一一种 release/acquire 挡不住、只有 seq_cst（或显式 `mfence`/`dmb ish`）
才挡得住的重排。** 记住这一条基本就够用了。

**为什么不用 `std::atomic_thread_fence`**：用两个 `seq_cst` fence 也能达到
同样效果，而且理论上更便宜（只在 fence 那一点付代价）。但注释里写明了实际
原因——**ThreadSanitizer 不建模 fence**（它的 happens-before 图是基于原子操
作建的，独立 fence 它看不懂），会报一堆假阳性。而这个项目的测试是在
TSan 下跑的，要保住 TSan 的可用性，就把强度加在操作本身上。**这是一个真实
的工程权衡：为了保住一个能抓 bug 的工具，接受一点点性能损失。**

### 4.4 `futex_word` 与 `futex_wait` 的 expected 值

```cpp
const uint32_t word = slot_->futex_word.load(std::memory_order_acquire);  // ① 先记下当前值
... 检查有没有数据 ...
futex_wait(&slot_->futex_word, word, left);                               // ② 值还是 word 才睡
```

`futex_wait(addr, expected, timeout)` 的语义是：**内核原子地检查
`*addr == expected`，相等才睡，不等立刻返回**。发布者唤醒前会
`futex_word.fetch_add(1)` 把值改掉。

所以即使发布者的唤醒发生在①和②之间（那一瞬间订阅者还没进内核），②里的检
查也会发现值变了，直接不睡。**这是 futex 这个 API 存在的全部理由**：它把
"检查条件"和"睡下去"这两步做成了原子的，而用户态无论如何都做不到这一点。

`FUTEX_WAKE` 用 `INT_MAX`（唤醒所有等待者）而不是 1：这个 futex word 是每
个订阅者槽位私有的，正常只有一个等待者，用 INT_MAX 只是图个稳妥。

### 4.5 `sleeping` 标志省掉的东西

```cpp
// 只有订阅者真的睡着了，才付 FUTEX_WAKE 这个系统调用的钱。
if (sl->sleeping.load(std::memory_order_seq_cst)) { ... }
```

在高速流（比如 10 kHz）下，订阅者基本一直是醒着的、在处理上一条，根本没进
过 futex_wait。如果每次 send 都无脑发一次 `FUTEX_WAKE`，那就是每条数据一次
**无用的系统调用**（几百纳秒到 1 微秒）。加了这个标志，高速路径上发布者的
系统调用次数是 **0**。

代价就是 4.3 那一整套 seq_cst 握手。**这是"用一点正确性推理的复杂度，换掉
热路径上的一个系统调用"的典型交易**，也是为什么 futex 的正确用法总是长这
个样子。

---

## 五、崩溃安全：pidfd 为什么比 `kill(pid, 0)` 好

```cpp
int pidfd_open(int pid) noexcept {
        return static_cast<int>(syscall(SYS_pidfd_open, pid, 0));
}

bool pidfd_alive(int pidfd) noexcept {
        if (pidfd < 0) return false;
        pollfd p{pidfd, POLLIN, 0};
        // pidfd 在进程退出时变成可读
        return poll(&p, 1, 0) == 0;
}
```

传统做法是 `kill(pid, 0)`——发一个不存在的信号，靠 errno 判断进程在不在。
这有一个**无法修补的竞态**：**pid 会被回收**。

```
订阅者 pid=1234 崩溃
系统跑了一会儿，pid 绕回，某个无关的进程也拿到 1234
发布者 kill(1234, 0) → 成功 → 以为订阅者还活着 → 那些 chunk 永远收不回来
```

pid 空间默认只有 32768（`/proc/sys/kernel/pid_max`），在一个频繁 fork 的设
备上绕回是分分钟的事。这类 bug 的特征是"跑几小时才出现一次，重启就好"，
极难复现。

`pidfd` 从根上解决了：`pidfd_open()` 拿到的文件描述符**指向那一个具体的进
程对象**，不是一个数字。那个进程一旦退出，这个 fd 就变成可读（`POLLIN`），
而且**永远不会指向别的进程**。pid 回绕再多次都没关系。

代码里的分工也是对的：
- **onboarding 时**（`scan_slots()` 发现新的 Active 槽位）才调
  `pidfd_open()`——注释特意写了"每个订阅者只发生一次，所以这个系统调用不是
  稳态热路径开销"。
- **稳态检查**用 `poll(fd, 0)`，超时 0，不阻塞，非常便宜。
- 检查频率由 `liveness_check_every`（默认 1024 次 send）控制，不是每次 send
  都查。

`process_alive(pid)` 是给"还没 onboard、只有 pid"的场合用的备份路径（比如
接管残留段时判断老发布者死没死），它自己开一个 pidfd 查完就关。

### 5.1 残留段接管

发布者构造时：

```cpp
try {
        ShmSegment existing = ShmSegment::open(name);
        const auto* h = header_of(existing);
        if (existing.size() >= sizeof(SegmentHeader) && process_alive(h->publisher_pid.load()))
                throw Error("... already has a live publisher");
        ShmSegment::unlink(name);
} catch (const Error& e) {
        if (std::string_view(e.what()).find("live publisher") != std::string_view::npos)
                throw;
        // open 失败：没有残留段，正常情况
}
```

三种情况：
1. 段不存在 → `open` 抛异常 → catch 里判断不是 "live publisher" → 吞掉，
   继续正常创建。
2. 段存在但原发布者已死 → `unlink` 掉，重新创建干净的。
3. 段存在且原发布者活着 → 重新抛出，拒绝启动（一个 service 只能有一个发布
   者）。

**第 3 种和第 1 种之间还有一个瞬间的竞态**（两个发布者同时启动，都检查完发
现没有段）。这个由 `create()` 里的 `O_EXCL` 兜住：

```cpp
int fd = shm_open(name.c_str(), O_CREAT | O_EXCL | O_RDWR, 0660);
```

`O_EXCL` 让"存在就失败"这件事由内核原子地完成，慢的那个会拿到 EEXIST 然后
抛异常。**检查-然后-行动（TOCTOU）的竞态，正确解法几乎总是"找一个内核提供
的原子原语代替这两步"，而不是加锁。**

那个用字符串匹配区分异常类型的写法（`find("live publisher")`）不好看，是这
份代码里我自己不满意的地方——更规矩的做法是定义一个带错误码的异常子类。留
着，因为它确实能工作而且只有一处。

### 5.2 析构：发布者走的时候

```cpp
PublisherCore::~PublisherCore() {
        header_->publisher_pid.store(0, std::memory_order_release);   // ① 宣告我走了
        for (每个槽位) {
                sl->futex_word.fetch_add(1, std::memory_order_release);
                futex_wake_all(&sl->futex_word);                       // ② 把睡着的都叫醒
                if (pidfds_[s] >= 0) close(pidfds_[s]);
        }
        ShmSegment::unlink(seg_.name());                               // ③ 摘掉名字
}
```

- ① `publisher_pid = 0` 是订阅者判断"发布者没了"的依据
  （`try_receive()` 返回 `PublisherGone`，`wait()` 里的 `publisher_gone()`）。
- ② 必须唤醒，否则正在 `futex_wait` 的订阅者会一直睡到超时。
- ③ `shm_unlink` 只是把**名字**从 `/dev/shm/` 摘掉，已经 `mmap` 的订阅者继续
  正常工作直到它自己 `munmap`（POSIX 共享内存跟文件一样是引用计数的）。所以
  下一个发布者能立刻用同一个名字建一个全新的段，互不干扰。

订阅者析构对称地简单：

```cpp
SubscriberCore::~SubscriberCore() {
        if (slot_)
                slot_->state.store(static_cast<uint32_t>(SlotState::Closing), std::memory_order_release);
        ...
}
```

只置一个 `Closing`，**不做任何清理**。剩下全交给发布者的
`scan_slots()`/`reclaim_slot()`。这样"干净退出"和"被 kill -9"走的是**同一条
回收路径**——只是后者靠 pidfd 发现而已。**让正常路径和异常路径共用同一段代
码，是让异常路径真正被测试到的唯一可靠办法。**

---

## 六、API 层：RAII 怎么把这套东西包成安全的

`Loan<T>` 和 `Sample<T>` 是两个 move-only 的 RAII 句柄：

```cpp
template <ShmPayload T>
class Loan {
        ~Loan() { if (core_) core_->release_loan(chunk_); }
        Loan(Loan&& other) noexcept : core_(std::exchange(other.core_, nullptr)), ... {}
        Loan& operator=(Loan&&) = delete;     // ← 注意：移动赋值被删掉了
        Loan(const Loan&) = delete;
};
```

- **移动构造把源的 `core_` 置空**（`std::exchange`），所以析构时只有一个会
  真正释放。
- **移动赋值被 `delete`**：移动赋值要先释放自己原有的资源，多一条出错路径，
  而实际根本用不到（这两个句柄的用法都是构造出来、用、销毁）。**能删就删，
  少一个需要正确的东西。**

`send()` 的签名值得看：

```cpp
SendReport send(Loan<T>&& loan) noexcept {
        detail::PublisherCore* core = std::exchange(loan.core_, nullptr);
        return core->send(loan.chunk_);
}
```

`&&` 强制调用方写 `pub.send(std::move(*loan))`，在语法上就表明"这个 loan 交
出去了"。函数里第一件事是把 `loan.core_` 置空，这样 loan 析构时**不会**再走
`release_loan()` 把 chunk 放回 free 列表——因为它已经交给 `send()` 了。这一
步漏掉的话，一块 chunk 会同时出现在 free 列表和某个订阅者的队列里，然后被
下一条数据覆盖：典型的"偶尔收到错数据"的 bug。

注释里还写了一条容易踩的：

```cpp
// 从这个 loan 的角度看，这块内存是未初始化的（里面是上一条数据留下的东西）：
// 你依赖的每一个字段都要自己写。
T& operator*() noexcept { return *ptr_; }
```

chunk 是循环复用的，**不清零**（清零 4 MiB 就把零拷贝的好处吃掉了）。所以
"只填了一半字段"的 bug 表现为"另一半是几十条之前的旧数据"，而不是 0。

### 6.1 类型哈希：防止两边结构体定义不一致

```cpp
template <typename T>
constexpr uint64_t type_hash() {
        return fnv1a(__PRETTY_FUNCTION__);
}
```

`__PRETTY_FUNCTION__` 在这个模板里会展开成包含 `T` 完整类型名的字符串，
FNV-1a 哈希一下存进段头。订阅者打开时校验：

```cpp
if (header_->type_hash != type.hash || header_->type_size != type.size ||
    header_->type_align != type.align)
        throw Error("devbus: payload type mismatch ...");
```

场景：发布者用的是旧版的 `struct Frame`（少一个字段），订阅者是重新编译过
的新版。共享内存不会报错，两边只是按各自的布局解释同一堆字节，**读出来全
是错位的垃圾但程序照常运行**。这三项检查把它变成启动时的一个明确异常。

注意哈希的是**类型名**不是布局，所以"同名但字段改了"只能靠 `size`/`align`
兜住一部分（改字段顺序、size 不变时抓不到）。这是个现实的取舍：真正严格要
靠版本号或者 IDL，这里选了零成本的那档。

### 6.2 为什么 core 是去类型化的

`core.hpp` 开头：

```
// 去类型化的 publisher/subscriber core。它们只搬运 chunk 索引和字节；
// 上面那层带类型的 Publisher<T>/Subscriber<T> 只是加了静态类型和 RAII 句柄。
// core 保持无类型，无锁代码就只编译一次、在一个 .cpp 里，而不是在每个
// include 它的头文件里按 payload 类型各编一份。
```

这是模板库常用的 **thin template / fat core** 手法：难的、长的、需要仔细
review 的代码放在一个非模板的 `.cpp` 里编译一次；模板层薄到一眼能看完。好
处不止是编译速度和代码体积——更重要的是**无锁代码只有一份，只需要被 TSan
测一次**。如果它是模板，每个 payload 类型都会实例化一份新的机器码，理论上
都需要重新验证。

---

## 七、等待策略：三种，各自的代价

```cpp
enum class WaitMode : uint32_t { BusySpin = 0, Yield = 1, Futex = 2 };
```

`wait()` 里三条路径：

```cpp
case WaitMode::BusySpin:
case WaitMode::Yield:
        for (uint32_t i = 0;; ++i) {
                if (has_data()) return true;
                if ((i & 63) == 0 && (publisher_gone() || (!forever && monotonic_ns() >= deadline)))
                        return has_data();
                if (cfg_.wait_mode == WaitMode::Yield) sched_yield();
                else cpu_relax();
        }
```

- `(i & 63) == 0`：**每 64 圈才检查一次时间**。`monotonic_ns()` 走
  `clock_gettime`，虽然有 vDSO 加速不进内核，但仍然比一次 `has_data()` 贵得
  多。忙等循环里，检查退出条件的成本本身就可能成为主要开销。
- `cpu_relax()` 在 x86 是 `pause`，在 ARM 是 `yield` 指令。它告诉 CPU"我在
  自旋等待"，让超线程的另一个逻辑核拿到资源、也降低功耗和退出自旋时的内存
  序惩罚。**忙等循环里不放 `pause` 是常见错误。**

忙等在真机上踩到的两个坑，都不是代码的问题而是内核配置的问题，单独写成了
案例：
- **RT throttling**：`SCHED_FIFO` 的线程默认每秒最多占用 950 ms，到点被强
  制停 50 ms → p99 直接 39 ms。见 `docs/debugging/case-08`。
- **饿死 RCU**：`PREEMPT_RT` 上忙等把 `rcuc/3` 这个内核线程饿死，130 ms 停
  顿。根因是 `bcm2712_defconfig` 没开 `NO_HZ_FULL`/`RCU_NOCB`，
  `nohz_full=` 命令行参数**被静默忽略**。见 `docs/debugging/case-09`。

第二个坑的教训要单独记住：**内核命令行参数是"请求"不是"确认"**。必须回读
`/sys/devices/system/cpu/nohz_full` 验证。

---

## 八、把整条路走一遍

一次完整的 publish → receive：

```
发布者                                            订阅者
──────────────────────────────────────────────────────────────
loan()
 ├ loans_out_ >= max_loans? → LoanLimit
 ├ 对每个活跃订阅者 drain_done()   ← 先回收，再分配
 ├ free_ 空? → scan_slots() 再试   ← 空本身是异常信号
 └ free_.pop_back() → chunk c

*loan = data                       ← 直接写进共享内存，零拷贝

send(c)
 ├ seq = next_seq_++
 ├ chunk_headers_[c] = {seq, monotonic_ns()}
 ├ scan_slots()                    ← 新订阅者上线 / 走掉的回收
 └ 对 active_list_ 里每个 s: deliver(s, c)
      ├ 队列满? → 按策略 DropOldest / DropNewest / Block
      ├ ring[head & mask] = c        (relaxed)
      ├ data_head = head+1           (seq_cst) ←─┐ Dekker
      ├ ++refcount_[c]; ++outstanding_[s][c]     │
      └ if (sleeping) FUTEX_WAKE   ←─────────────┘
                                                   wait()
                                                    ├ sleeping = 1   (seq_cst)
                                                    ├ 再查一次 data_head
                                                    └ futex_wait(word, ...)
                                                   try_receive()
                                                    ├ 读 ring[tail & mask]
                                                    ├ CAS data_tail  ← 可能输给驱逐
                                                    ├ 边界检查 c
                                                    ├ 对账 seq → observed_gaps_
                                                    └ ++borrowed_ → Sample
                                                   用数据（就地读，零拷贝）
                                                   ~Sample()
                                                    └ done_ring[head] = c; done_head++
 下一次 loan() 里 drain_done() 收到 ────────────────┘
      └ release_ref: --outstanding_, --refcount_, 归零则 free_chunk
```

注意 **`loan()` 开头就 `drain_done()`** 这个安排：回收发生在分配之前，所以
稳态下 free 列表总是刚被补充过。不需要单独的回收线程，不需要定时器——**回
收搭在分配的顺风车上**，这是无 GC 系统里很常见的一招。

---

## 八·五、后来加的：`pressure()` —— 为什么这个信号有价值

2026-09-20 加的，很短，但它体现的思路值得单独说：

```cpp
uint32_t PublisherCore::max_queued() const noexcept {
        uint32_t worst = 0;
        for (uint32_t s : active_list_) {
                const SubscriberSlot* sl = slot(s);
                const uint64_t head = sl->data_head.load(std::memory_order_relaxed);
                const uint64_t tail = sl->data_tail.load(std::memory_order_acquire);
                const uint64_t queued = head - tail;
                if (queued > worst) worst = static_cast<uint32_t>(queued);
        }
        return worst;
}
```

三个点：

1. **`head` 用 `relaxed` 读**：`data_head` 只有发布者自己写，读自己写的东西
   不需要任何同步。`tail` 是订阅者在动，用 `acquire`。
2. **算出来的值天生是"陈旧"的**——读完 `tail` 之后订阅者可能又取走了几条。
   但**误差方向是安全的**：订阅者只会让队列变短，所以这个数只会偏大，不会
   漏报拥塞。**一个压力信号不需要精确，只需要误差方向可控。** 想让它精确
   就得加锁，那就把无锁设计毁了，完全不值。
3. **这是领先指标，不是滞后指标。** 丢弃计数器（`dropped_oldest` 之类）按
   定义只有在**数据已经丢了之后**才会动；队列深度在**一条都没丢的时候**就
   开始涨。这两者的区别，2026-09-20 在真机上量到了具体数字：扫
   `inter_frame_us` 时，吞吐、序号缺口、`kfifo_overflow` 三个指标**全程健
   康**，而延迟已经涨了 **11 倍**，再走一步整条管道就塌了。

**所以一个控制器该看什么，取决于它还剩多少时间去反应。** 看丢弃计数器等于
在事故发生后才踩刹车。device-service 的 `BackpressureController` 因此加了
`Congestion::Warning` 这一档：预警来了就退一步、并且停止往上爬；只有真丢数据
才腰斩。

## 九、这份代码里值得带走的东西

按"以后写别的东西还用得上"排序：

1. **共享内存里只能有偏移量和索引，不能有指针**，还有由此推出的一整串限制
   （trivially copyable、无锁原子、不可信输入）。
2. **单调递增的索引 + `& mask`**：一行区分空/满，并且天然免疫 ABA。选对表
   示法可以消灭一整类 bug。
3. **chunk 预算公式**：把"一个资源可能待在哪些地方"穷举一遍求和，就能让分
   配变成有界且确定的操作。实时系统里这比"平均很快"重要得多。
4. **owner-driven reclaim**：只有所有者能释放；其他人只能说"我用完了"。配
   合把簿记放在私有内存，让崩溃从"数据结构被破坏"降级成"几块资源暂时不可
   用"。
5. **pidfd > `kill(pid, 0)`**：pid 会回绕，fd 不会。
6. **Dekker 式睡眠/唤醒握手必须 seq_cst**，因为 StoreLoad 是 acquire/release
   挡不住的唯一一种重排。
7. **`alignas(cache line)` 分开被不同核高频写的变量**，否则逻辑对但性能塌。
8. **用空间换掉整条错误路径**（done ring 开到绝不会满，所以 `release()` 无
   法失败）。
9. **让正常退出和崩溃走同一条回收路径**，异常路径才会真的被测到。
10. **为了保住工具而做的权衡是合理的**（不用 fence 以便 TSan 能工作）。
11. **领先指标 vs 滞后指标**：队列深度在丢数据之前就涨，丢弃计数器只在丢了
    之后才动。控制器该看哪个，取决于它还剩多少时间反应。
12. **一个信号不需要精确，只需要误差方向可控**——`max_queued()` 读出来天生
    偏大，而偏大正好是安全的那一侧。

## 十、我自己还不满意的地方

老实记下来，以后有机会再改：

- 用异常消息的字符串匹配来区分错误种类（5.1），应该定义错误码。
- 类型哈希只哈希类型名，改了字段顺序但 size 不变时抓不到。
- `Overflow::Block` 的超时是自旋等待（`cpu_relax()`），没有让出 CPU。对
  这个用例（超时 1 ms）可以接受，但在更长的超时下会白烧一个核。
- 发布者是单线程的（`Publisher` 明确写了 not thread-safe），多线程发布需要
  调用方自己加锁。真正的多生产者要重新设计 free 列表。
