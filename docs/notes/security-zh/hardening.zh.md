# 产线镜像加固（中文译本）

> 译自 [`docs/security/hardening.md`](../../security/hardening.md)，供自己复习用。
> 以英文原版为准；命令、配置项、日志输出保持原样。

在镜像 recipe
[`device-platform-image-prod`](../../../yocto/meta-device-platform/recipes-core/images/device-platform-image-prod.bb)
里关闭[威胁模型](threat-model.zh.md)中的发现 **F1、F2、F3、F11** 以及 **F5** 的前一半。

**状态：** 已构建，每项改动都在生成的根文件系统里验证过，并且**在 Raspberry Pi 5 上启动并检查过**（§5）。
原始输出：[`results/security/hardening/first-boot-prod.txt`](../../../results/security/hardening/first-boot-prod.txt)。

## 1. 两个镜像，同一套软件

`device-platform-image-prod` `require` 了开发镜像，只改变设备**能怎样被访问和修改**。
它从不改变**跑的是什么**，所以在一个镜像上发现的 bug 能在另一个上复现。

| | `device-platform-image`（开发） | `device-platform-image-prod` |
| --- | --- | --- |
| root 密码 | 空（`debug-tweaks`） | `*` —— 任何密码都不可能匹配 |
| SSH | dropbear `-B`：接受空密码，允许 root | dropbear `-w -s -j -k`：禁 root，**完全禁用密码**，禁端口转发 |
| 谁能登录 | root，经 SSH、串口和 HDMI 控制台 | `admin`，用一把 SSH 密钥，再 `sudo` |
| 串口控制台（`ttyAMA10`） | root 登录提示 | getty 被屏蔽（`→ /dev/null`） |
| 根文件系统 | 可读写 | 只读（`read-only-rootfs`），`/var/lib`、`/var/log`、`/tmp` 在 tmpfs 上 |
| `device-service` | root，无限制 | root uid，2 个 capability，systemd 沙箱（§3） |

## 2. 每项改动是什么，怎么检查的

所有检查都是在构建产出的根文件系统 tarball 上做的，并与同一版本构建的开发镜像对比。

**F1 —— 没有远程 root，没有密码。** 去掉 `debug-tweaks`；正是它在 `/etc/shadow` 里设置了
`root::`（空哈希）并给 dropbear 加上 `-B`。新 recipe
[`device-platform-admin`](../../../yocto/meta-device-platform/recipes-support/configuration/device-platform-admin_1.0.bb)
通过 `useradd.bbclass` 创建 `admin`，密码字段设为 `*`，安装它的 `authorized_keys`
（属主 `1000:1000`，权限 `0600`，放在 `0700` 的 `.ssh` 里）和一个 `sudoers.d` 条目（`0440`）。
两个要紧的细节：

- 用 `*`，不用 `!`。两者都表示"没有密码"，但 dropbear 把以 `!` 开头的哈希当作**已锁定账户**，
  连公钥登录也拒绝。
- 公钥本身来自仓库外面的一个文件（`DEVICE_PLATFORM_ADMIN_PUBKEY`），和 WiFi 凭据一样。
  recipe 用 `do_install[file-checksums]` 声明它，所以换了公钥会改变任务哈希——没有这一条，
  sstate 会静默地继续安装旧公钥（WiFi 文件也有同样的 bug，一并修了）。

```
dev   /etc/shadow   root::            /etc/default/dropbear   DROPBEAR_EXTRA_ARGS=" -B"
prod  /etc/shadow   root:*  admin:*   /etc/default/dropbear   DROPBEAR_EXTRA_ARGS="-w -s -j -k"
```

`-j`/`-k` 禁用本地和远程端口转发：就算 admin 的密钥被偷，也没法把设备变成通往客户网络的隧道。

**F3 —— UART 排针上没有登录。** `SERIAL_CONSOLES` 是和开发镜像共用的 machine 设置，
而 `systemd-serialgetty` 在这里是 systemd 的硬依赖，所以没法直接不构建 getty。产线镜像改为
屏蔽每个 `serial-getty@<tty>` 实例。（这个后处理步骤的第一版失败了：`SERIAL_CONSOLES` 是
`115200;ttyAMA10`，没加引号的 `;` 成了 shell 命令分隔符。现在改用 BitBake 的内联 Python
把 tty 名拆出来。）

**F5（前一半）—— 只读根。** `IMAGE_FEATURES += "read-only-rootfs"` 把 `/` 以只读挂载，并通过
`volatile-binds` 把 `/var/lib`、`/var/log` 和 `/tmp` 挪到 tmpfs。值得知道的副作用：Yocto 还会去掉
那些只为修改运行中系统而存在的包（`shadow`、`base-passwd`、`update-rc.d`、`update-alternatives`），
这让 setuid 二进制从 15 个降到 8 个。

**F11 —— 服务沙箱。** 见 §3。

### 前后对比

| | 开发 | 产线 |
| --- | --- | --- |
| 能登录的账户 | root（无密码） | admin（仅密钥） |
| SSH 密码登录 | 接受，包括空密码 | 不可能 |
| UART 上的登录提示 | 1 个（root） | 0 |
| setuid 二进制 | 15 | 8 |
| `device-service` 暴露评分（`systemd-analyze security`） | 9.4 UNSAFE | **1.8 OK** |
| 软件包 | 1918 | 1919（−4 个镜像构建期工具，+5 个 admin/sudo/沙箱） |
| 其中内核模块 | 1808 | 1808 |
| 根文件系统 | 121.7 MB，rw | 121.6 MB，ro |
| 未修补 CVE（用户态） | 16 | 4（§4） |

包的数量主要来自 `kernel-modules`，它安装内核构建出的**每一个**模块（1808 个包，26 MB），
因为这是 bring-up 时缺 WiFi 驱动的快速修复。换成这块板子实际加载的模块是显而易见的下一步精简；
这里没做，因为只能在板子上验证。

## 3. 给 `device-service` 加沙箱

服务需要的东西：`/dev/acq0`、驱动 sysfs 属性的写权限（背压要写 `sample_rate`）、给 devbus 用的
POSIX 共享内存、`sd_notify`，以及——仅当 `config.json` 要求时——`SCHED_FIFO` 和 `mlockall()`。
一个 drop-in 文件（[`hardening.conf`](../../../yocto/meta-device-platform/recipes-apps/device-service/files/hardening.conf)，
只由产线镜像安装）把其他一切都拿走：

- `CapabilityBoundingSet=CAP_SYS_NICE CAP_IPC_LOCK`、`NoNewPrivileges=yes`。进程保留 uid 0，
  因为 `/dev/acq0` 和 sysfs 属性属于 root；但没有 `CAP_DAC_OVERRIDE`、`CAP_SYS_MODULE`、
  `CAP_SYS_ADMIN`……的 root 只能碰它自己拥有的文件。
- `ProtectSystem=strict`、`ProtectHome`、`PrivateTmp`、`DevicePolicy=closed` +
  `DeviceAllow=/dev/acq0 rw`、`PrivateNetwork` 和 `RestrictAddressFamilies=AF_UNIX`
  （devbus 是共享内存；sd_notify 和 journald 走 Unix socket）、`SystemCallFilter=@system-service`，
  再加上常规的 `Protect*`/`Restrict*` 一整套。
- **有意不设** `ProtectKernelTunables`：它会让 `/sys` 只读，而背压正是靠往那里写 `sample_rate` 实现的。
  通用加固清单最容易在这种地方出错。

`systemd-analyze security --offline`（systemd 255，与镜像同一主版本）：**9.4 UNSAFE → 1.8 OK**。
完整输出：[`results/security/hardening/`](../../../results/security/hardening/)。

剩下的一步——以专用用户运行——需要一条给 `/dev/acq0` 的 udev 规则和 sysfs 属性的组权限，
留到能对着真实驱动测试时再做。

## 4. 漏洞管理（F2）

`INHERIT += "cve-check"` 会针对 NVD 数据库为每个 recipe 生成报告（数据库由构建下载：378,490 条，
没有 API key 要 49 分钟）。Scarthgap 还默认为每个镜像生成 SPDX 2.2 格式的 SBOM。

对 `device-platform-image-prod`，NVD 匹配给出 **3,869 条未修补**。这个数字在分诊之前毫无意义，
而分诊把它分成两个性质完全不同的问题。

### 用户态：16 → 4，每条都有理由

每一条都对照镜像实际包含的内容检查过：

| 软件包 | CVE | 证据 | 结论 |
| --- | --- | --- | --- |
| openssh | 6 | ssh 客户端、ssh-agent、X11 转发、GSSAPI、sshd——镜像里只有 `openssh-sftp-server`（由 `ssh-server-dropbear` 拉进来）；SSH 服务端是 dropbear | `not-applicable-config` |
| expat | 6 | 链接 libexpat 的程序只有 `dbus-daemon` 和它的启动辅助程序，解析的是只读 rootfs 上 root 拥有的配置——没有攻击者提供的 XML | `not-applicable-config` |
| glibc | CVE-2026-5450（9.8） | 需要带 `%mc` 且宽度 > 1024 的 `scanf`；**镜像里没有任何 ELF 包含 `%mc` 格式串** | `vulnerable-investigating`，下一版 scarthgap glibc 修复 |
| glibc | CVE-2026-5928 | `ungetwc` 配合重叠的多字节字符集；只有 libstdc++ 导入它，镜像用 C/UTF-8 | 未关闭，低 |
| glibc | CVE-2026-6238 | 已废弃的 `ns_printrr*`/`fp_nquery`——没有二进制导入它们 | 未关闭，低 |
| glibc | CVE-2010-4756 | glob() 通过 FTP 风格的模式耗尽 CPU/内存 | 未关闭，低 |

这些决定以 `CVE_STATUS` 记录在
[`recipes-security/cve-status/`](../../../yocto/meta-device-platform/recipes-security/cve-status/) 里，
下一份报告会把它们显示为 *Ignored* 并附上理由。每个文件里都写了一个注意事项：这些是关于**这个镜像**
的判断，却存放在**recipe** 上。如果有人加上了 ssh 客户端，openssh 那几条就错了。这种矛盾是
recipe 级 CVE 状态固有的，也是每条理由都要写明证据的原因。

### 内核：3,853 条，以及为什么答案是"跟 stable"

对 NVD 做版本匹配会报出所有版本范围覆盖 6.6.63 的 CVE，包括这个内核从来不编译的驱动。
poky 自带 `scripts/contrib/improve_kernel_cve_report.py`，它使用内核 CNA 自己的记录
（[vulns.git](https://git.kernel.org/pub/scm/linux/security/vulns.git)）和编译文件列表。
在 scarthgap + `linux-raspberrypi` 上它需要三个绕过办法，都在
[`security/cve/kernel-cve-triage.sh`](../../../security/cve/kernel-cve-triage.sh) 里：

1. recipe 版本号带 epoch（`1_6.6.63+git`），脚本解析不了；
2. scarthgap 的 cve-check JSON 没有逐条的 `detail` 字段，而脚本会去取它；
3. **`SPDX_INCLUDE_COMPILED_SOURCES` 对这个内核不起作用**：它生成的 SPDX 列出了整棵源码树的
   全部 33,155 个 `.c` 文件，连 AMD GPU 驱动都在里面，而构建实际只产生了 7,048 个目标文件。
   真实列表是从构建目录里的 `.o` 文件反推出来的（6,357 个 `.c`/`.S`），为保守起见所有头文件都算作已编译。

结果（[`results/security/cve/kernel-triage-summary.txt`](../../../results/security/cve/kernel-triage-summary.txt)）：

```
NVD version match only:   Unpatched 3853
+ kernel CNA + compiled:  Unpatched 3602, Ignored 3189 (2875 code not compiled, 314 rejected)
unpatched 3602: 1102 fixed in a later 6.6.y (newest needed 6.6.157)
                 323 fixed only in newer branches (6.12 / 6.18 / 7.x)
                2177 NVD-only, no fix data from the kernel CNA
```

两个结论：

- **编译文件过滤很重要**——2,875 个 CVE 所在的代码这个内核根本没有——但前提是文件列表正确。
  用 scarthgap 生成的那份列表，只过滤掉了 90 个。
- **对一个落后 94 个 stable 版本的内核，分诊不是解决办法。** meta-raspberrypi 的 scarthgap 分支
  钉死在 6.6.63（2024 年 12 月）；当前 6.6 stable 是 6.6.157。跟上 stable 分支一次就能修掉 1,102 条。
  "你怎么处理一百个内核 CVE"的答案是一个流程——跟 stable、重新构建、跑回归测试——只对 stable
  没覆盖到的少数几个做逐条分析。

## 5. 在板子上

每一项都在运行中的设备上检查过，包括反向的——本该失败的访问真的试过，并看到它失败了：

| 检查 | 结果 |
| --- | --- |
| `admin` 用 SSH 密钥登录，再 `sudo` | 成功 |
| `root` 用同一把密钥 | `Permission denied (publickey)` |
| `admin` 用密码 | `Permission denied (publickey)` —— 服务器根本不提供密码方式 |
| 经设备做端口转发到路由器的网页管理界面 | `channel open failed: administratively prohibited`（直接发同样的请求：HTTP 200） |
| 监听的 socket | 只有 22 端口（外加 127.0.0.53/54 上的 systemd-resolved） |
| 串口 getty | `masked`、`inactive` |
| `/` | `ext4 (ro,relatime)`；写 `/etc` → `Read-only file system` |
| `device-service` | active，MCU 应答，1000 样本/秒，无溢出 |
| 它的进程 | uid 0，`CapEff = CapBnd = 0x804000`（只有 `CAP_IPC_LOCK`、`CAP_SYS_NICE`），`NoNewPrivs=1`，seccomp 过滤开启，独立网络命名空间，看到的 `/` 只读、`/sys` 可写 |
| 沙箱下的背压 | 用相同的沙箱属性写 `sample_rate`：写入 900 并读回，再恢复 1000 |

只读根文件系统的两个后果，都是从构建输出预测到的，并且都**被一次重启证实**：

- **SSH 主机密钥每次开机都变**（`SHA256:/kjyH7sa…` → `SHA256:wDpLE42O…`；客户端报
  `Host key verification failed` 拒绝连接）。dropbear 把它放在 `/var/lib/dropbear`，那是 tmpfs。
  每次重启都变的指纹会训练管理员接受任何指纹——这正是中间人攻击需要的条件。修法：在 provisioning
  时生成每台设备自己的主机密钥，存在持久的数据分区上
  （[update-and-provisioning](update-and-provisioning.zh.md)）。把一把密钥烤进镜像会更糟——
  所有设备都共用它。
- **设备的 IP 地址每次开机都变**（`.177` → `.178`）——这个没预测到，是在板子上发现的。
  `/etc/machine-id` 在 tmpfs 上、每次开机重新生成，systemd-networkd 由它推导 DHCP 客户端标识（DUID），
  于是路由器看到的是一个新客户端。在 `wlan0.network` 里用 `[DHCPv4] ClientIdentifier=mac` 修复并
  **验证过**：machine-id 重启后仍然会变，地址不再变了。machine-id 本身也应该像主机密钥一样来自
  provisioning。修好 IP 反而让主机密钥问题**更显眼**了，这正是重点：地址稳定之后，每次重启管理员都会看到
  `WARNING: REMOTE HOST IDENTIFICATION HAS CHANGED! … someone could be eavesdropping on you right now
  (man-in-the-middle attack)`——人们一周之内就会学会无视这种警告。

剩下的两项在 `device-platform-image-ab` 里完成了（在板子上，[记录](../../../results/security/update/on-target-ab.txt)）：

- **device-service 以用户 `acq` 运行**，不再是 root：一条 udev 规则把 `/dev/acq0` 交给该组，并且只给它
  控制的那两个 sysfs 属性（`control`、`sample_rate`）写权限；`CapEff = CapBnd = 0x804000`；
  1000 样本/秒，无间断。
- **只装板子用到的内核模块**：列表来自运行中系统上的 `lsmod`，再去掉蓝牙和摄像头/编解码器栈
  （它们被加载只是因为硬件存在）。**1919 个包 → 171 个，1808 个内核模块 → 31 个。** 这次精简也去掉了
  coreutils，连带去掉了 `install`，而本项目有个脚本依赖它——这件事是怎么暴露出来的见
  [update-and-provisioning](update-and-provisioning.zh.md) §3.1。

## 6. 二进制的编译期加固

`checksec` 会报告的那些项目，由
[`binary-hardening-report.py`](../../../security/hardening/binary-hardening-report.py) 对
`device-platform-image-ab` 根文件系统里的每个 ELF 文件统计（pyelftools，只读头部——不需要运行
aarch64 代码；[完整列表](../../../results/security/hardening/binary-hardening.txt)）：

| | 可执行文件（178） | 共享库（94） |
| --- | --- | --- |
| PIE | 100% | — |
| full RELRO（`-z relro -z now`） | 100% | 100% |
| NX 栈 | 100% | 100% |
| 看到 stack canary（`__stack_chk_fail`） | 96.6% | 85.1% |
| 看到 FORTIFY（`__*_chk`） | 88.2% | 63.8% |
| 标记了 BTI + PAC | 100% | 97.9% |

这些都不需要额外添加：poky 的发行版配置 require 了 `security_flags.inc`（`-fstack-protector-strong`、
`-D_FORTIFY_SOURCE=2`、PIE、`-z relro -z now`），poky 通用的 arm64 tune（`arch-arm64.inc`）为每个
aarch64 machine 加了 `-mbranch-protection=standard`。`device-service`、`devbus` 和 `acq-bridge`
得到同样的 flag，因为它们是 BitBake 构建的（SDK 构建也一样，SDK 的 `$CC` 带着这些 flag——
用一个测试程序检查过）。

正确解读这些数字：

- **canary 和 FORTIFY 两列是下限。** 函数只有在栈上有值得保护的东西时才会得到 canary，程序只有在编译器
  找到了能检查的调用时才会显示 FORTIFY。那 6 个看不到 canary 的可执行文件都是很小的程序
  （`dbus-uuidgen`、`systemd-ac-power`……），不是没加 flag 编译的。glibc 本身有意不用 `SECURITY_CFLAGS`
  构建——它就是实现这些检查的代码。
- **工具用已知答案校验过。** 同一个测试程序用 SDK 的 flag 构建，每一列都显示；用
  `-fno-stack-protector -U_FORTIFY_SOURCE -no-pie -z norelro -z lazy` 构建，一列都不显示。
  作为第二个参照，[optee](optee.zh.md) 里 OP-TEE 的 Buildroot rootfs：57% 的库 full RELRO，
  54% 有 canary，BTI/PAC 一个都没有——另一个构建系统的默认值，在同一份报告里一目了然。
- **BTI 和 PAC 标记了，但在这块板子上什么都不做。** 两者都编码在 hint 指令空间里，这样同一个二进制
  到处都能运行；在不支持它们的 CPU 上，这些指令就是 NOP。Pi 5 的 Cortex-A76 是 Armv8.2——
  指针认证（PAC）在 Armv8.3 才出现，BTI 在 Armv8.5。这些二进制为更新的芯片（Cortex-A78AE、
  A720……）准备好了；在这块芯片上，返回地址保护只靠 canary。有两个库（`libffi`、`libzstd`）完全没标记：
  它们含有手写汇编，没有 BTI 着陆点，而只要有一个目标文件缺这个属性，链接器就会为整个文件去掉它。

## 7. 内核：配置和运行时设置

### 配置

[`kconfig-hardening-check.py`](../../../security/hardening/kconfig-hardening-check.py)
检查 44 个选项——从内核自我保护项目（KSPP）的建议里手挑的 arm64 子集，每项理由都写在脚本里
（完整的 [kernel-hardening-checker](https://github.com/a13xp0p0v/kernel-hardening-checker) 有几百项，
是该在 CI 里跑的工具；`pip install git+https://github.com/a13xp0p0v/kernel-hardening-checker`）。
之前：A/B 镜像跑的 6.6.63 内核。之后：6.12.93 加上 fragment
[`hardening.cfg`](../../../yocto/meta-device-platform-verity/recipes-kernel/linux/files/hardening.cfg)
（[记录](../../../results/security/hardening/kernel-config-hardening.txt)）：

| | 6.6.63（刷写的） | 6.12.93 + `hardening.cfg`（1.1.0） | 1.2.x |
| --- | --- | --- | --- |
| 自我保护（31 项） | 12 | 27 | 28 |
| 攻击面（13 项） | 3 | 6 | 8 |
| **合计** | **15 / 44** | **33 / 44** | **36 / 44** |

1.2.x 加上的是板子必须先验证过才能开的：`MODULE_SIG_FORCE`（在确认每个模块、包括 `custom_acq`，
都已签名且被接受之后），以及关掉 `COMPAT` 和 `IO_URING`（镜像里没有 32 位 ELF，也没有 io_uring 的用户；
只剩下 `__arm64_sys_io_uring_*` 这些入口名，调用返回 `ENOSYS`）。剩下的八项是下面有意不开的。

改了什么，每项换来什么：

- **堆和拷贝加固** —— `HARDENED_USERCOPY`、`FORTIFY_SOURCE`、`SLAB_FREELIST_HARDENED`/`_RANDOM`、
  `SHUFFLE_PAGE_ALLOCATOR`、`INIT_ON_ALLOC_DEFAULT_ON`、`LIST_HARDENED`、`BUG_ON_DATA_CORRUPTION`：
  这些选项把内核内存破坏 bug 从"可利用"变成"崩溃"。Raspberry Pi 的 defconfig 为兼容性和速度优化，
  全都没开。
- **`STRICT_DEVMEM` + `IO_STRICT_DEVMEM`** —— 在原版 Pi 内核上，root 可以通过 `/dev/mem` 读写**全部**
  物理内存，这让其他所有内核防护对一个拿到 root 的攻击者来说都形同虚设。
- **`MODULE_SIG` + `MODULE_SIG_ALL`**，暂时还不是 `MODULE_SIG_FORCE`：每个树内模块在构建时用一把
  内核构建生成、用完即丢弃的密钥签名。强制之前，板子必须证明树外的 `custom-acq` 也被签了——否则
  强制会把产品自己的驱动挡在外面。（B6 期间曾从 `/tmp` `insmod` 过一个测试驱动；强制签名正是让这种事
  变得不可能的东西。）
- **Yama**、`SECURITY_DMESG_RESTRICT`，以及关掉三块攻击面（`HIBERNATION`、`LEGACY_TIOCSTI`、`LDISC_AUTOLOAD`）。

有意保持原样的（"为什么不开"写在 fragment 里）：`INIT_ON_FREE_DEFAULT_ON`（每次 free 都有可测量的代价）、
lockdown LSM、`KPROBES` 和 `FTRACE`（本项目用 ftrace 调试——case 04 和 09 就是靠它解决的；产线内核应该
去掉它们，另外保留一个调试构建）、`COMPAT` 和 `IO_URING`（当时还没证明用不到）。
`ARM64_BTI_KERNEL`/指针认证需要 A76 没有的 CPU 特性（§6）。

### 运行时设置

[`90-device-platform-hardening.conf`](../../../yocto/meta-device-platform-verity/recipes-support/device-platform-ab/files/90-device-platform-hardening.conf)
（sysctl.d，开机时由 systemd 应用）：`kptr_restrict=2`、`dmesg_restrict=1`、关闭非特权 BPF 并开启 JIT 加固、
关闭 `kexec` 和 SysRq、setuid 程序不产生 core dump、不自动加载行规程（line discipline）、Yama
`ptrace_scope=1`、禁止非特权 userfaultfd，以及终端设备的网络设置（不接受重定向、不接受源路由、开启反向路径过滤）。
内核没有的 key 前面加 `-`，这样缺少的选项会被跳过，而不是让整个 unit 失败。

### 内核 6.12：升级之后做同样的分诊

上面的结论是"跟 stable"。meta-raspberrypi 的 scarthgap 分支也带有 `linux-raspberrypi_6.12.bb`
（6.12.93，Raspberry Pi OS 自己已经转向的较新长期支持分支）；`device-platform-image-ab` 现在构建它
（verity 层里的 `PREFERRED_VERSION_linux-raspberrypi = "6.12%"`）。同一个 NVD 数据库和 vulns.git 检出，
同一个脚本（[6.6 记录](../../../results/security/cve/kernel-triage-summary.txt)，
[6.12 记录](../../../results/security/cve/kernel-triage-summary-6.12.txt)）：

| | 6.6.63 | 6.12.93 |
| --- | --- | --- |
| 仅 NVD 版本匹配 | 3,853 未修补 | 782 未修补 |
| 加上内核 CNA 数据 + 编译文件过滤之后 | **3,602** | **1,789** |
| 其中：同一分支的后续版本已修复 | 1,102（到 6.6.157） | 1,093（到 6.12.111） |
| 只在更新的分支里修复 | 323 | 243 |
| 仅 NVD 有，CNA 没有修复数据 | 2,177 | 453 |

- **分诊后的总数减半，只靠一行配置。** 减少的大部分来自"仅 NVD"那一列：NVD 的版本范围挂到 6.6 上、
  而 CNA 从未给出修复的老 CVE。
- **对 6.12，CNA 数据反而*增加*了条目**（782 → 1,789）。NVD 对近期内核 CVE 的补录滞后；版本范围缺失，
  所以版本匹配找不到它们。内核 CNA 的记录能找到。只用 NVD 生成的报告会显得比 6.6 好五倍——而且是错的。
  要比较的是分诊后的数字。
- **同样的流程缺口依然存在：** 6.12.93 落后 6.12.111 共 18 个版本，1,093 个修复在 stable 里等着。
  升级只是把计数器清零；只有跟 stable 分支才能让它保持在低位。
- 本项目的升级代价是两行：驱动的 `<asm/unaligned.h>` 在 6.12 里挪到了 `<linux/unaligned.h>`，
  RP1 南桥驱动变成了内建，所以它的模块包从镜像的模块列表里消失了（失败的是镜像构建，不是设备）。

### 在板子上

在运行中的 6.12 镜像上读回（[记录](../../../results/security/update/on-target-signed-ab.txt)），
因为**配置是一个请求，而不是一个确认**：

- **每个 sysctl 都按写的生效了**，除了三个不存在的 key——而每一个不存在的含义都不同：
  - `kexec_load_disabled`、`unprivileged_userfaultfd`：这两个功能被编译掉了（`KEXEC=n`、
    `USERFAULTFD=n`），这比一个开关更好。
  - `kernel.yama.ptrace_scope`：Yama **确实**编进去了，但 `/sys/kernel/security/lsm` 只显示
    `capability`。Raspberry Pi 的 defconfig 里是 `CONFIG_LSM=""`，空列表意味着一个 LSM 都不初始化——
    同样编进去的 AppArmor 也一样没激活。用 `CONFIG_LSM="yama"` 修复，作为更新 1.1.1 通过 A/B 下发：
    `capability,yama`，`ptrace_scope = 1`。**编进去不等于启用了。**
- **所有模块都签了名，并且被接受**：`custom_acq` 只带 `O` 污点（树外），没有 `E`（未签名）——它用
  构建密钥做的签名被接受了——所以 1.2.0 开启了 `MODULE_SIG_FORCE`，驱动照常加载（只有 `O` 污点）。
  没用内核构建期密钥编译的模块再也加载不了，谁都不行，从 `/tmp` `insmod` 也不行。
- **`FORTIFY_SOURCE` 在第一次开机就发现了东西**：WiFi 驱动（`brcmfmac/fweh.c:466`）里
  `memcpy: detected field-spanning write (size 27) of single field "eventmask_msg->mask"`。
  结构体声明的是 `u8 mask[1]`——变长尾部的老式写法——而缓冲区是按完整长度分配的，所以没有内存被覆盖；
  上游修这种模式的办法是把这类字段改成柔性数组。这项检查只是警告并放行（`W` 污点）。这正是这个选项的用途：
  在没人在这里写过的代码中，找出编译器无法证明大小的拷贝。
- `DEBUG_WX`：`Checked W+X mappings: passed, no W+X pages found`。
