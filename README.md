# chuart

把 CH914X 低功耗蓝牙转串口模组（CH9140lib）变成 **Linux 里的一个字符设备**，让
`picocom`、`minicom`、`socat` 或任意脚本直接按串口使用。

```bash
$ sudo chuart serve --mac 50:54:7B:55:58:1D --owner $USER --mode 0660
[10:39:17] INF cuse: /dev/ttyCH9140 ready
[10:39:17] INF ble: 50:54:7B:55:58:1D is not in the BlueZ cache, scanning for up to 5 second(s)
[10:39:23] INF connecting to 50:54:7B:55:58:1D...
[10:39:24] INF ble: connected to 50:54:7B:55:58:1D
[10:39:24] INF ble: MTU 200
[10:39:24] INF ble: notifications enabled on 0xfff1
[10:39:24] INF device ready: /dev/ttyCH9140 (MTU 200)
device ready: /dev/ttyCH9140
try: picocom --nolock -b 115200 /dev/ttyCH9140
press Ctrl-C to stop

$ picocom --nolock -b 115200 /dev/ttyCH9140      # 普通用户即可
```

## 安装

```bash
sudo scripts/install.sh                 # 装到 /usr/local/bin 与 /usr/local/lib
chuart scan
```

脚本会自动构建、安装二进制与随仓库的 `libCH9140lib.so`，并刷新动态库缓存。
其他用法：


| 命令                                             | 说明                                                           |
| ---------------------------------------------- | ------------------------------------------------------------ |
| `sudo scripts/install.sh --prefix /opt/chuart` | 装到指定前缀                                                       |
| `sudo scripts/install.sh --install-deps`       | 先装好构建依赖                                                      |
| `sudo scripts/install.sh --with-cuse-rule`     | 额外装 udev 规则，让 `dialout` 组能打开 `/dev/cuse`，即 `serve` 不再需要 root |
| `sudo scripts/install.sh --uninstall`          | 卸载                                                           |
| `scripts/install.sh --check`                   | 只做环境自检                                                       |


- 若省略 `--with-cuse-rule`，`serve` 仍需 `sudo`。
- 构建依赖交给脚本：`--install-deps` 会按你的包管理器安装 fuse3 / glib / bluez 的开发文件。

## 手动构建

```bash
cmake -S . -B build
cmake --build build
./build/chuart help          # 直接跑构建产物
cmake --install build --prefix ~/.local   # 或者手动装
```

## 用法

表格用 `-` 分隔线框起来，只写 stdout；进度与摘要在 stderr，所以
`chuart scan > devices.txt` 拿到的是干净的一张表。终端里默认带颜色（表头粗体、设备名
青色、MAC 与时间暗色、RSSI 按强弱显示绿/黄/红），输出被重定向或管道时自动去掉颜色，
环境变量 `NO_COLOR` 同样可以关闭。

### 设备名字：用 `-d` 代替 MAC

每台模组第一次被看到时会自动获得一个名字（形容词 + 水果，camelCase，如
`excitedApple`），固定 10 × 10 = 100 种组合，存在下面这个文件里：

```
~/.config/chuart/devices.conf        # 纯文本，可手改
```

之后连接就不用敲 MAC 了：

```bash
chuart scan                      # 会为发现的模组分配名字
chuart devices                   # 列出记得的模组
chuart serve -d excitedApple     # 等价于 --mac 50:54:7B:55:58:1D
chuart rename excitedApple lab-rig
```

- 用 `sudo` 跑 `serve` 时，读写的仍是**运行 sudo 的那个用户**的注册表（靠 `SUDO_USER`
  解析），不会跑到 `/root` 下，也不会留下 root 独占的文件。
- 名字可以随时改（`chuart rename`，或直接编辑那个文件）；名字只允许字母、数字、`-`、`_`，
  最长 23 字符，且不能与已有的重名。
- 某个模组即使名字表用满了也不影响使用：`chuart` 会提示无法命名，然后照常用 MAC 连接。

### 扫描

```bash
chuart scan                 # 默认 5 秒
chuart scan -t 8
chuart scan --json          # 给脚本用
```

```
DEVICE           ADDRESS            MODULE                        RSSI  CHIP
braveFig         50:54:7B:55:58:1D  CH9141BLE2U                -42 dBm  chip_ver=67 (0x43)
```

`DEVICE` 列就是自动分配的名字，`MODULE` 列是模组自己广播的名字，二者不同。
扫描会把新发现的模组记进注册表（所以 `scan` 也会写那个文件）。

### 暴露成 /dev 节点

```bash
sudo chuart serve -d excitedApple --owner $USER --mode 0660   # 或 --mac <MAC>
```

常用选项：


| 选项                                              | 说明                                      |
| ----------------------------------------------- | --------------------------------------- |
| `-d, --device <name>`                           | 按名字选择模组（`chuart devices` 可查）            |
| `-m, --mac <MAC>`                               | 按地址选择模组（二者之一，不能同时给）                     |
| `-n, --name <name>`                             | 设备名，默认 `ttyCH9140` → `/dev/ttyCH9140`   |
| `-u, --notify-uuid`                             | 通知特征，默认 `0xfff1`                        |
| `-M, --mtu <n>`                                 | 限制写方向的 MTU（默认用模组协商值），如 `-M 64`          |
| `-T, --connect-timeout <ms>`                    | 连接超时，默认 15000                           |
| `-Y, --scan-time <seconds>`                     | BlueZ 缓存里没有该设备时先扫描的秒数（默认 5）；0 = 不扫描直接连接 |
| `-o, --owner user[:group]`、`-O, --mode <octal>` | 节点属主/权限                                 |
| `-v` / `-q`                                     | 调试日志 / 只报错误                             |


`serve` 在前台运行，按 `Ctrl-C` 退出并自动删除设备节点。

### 快速自检

```bash
chuart version                       # 库与适配器是否可用
chuart scan -t 5                     # 能否发现模组
chuart features -d <name>            # 特征是否符合预期（0xfff1 通知）
```

### 与 picocom / minicom 配合

```bash
picocom --nolock -b 115200 /dev/ttyCH9140
minicom -D /dev/ttyCH9140 -b 115200
socat - /dev/ttyCH9140,raw,echo=0
```

## 仓库结构

```
CMakeLists.txt
src/
  main.c        子命令、写线程、信号监督
  ble.c/.h      CH9140lib 封装
  devices.c/.h  设备名字注册表（名字生成、持久化、改名）
  cuse_tty.c/.h CUSE tty 模拟（termios/ioctl/poll）
  ring.c/.h     有界环形缓冲
  log.c/.h      日志
  lib/          CH9140lib.h + libCH9140lib.so（厂商预编译）
docs/
  CH9140lib.md  厂商 API 整理 + 实测修正
scripts/
  install.sh    安装/卸载（可选装 /dev/cuse 的 udev 规则）
```