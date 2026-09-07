# meta-device-platform 讲解（中文，学习用）

通用概念（layer/recipe/image 是什么、BitBake 怎么跑、`bblayers.conf`
和 `layer.conf` 的区别、优先级机制）都在
`docs/notes/systems-programming-patterns.md`的"Yocto/BitBake"一节，那份是
换任何项目都适用的地基知识，这份文档**不重复讲**，只讲这个项目具体
做了什么业务决定、踩了什么坑。

## 自上而下：这一层解决的是"造镜像需要的哪几类东西"

造一个能用的嵌入式 Linux 系统，从零开始通常要凑齐这五类东西（这是
Yocto/OpenEmbedded 社区约定的分类习惯，对应 `recipes-*` 开头的目录
名，不是这个项目发明的）：

| 目录 | 解决的问题 | 这个项目里放了什么 |
|---|---|---|
| `recipes-kernel/` | 硬件驱动（内核这一层的东西） | `custom-acq-driver`：咱们的 SPI 采集驱动 |
| `recipes-bsp/` | 板级支持——硬件描述、bootloader 配置，非驱动本身但跟具体板子绑定 | `custom-acq-overlay`：Device Tree overlay，告诉内核咱们的外设长什么样、接在哪 |
| `recipes-apps/` | 业务应用程序（用户空间） | `device-service`：C++ 采集服务 |
| `recipes-support/` | 辅助性配置，不是独立软件，更像"粘合剂" | `device-platform-network`：WiFi/SSH 配置 |
| `recipes-core/images/` | 总装——把以上全部 + 官方基础系统组装成一个可烧录的镜像 | `device-platform-image` |

`recipes-kernel`（驱动）和 `recipes-bsp`（Device Tree）放在一起理解
最清楚：**bootloader 启动时把内核和 Device Tree 一起加载，内核拿到
Device Tree 之后，才知道"这块板子上接了哪些外设、每个外设该配哪个
驱动"，然后去匹配对应的驱动完成绑定**——这跟 `driver/custom-acq/
CODE_WALKTHROUGH.zh.md` 里讲的 `of_device_id`/`MODULE_DEVICE_TABLE`
匹配机制是同一件事，只是这次是从"系统启动流程"的角度再看一遍。

`recipes-core/images/device-platform-image.bb` 这个总装配方，本质上
就是一张购物清单——把上面四类自己写的东西的名字，加上官方已经提供
好的基础系统（内核本身、C 库、systemd……）的名字，全部列在一起，交
给 BitBake 去组装。以后如果还要加新类别的东西（比如加一个日志采集
服务），大概率也是新开一个 `recipes-xxx/` 目录，再把它的名字加进这
张购物清单。

## 每个文件的业务决定（不重复讲通用概念，只讲"为什么这么写"）

### `conf/layer.conf`

除了 `bitbake-layers create-layer` 自动生成的标准身份声明（层名、优
先级），额外加了一行 `RPI_EXTRA_CONFIG += "dtoverlay=custom-acq"`
（启用 Device Tree overlay 的开关）。**这行为什么放在这里、不放在
镜像配方里**：生成 `config.txt` 的 `rpi-bootfiles` 是一个独立于任
何具体镜像的共享配方，不会读某个镜像配方自己的变量作用域——凡是要
影响"共享配方"的配置，必须放在全局作用域（`layer.conf`/
`local.conf`），这是这次调试花时间最多的一个坑，放镜像配方里完全
不报错、只是悄悄不生效。

### `recipes-kernel/custom-acq-driver_1.0.bb`

`inherit module`（外部内核模块的标准流水线），指回 `driver/
custom-acq/` 而不是复制源码。踩的坑：驱动的 `Makefile` 自己定义的
变量名（`KDIR`，给手动 `make` 用）跟 Yocto 约定的变量名
（`KERNEL_SRC`）对不上暗号，编译时会悄悄用**这台编译主机自己的内
核**（完全编译错方向，还不报"用错内核"这种明显错误）——用
`EXTRA_OEMAKE += "KDIR=${STAGING_KERNEL_DIR}"` 显式传参解决，不改
共享 Makefile（不破坏手动 `make` 那条已经写进 `device-tree/
README.md` 的路径）。另外补了一个 `/etc/modules-load.d/` 兜底，保
证开机一定加载这个模块，不完全依赖硬件热插拔自动探测。

### `recipes-bsp/custom-acq-overlay_1.0.bb`

**没有 `inherit` 任何现成 class**——Yocto 自带的
`devicetree.bbclass` 部署路径跟 meta-raspberrypi 自己的
`overlays/` 约定不兼容，索性自己手写"编译→安装→部署"三个步骤，本
质就是把 `device-tree/README.md` 里那条手动 `dtc` 命令原样搬进来，
让 BitBake 在正确的时机自动跑。产物 `custom-acq.dtbo` 放的位置（部
署目录根下，不是子目录）是查了 meta-raspberrypi 的
`make_dtb_boot_files()` 源码才确认的——这是"用别人的机制就得照别人
的规矩来"的例子。

### `recipes-apps/device-service_1.0.bb`

`inherit cmake systemd`，整个 `userspace/device-service/` 目录作为
一个 `SRC_URI` 条目直接拉进来（不是一个个文件列，目录里文件多的时
候更省事）。给 `device-service/CMakeLists.txt` 补了一行
`install(TARGETS ...)`（原来完全没有 install 规则）——Yocto 的
`cmake.bbclass` 完全靠 CMake 自己的 `install()` 规则决定往镜像里
装什么，这行缺了，`cmake.bbclass` 的默认安装步骤会"成功"但什么也
不装。配置文件、systemd unit 文件不归 CMake 管，在
`do_install:append()` 里单独补上——"代码怎么编"是 CMake 的事，"代
码之外的东西怎么装"是 Yocto recipe 的事，这条分工线在这个文件里体
现得很清楚。

### `recipes-support/configuration/device-platform-network_1.0.bb`

WiFi 密码处理：真实 `wpa_supplicant.conf` 放在 `/opt/yocto/
local-config/`（完全在这个 git 仓库目录树之外），layer 里只有一份
占位模板，靠 `local.conf` 里一个变量（`DEVICE_PLATFORM_WPA_CONF`）
指向真实文件——这样 clone 仓库下来就能编译（用占位模板，WiFi 连不
上但不会编译失败），真实密码永远不会出现在任何一次 commit 里。自
己写了 `wpa_supplicant-wlan0.service`（没用上游自带的
`wpa_supplicant.service`，那个是 DBus 驱动模式，不会自动读一份静
态配置文件去连特定的 WiFi）+ 一个 `systemd-networkd` 的
`wlan0.network`（负责连上之后拿 DHCP 地址）+ 一个 systemd preset
文件专门用来启用 `systemd-networkd.service`（它是 `systemd` 包自
带的服务，不是这个 recipe 自己打的包，不能用
`SYSTEMD_AUTO_ENABLE`，preset 是"启用别人打的包里的服务"的标准解
法）。

### `recipes-core/images/device-platform-image.bb`

`require core-image-minimal.bb`（复用官方参考镜像的定义，不是复制
粘贴），在此基础上把前面四类自己的 recipe 名字 + WiFi 相关的固件/
驱动包名，加进购物清单（`IMAGE_INSTALL`）。**后面这几个 WiFi 相关
的包是踩坑之后才补的**：`core-image-minimal` 默认只装最基础的
`packagegroup-core-boot`，不装 `packagegroup-base`（真正负责拉取
WiFi 固件/内核模块这些"硬件支持"的那个包组）——固件和驱动模块虽然
编译出来了，但从没被装进镜像，实机测试时 `wlan0` 这个网卡压根不存
在才发现。另外 WiFi 固件是 Broadcom 的二进制固件，不是完全开源许
可，Yocto 要求在 `local.conf` 里显式接受
（`LICENSE_FLAGS_ACCEPTED += "synaptics-killswitch"`）才让编译。

`custom-acq-overlay` 那个 `.dtbo` 真正生效，其实是三处配合完成的：
**编出来（`custom-acq-overlay` recipe）→ 复制到启动分区（这个文件
里的 `EXTRA_IMAGEDEPENDS`/`KERNEL_DEVICETREE:append`）→ 树莓派固件
真正加载它（`layer.conf` 里那行 `config.txt` 配置）**——缺任何一
环，`.dtbo` 文件会老老实实"存在"，但驱动永远绑定不上真实硬件，而
且没有任何报错提示少了哪一步，这三处的关联只能靠实机测试确认。

## 实机验证结果

写卡到真实 Pi 5、真实 MCU 通电后，SSH 上去确认：驱动自动加载
（`lsmod | grep custom_acq`）、`/dev/acq0` 存在、`device-service`
在 systemd 下是 `active` 且读到真实递增的样本数据、`device_id`/
`fw_version` 跟硬件已知值完全一致。完整过程和踩过的坑见
`private/session-log.md` 2026-09-03 的两条 V6 记录。
