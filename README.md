# chuart

把 CH914X 低功耗蓝牙转串口模组（CH9140lib）变成 **Linux 里的一个字符设备**，让
`picocom`、`minicom`、`socat` 或任意脚本直接按串口使用。它只做**映射**：串口参数
（波特率/数据位/停止位/校验位/流控/DTR/RTS）全部由客户端决定，`chuart` 不自行配置模组。

```bash
$ sudo chuart serve --mac 50:54:7B:55:58:1D --owner $USER --mode 0660
[18:13:58] INF ble: connected to 50:54:7B:55:58:1D
[18:13:58] INF ble: MTU 200
[18:13:58] INF ble: notifications enabled on 0xfff1
[18:13:59] INF ble: module UART set to 115200 8N1
[18:13:59] INF device ready: /dev/ttyCH9140 (MTU 200, 115200 8none1)
device ready: /dev/ttyCH9140
try: picocom --nolock -b 115200 /dev/ttyCH9140

$ picocom --nolock -b 115200 /dev/ttyCH9140      # 普通用户即可
```

- 详细的库函数说明：[docs/CH9140lib.md](docs/CH9140lib.md)

## 依赖

```bash
sudo apt install build-essential cmake pkg-config \
                 libfuse3-dev libglib2.0-dev libbluetooth-dev
# 运行期还需要 bluez（提供 /dev/cuse 的 cuse 模块通常已内置）
sudo modprobe cuse   # 若 /dev/cuse 不存在
```

## 构建

```bash
cmake -S . -B build
cmake --build build
./build/chuart help
```

`build/chuart` 已设置 RPATH，可直接运行，无需 `LD_LIBRARY_PATH`
（`src/lib/libCH9140lib.so` 是随仓库提供的预编译库）。

## 用法

### 扫描

```bash
chuart scan                 # 默认 5 秒
chuart scan -t 8
chuart scan --json          # 给脚本用
```

```
ADDRESS            NAME                                  RSSI  CHIP
50:54:7B:55:58:1D  CH9141BLE2U                        -42 dBm  chip_ver=67 (0x43)
```

同一模组会被广播重复上报，`scan` 按地址去重并保留最强 RSSI。

### 查看 GATT 结构（选 --notify-uuid 用）

```bash
chuart features --mac 50:54:7B:55:58:1D
```

```
  service 0x001f  0xfff0  (handles 0x001f-0x0027)
    0x0020  0xfff1    props=0x12  [ read notify ]  <- notify/data RX, chuart's default --notify-uuid
    0x0027  0xfff3    props=0x1a  [ read write notify ]  <- config baud/modem, hardcoded by CH9140lib
    0x0024  0xfff2    props=0x0c  [ write-nr write ]  <- data/TX, hardcoded by CH9140lib
```

CH914X 的约定是：`0xfff1` 通知下行、`0xfff2` 上行写、`0xfff3` 配置（后两者由
CH9140lib 内部固定使用）。因此正常情况下不需要传 `--notify-uuid`。

### 暴露成 /dev 节点

```bash
sudo chuart serve --mac 50:54:7B:55:58:1D --owner $USER --mode 0660
```

常用选项：

| 选项 | 说明 |
| --- | --- |
| `-m, --mac <MAC>` | 目标模组地址（必填） |
| `-n, --name <name>` | 设备名，默认 `ttyCH9140` → `/dev/ttyCH9140` |
| `-u, --notify-uuid` | 通知特征，默认 `0xfff1` |
| `-M, --mtu <n>` | 限制写方向的 MTU（默认用模组协商值），如 `-M 64` |
| `-T, --connect-timeout <ms>` | 连接超时，默认 15000 |
| `-Y, --scan-time <seconds>` | BlueZ 缓存里没有该设备时先扫描的秒数（默认 5）；0 = 不扫描直接连接 |
| `-o, --owner user[:group]`、`-O, --mode <octal>` | 节点属主/权限 |
| `-I, --ignore-client-params` | 只做数据搬运，绝不把客户端的串口参数下发给模组 |
| `-v` / `-q` | 调试日志 / 只报错误 |

`serve` 在前台运行，按 `Ctrl-C` 退出并自动删除设备节点。

跑起来之后，客户端的串口参数会**实时**下发到模组，不用重启：

```bash
sudo chuart serve --mac 50:54:7B:55:58:1D --owner $USER --mode 0660   # 初始 115200
picocom --nolock -b 9600  /dev/ttyCH9140     # → 模组 UART 改成 9600
picocom --nolock -b 115200 -y e -d 7 /dev/ttyCH9140   # → 115200 7E1
```

### 快速自检

```bash
chuart version                       # 库与适配器是否可用
chuart scan -t 5                     # 能否发现模组
chuart features --mac <MAC>          # 特征是否符合预期（0xfff1 通知）
```

### 与 picocom / minicom 配合

```bash
picocom --nolock -b 115200 /dev/ttyCH9140
minicom -D /dev/ttyCH9140 -b 115200
socat - /dev/ttyCH9140,raw,echo=0
```

注意事项：

- **`chuart` 只是映射器：启动时它不碰模组的串口参数**。波特率、数据位、停止位、
  校验位、硬件流控、DTR/RTS 全部由客户端决定 —— `picocom -b 9600 /dev/ttyCH9140`
  会把模组 UART 真的改成 9600，无需重启 `serve`。在客户端设置之前，节点 `TCGETS`
  报的是占位值（115200 8N1），而模组保持它自己原有的设置。
- 因此**客户端的第一次设置一定会下发**（哪怕它与占位值相同），因为模组真实参数在
  设置之前是未知的。一次变更约 120 ms（库要写 `0xfff3` 并读回确认），所以
  `tcsetattr` 会短暂阻塞；
- 想让 `chuart` 完全不配置模组（只搬数据），用 `-I/--ignore-client-params`：节点照样
  接受 termios，但 `TCGETS` 会如实回报模组的真实参数，于是 picocom 会打印
  `!! Settings mismatch !!` —— 这正是想要的效果：它在告诉你实际生效的是什么。
- 参数只改模组侧；**对面设备的波特率必须与之一致**，否则收到的是乱码。
- 建议加 `--nolock`，避免 `/var/lock/LCK..ttyCH9140` 残留（实测不加也能工作）。
- 模组会以 `0x88` 开头的帧上报 Modem 线状态，`chuart` 会把它记进日志，同时**原样
  透传**该帧给客户端。
- CH9140lib 自身会往 stdout 打印 GBK 中文状态行（如 `BLEUART参数设置成功`），
  这是厂商库行为，脚本解析输出时请注意。

## 故障排查

| 现象 | 原因与处理 |
| --- | --- |
| `cuse: cannot set up /dev/ttyXXX - opening /dev/cuse needs root` | 用 `sudo`，或给用户 `/dev/cuse` 权限（udev 规则），并 `modprobe cuse` |
| `cuse: /dev/ttyCH9140 already exists` | 已有实例在跑（`pgrep -a chuart`），或换个 `--name` |
| picocom 显示 `!! Settings mismatch !!` | 要么用了 `-I/--ignore-client-params`（模组参数不由 `chuart` 改，节点如实回报），要么模组拒绝了某个参数（日志里有 `cannot apply`） |
| 连不上，日志有 `Device 'XX' cannot be found` | 正常不会出现：`chuart` 会先查 BlueZ 对象表，缓存里没有才扫描（所以不会白失败一次）。若仍看到，说明扫描也没发现它——用 `chuart scan` 确认模组在范围内/已上电；BlueZ 异常时可用 `bluetoothctl devices` 对照 |
| `bluetooth adapter unusable: Permission denied` | 适配器存在但未开机：`rfkill unblock bluetooth` / `bluetoothctl power on` |
| `picocom: FATAL: failed to add port: Filedes is not a tty` | 设备不是 `chuart` 创建的节点（真 tty 才会显示 tty 语义） |
| 收不到数据 / `ble: rx buffer full` 告警 | 客户端读得太慢，调大 `src/main.c` 里的 `RX_RING_BYTES`（默认 256 KiB） |
| `cuse: /dev/xxx did not appear within 5 s` | udev 没有创建节点：容器/无 udev 环境，或系统里残留了旧的 CUSE 设备（`ls /sys/class/cuse/`，逐个杀掉其属主进程即可腾出位置） |
| 退出后进程还在 | 已由“打断 FUSE 线程（SIGUSR2 强制 EINTR）”机制修复；若仍复现请开 issue |

## 仓库结构

```
CMakeLists.txt
src/
  main.c        子命令、写线程、信号监督
  ble.c/.h      CH9140lib 封装
  cuse_tty.c/.h CUSE tty 模拟（termios/ioctl/poll）
  ring.c/.h     有界环形缓冲
  log.c/.h      日志
  lib/          CH9140lib.h + libCH9140lib.so（厂商预编译）
docs/
  CH9140lib.md  厂商 API 整理 + 实测修正
```
