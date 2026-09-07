# Yocto 编译环境搭建（V6）

这份文档记录**怎么从零搭出能编译 `device-platform-image` 的环境**。
只有 `meta-device-platform/`（这个项目自己写的层）进这个 git 仓库；
`poky`/`meta-openembedded`/`meta-raspberrypi`（官方/社区维护的上游
层）和 `build/`（编译产物、缓存，几十 GB）都不进——概念上的原因见
`docs/notes/systems-programming-patterns.md`的"Yocto/BitBake"一节。

## 一次性环境准备

1. **主机装编译依赖**（AlmaLinux/RHEL 9 家族，来源：Yocto 官方系统
   需求文档）：
   ```bash
   sudo dnf install -y epel-release dnf-plugins-core
   sudo dnf config-manager --set-enabled crb
   sudo dnf makecache
   sudo dnf install -y bzip2 ccache chrpath cpio cpp diffstat diffutils \
     gawk gcc gcc-c++ git glibc-devel glibc-langpack-en gzip libacl make \
     patch perl perl-Data-Dumper perl-Text-ParseWords perl-Thread-Queue \
     python3 python3-GitPython python3-jinja2 python3-pexpect python3-pip \
     rpcgen socat tar texinfo unzip wget which xz zstd lz4
   ```
   （`lz4` 官方文档没提到，实测 `bitbake-layers` 会因为缺 `lz4c` 直
   接报错，是这次补上的坑。）

2. **拉三个上游层**（固定在 Scarthgap，5.0 LTS，选它是因为对 Pi 5
   的支持比最新版更成熟、社区踩坑记录更多）：
   ```bash
   mkdir -p /opt/yocto && cd /opt/yocto
   git clone -b scarthgap https://git.yoctoproject.org/git/poky poky
   git clone -b scarthgap https://github.com/openembedded/meta-openembedded meta-openembedded
   git clone -b scarthgap https://git.yoctoproject.org/git/meta-raspberrypi meta-raspberrypi
   ```
   用 https 不用 `git://`（后者走的端口在某些网络环境下会被挡）。

3. **建编译目录**：
   ```bash
   cd /opt/yocto/poky
   source oe-init-build-env /opt/yocto/build
   ```
   这一步会自动生成 `build/conf/local.conf`/`bblayers.conf` 的默认
   模板，接下来几步是往这两个文件里加东西。

4. **把这个项目的层，连同两个上游依赖层，一起注册**：
   ```bash
   bitbake-layers add-layer /opt/yocto/meta-openembedded/meta-oe
   bitbake-layers add-layer /opt/yocto/meta-raspberrypi
   bitbake-layers add-layer /path/to/这个仓库/yocto/meta-device-platform
   bitbake-layers show-layers   # 确认四层都在，没拼错路径
   ```

5. **`build/conf/local.conf` 里必须有这几行**（这个文件本身不进 git，
   下面是内容，不是文件本身）：
   ```
   MACHINE = "raspberrypi5"

   # device-service 用 sd_notify()/systemd watchdog（V4 Phase 2），
   # Poky 默认是 sysvinit，得显式切到 systemd。
   DISTRO_FEATURES:append = " systemd usrmerge"
   VIRTUAL-RUNTIME_init_manager = "systemd"
   VIRTUAL-RUNTIME_initscripts = ""
   VIRTUAL-RUNTIME_syslog = ""

   # WiFi 固件是 Broadcom 二进制固件，非完全开源许可，必须显式接受。
   LICENSE_FLAGS_ACCEPTED += "synaptics-killswitch"

   # 真实 WiFi 密码——指向仓库之外的一个本地文件，内容见下一节。
   DEVICE_PLATFORM_WPA_CONF = "/opt/yocto/local-config/wpa_supplicant.conf"

   # 这台机器资源比 Yocto 官方参考配置小，保守设并发度，避免 OOM。
   BB_NUMBER_THREADS = "4"
   PARALLEL_MAKE = "-j 4"
   ```

## WiFi 真实密码怎么放（每台新机器都要重新做一次，不在 git 历史里）

```bash
mkdir -p /opt/yocto/local-config
cat > /opt/yocto/local-config/wpa_supplicant.conf <<'EOF'
ctrl_interface=/var/run/wpa_supplicant
update_config=1

network={
	ssid="真实 WiFi 名字"
	psk="真实密码"
}
EOF
chmod 600 /opt/yocto/local-config/wpa_supplicant.conf
```

模板在仓库里：`meta-device-platform/recipes-support/configuration/
files/wpa_supplicant.conf.example`。

## 编译

```bash
source /opt/yocto/poky/oe-init-build-env /opt/yocto/build
bitbake device-platform-image
```

单独编译某一个自定义 recipe（不用编整个镜像，调试单个 recipe 时更
快）：

```bash
bitbake custom-acq-driver      # 内核驱动
bitbake custom-acq-overlay     # Device Tree overlay
bitbake device-service         # C++ 服务
bitbake device-platform-network  # WiFi/SSH 配置
```

## 写卡

产物在 `/opt/yocto/build/tmp/deploy/images/raspberrypi5/
device-platform-image-raspberrypi5.rootfs.wic.bz2`。Raspberry Pi
Imager 的"Use custom"能直接选 `.wic.bz2`；如果选择框只认识 `.img`
后缀，先解压一次：

```bash
bunzip2 -k -c device-platform-image-raspberrypi5.rootfs.wic.bz2 > device-platform-image-raspberrypi5.img
```

写卡验证结果见 `private/session-log.md`。
