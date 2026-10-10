# CH9140lib BLE 串口库 API 参考

> 适用对象：CH914X 系列低功耗蓝牙转串口芯片模组（Linux 平台）。
>
> 本文档由厂商《CH9140lib 库接口函数说明》整理而成，并与本仓库的实际产物核对：
>
> | 核对项 | 位置 |
> | --- | --- |
> | 公开头文件 | `src/lib/CH9140lib.h` |
> | 预编译动态库 | `src/lib/libCH9140lib.so` |
> | 实际导出符号 | `nm -D --defined-only src/lib/libCH9140lib.so` |
>
> 文档与头文件/库不一致之处集中在 [§7 差异与补充](#7-文档与头文件--库的差异)。下文凡标注 **【头文件】** 的原型均以 `src/lib/CH9140lib.h` 为准。

---

## 1. 概述

CH9140lib 是 Linux 系统下的 BLE 串口接口库，提供以下能力：

- 查询系统蓝牙控制器（适配器）版本与开关状态；
- 扫描附近的 CH914X 设备（地址、名称、RSSI、芯片版本）；
- 通过 MAC 地址建立/断开 BLE 连接，并接收连接与断开事件回调；
- GATT 服务/特征发现、按句柄读特征、按 UUID 写特征、订阅/退订通知（notify）；
- 设置模组侧串口参数（波特率、数据位、停止位、校验位）与流控 / MODEM 信号；
- 收发串口数据（发送接口按 MTU 分片，单次上限 4096 字节）；
- 解析模组上报的 MODEM 状态（DCD / RI / DSR / CTS）。

### 1.1 库产物属性

| 属性 | 值 |
| --- | --- |
| 文件 | `src/lib/libCH9140lib.so` |
| 格式 | ELF 64-bit LSB shared object, x86-64 |
| SONAME | `libCH9140lib.so` |
| 构建工具链 | GCC 5.4.0 (Ubuntu 16.04)，含调试符号（未 strip） |
| 运行期依赖 | `libglib-2.0.so.0`、`libgio-2.0.so.0`、`libgobject-2.0.so.0`、`libbluetooth.so.3`、`libc.so.6` |
| 后端实现 | BlueZ 5 D-Bus API（`org.bluez` 的 Adapter1 / Device1 / GattService1 / GattCharacteristic1） |
| 线程模型 | 库内部通过 `pthread_create` 启动线程并运行 GLib 主循环（`g_main_loop_run`），**所有回调都在该线程上触发** |

### 1.2 运行前置条件

- 系统已安装并运行 BlueZ（`bluez` 5.x）与 D-Bus 系统总线；
- 本机蓝牙适配器已打开（未打开时扫描/查询会直接返回错误码）；
- 目标模组已上电、未被其他主机占用（BLE 串口通常为单连接）；
- 程序链接该 `.so`，并能解析到 `libbluetooth.so.3` 等依赖（见 [§8](#8-链接与运行环境要求)）。

---

## 2. 头文件与类型定义

```c
#include <stdint.h>
#include <stdbool.h>

#include <bluetooth/bluetooth.h>   /* uuid_t */
#include <bluetooth/sdp.h>
#include <bluetooth/sdp_lib.h>

typedef struct _gatt_connection_t CH9140HANDLE;   /* 不透明句柄 */
```

`CH9140HANDLE` 为不透明的连接句柄（内部是 GATT 连接结构 `_gatt_connection_t`），由 `CH9140Ble_Connect()` 返回，后续所有针对该连接的操作都必须传入同一句柄。

### 2.1 `gatt_primary_service`（服务）

```c
typedef struct {
    uint16_t attr_handle_start;   /* 服务属性句柄起始 */
    uint16_t attr_handle_end;     /* 服务属性句柄结束 */
    uuid_t   uuid;                /* 服务 UUID */
} gatt_primary_service;
```

### 2.2 `gatt_characteristic`（特征）

```c
typedef struct {
    uint16_t handle;        /* 特征句柄 */
    uint8_t  properties;    /* 特征属性位图 */
    uuid_t   uuid;          /* 特征 UUID */
} gatt_characteristic;
```

`properties` 位定义：

| 位 | 掩码 | 含义 |
| --- | --- | --- |
| bit0 | `0x01` | 1 = 支持广播（broadcast） |
| bit1 | `0x02` | 1 = 支持读（read） |
| bit2 | `0x04` | 1 = 支持无应答写（write without response） |
| bit3 | `0x08` | 1 = 支持有应答写（write with response） |
| bit4 | `0x10` | 1 = 支持通知（notification） |

---

## 3. 接口函数

### 3.1 查询蓝牙控制器版本

```c
int CH9140BleGetBluetoothVer();
```

查询本机蓝牙控制器（适配器）支持的蓝牙版本。

| 返回值 | 版本号 |
| --- | --- |
| 0 | V1.0 |
| 1 | V1.1 |
| 2 | V1.2 |
| 3 | V2.0 |
| 4 | V2.1 |
| 5 | V3.0 |
| 6 | V4.0 |
| 7 | V4.1 |
| 8 | V4.2 |
| 9 | V5.0 |
| 10 | V5.1 |
| 11 | V5.2 |
| -1 | 设备查询超时 |
| -2 | 系统蓝牙未打开 |

### 3.2 扫描设备

```c
int CH9140Ble_BLE_Scan(int ScanTime, FunDiscoverDeviceInfo Ble_AdvertisingDevice_Info);
```

在规定时间内扫描附近的 CH914X 设备，每发现一个设备就通过回调返回其信息。

| 参数 | 说明 |
| --- | --- |
| `ScanTime` | 扫描时长，单位：秒 |
| `Ble_AdvertisingDevice_Info` | 扫描结果回调，见 [§4.1](#41-设备扫描回调) |

| 返回值 | 含义 |
| --- | --- |
| 0 | 扫描成功 |
| 1 | 系统蓝牙未打开 |

回调返回的信息包括：设备地址、设备名称、信号强度 RSSI、芯片版本号。

### 3.3 连接设备

```c
CH9140HANDLE *CH9140Ble_Connect(const char *mac, FunConnectionStateCallback connectionstate);
```

通过 MAC 地址连接设备。

| 参数 | 说明 |
| --- | --- |
| `mac` | 设备地址，可由扫描接口获得 |
| `connectionstate` | 连接状态回调，见 [§4.2](#42-连接状态回调) |

| 返回值 | 含义 |
| --- | --- |
| `CH9140HANDLE *` | 连接句柄，后续函数调用时作为参数传递 |

> 注意：返回句柄非 `NULL` 不等于连接已建立。是否真正连上以 `connectionstate` 回调的 `state` 为准（1 = 连接成功，0 = 连接失败），并且必须在打开通知 / 收发数据**之前**等待连接成功。

### 3.4 断开连接

```c
void CH9140Ble_Disconnect(CH9140HANDLE *connection);
```

断开已连接的设备。参数 `connection` 为设备连接句柄。

### 3.5 注册连接断开回调

```c
void CH9140Ble_register_on_disconnect(CH9140HANDLE *connection,
                                      FunDisconnectionStateCallback disconnection_state);
```

注册设备连接断开事件。

| 参数 | 说明 |
| --- | --- |
| `connection` | 设备连接句柄 |
| `disconnection_state` | 连接断开状态回调，见 [§4.3](#43-断开连接状态回调) |

### 3.6 获取设备服务

```c
int CH9140Ble_discover_primary(CH9140HANDLE *connection,
                               gatt_primary_service *services,
                               int *services_count);
```

获取已连接设备的全部 GATT 服务。

| 参数 | 说明 |
| --- | --- |
| `connection` | 设备连接句柄 |
| `services` | 输出：设备服务数组（调用方预分配，容量由入参 `*services_count` 决定） |
| `services_count` | 入参为数组容量，出参为实际服务个数 |

| 返回值 | 含义 |
| --- | --- |
| 0 | 获取服务成功 |
| -1 | 获取服务失败 |

### 3.7 获取设备特征

```c
int CH9140Ble_discover_characteristics(CH9140HANDLE *connection,
                                       const char *ServiceHandle,
                                       gatt_characteristic *characteristics,
                                       int *characteristics_count);
```

获取连接设备的特征列表。**【头文件】** 该函数比厂商文档多一个入参 `ServiceHandle`，用于指定要枚举的服务句柄（即 `gatt_primary_service.attr_handle_start`）。

| 参数 | 说明 |
| --- | --- |
| `connection` | 设备连接句柄 |
| `ServiceHandle` | 服务句柄字符串（来自服务发现结果） |
| `characteristics` | 输出：特征结构体数组 |
| `characteristics_count` | 入参为数组容量，出参为实际特征个数 |

| 返回值 | 含义 |
| --- | --- |
| 0 | 获取特征成功 |
| -1 | 获取特征失败 |

`properties` 字段含义见 [§2.2](#22-gatt_characteristic特征)。

### 3.8 读取特征值

```c
int CH9140Ble_Read_Char_by_Handle(CH9140HANDLE *connection, uint16_t handle,
                                  char *buffer, size_t *buffer_length);
```

按特征句柄读取特征值。

| 参数 | 说明 |
| --- | --- |
| `connection` | 设备连接句柄 |
| `handle` | 特征句柄 |
| `buffer` | 输出：存放读取结果 |
| `buffer_length` | 期望读取长度（`size_t *`，**【头文件】** 为指针，实际读取长度由库回填） |

| 返回值 | 含义 |
| --- | --- |
| 0 | 读取特征成功 |
| 其他 | 读取特征失败 |

### 3.9 写入特征值

```c
int CH9140Ble_Write_Characteristic(CH9140HANDLE *connection,
                                   const char *CharacteristicUUID,
                                   bool WriteWithResponse,
                                   const void *buffer, size_t buffer_length);
```

按 UUID 写入特征值（该特征必须可写）。

| 参数 | 说明 |
| --- | --- |
| `connection` | 设备连接句柄 |
| `CharacteristicUUID` | 目标特征 UUID 字符串（特性值需可写） |
| `WriteWithResponse` | 传输模式：0 = 无应答传输，1 = 有应答传输 |
| `buffer` | 待写入数据 |
| `buffer_length` | 待写入数据长度 |

| 返回值 | 含义 |
| --- | --- |
| 0 | 成功 |
| 1 | 写入失败 |

### 3.10 注册通知回调

```c
void CH9140Ble_register_notification(CH9140HANDLE *connection,
                                     FunRegisterNotifyCallback notification_handler);
```

注册通知处理函数，之后通过通知回调接收模组上报（串口下行）的数据。

| 参数 | 说明 |
| --- | --- |
| `connection` | 设备连接句柄 |
| `notification_handler` | 通知回调函数，见 [§4.4](#44-通知回调) |

### 3.11 打开通知

```c
int CH9140Ble_Open_Notification(CH9140HANDLE *connection, const char *CharacteristicUUID);
```

打开（订阅）指定特征的 notify。

| 参数 | 说明 |
| --- | --- |
| `connection` | 设备连接句柄 |
| `CharacteristicUUID` | 特征标识（UUID 字符串） |

| 返回值 | 含义 |
| --- | --- |
| 0 | 打开通知成功 |
| -1 | 打开通知失败 |

### 3.12 关闭通知

```c
int CH9140Ble_Close_Notification(CH9140HANDLE *connection, const char *CharacteristicUUID);
```

关闭（退订）指定特征的 notify。

| 返回值 | 含义 |
| --- | --- |
| 0 | 关闭通知成功 |
| -1 | 关闭通知失败 |

### 3.13 获取设备 MTU

```c
int CH9140Ble_Get_MTU(CH9140HANDLE *connection, uint16_t *mtu);
```

获取当前连接协商出的 MTU 值（与 [§3.14](#314-发送串口数据) 搭配，用于分片发送）。

| 参数 | 说明 |
| --- | --- |
| `connection` | 设备连接句柄 |
| `mtu` | 输出：MTU 值 |

| 返回值 | 含义 |
| --- | --- |
| 0 | 获取成功 |
| 其他 | 失败 |

### 3.14 发送串口数据

```c
int CH9140BleUart_Write_Buffer(CH9140HANDLE *connection, char *buffer,
                               int buffer_length, uint16_t mtu);
```

把数据写入模组串口发送方向（即主机 → 模组 → 串口 TX）。

| 参数 | 说明 |
| --- | --- |
| `connection` | 设备连接句柄 |
| `buffer` | 待写入数据 |
| `buffer_length` | 待写入数据长度 |
| `mtu` | 当前连接的 MTU（来自 [§3.13](#313-获取设备-mtu)，用于分片） |

| 返回值 | 含义 |
| --- | --- |
| > 0 | 实际发送的数据字节数 |
| -1 | `buffer` 长度超过 4096 |

### 3.15 设置串口参数

```c
int CH9140Ble_Set_SerialBaud(CH9140HANDLE *connection, int BaudRate, int DataBit,
                             int StopBit, int Parity);
```

设置模组侧串口波特率、数据位、停止位和校验位。

| 参数 | 取值 |
| --- | --- |
| `BaudRate` | 常规波特率，如 9600、115200 等 |
| `DataBit` | 数据位，5–8 |
| `StopBit` | 停止位，1–2 |
| `Parity` | 0 = 无校验，1 = 奇校验，2 = 偶校验，3 = 标志位（Mark），4 = 空白位（Space） |

| 返回值 | 含义 |
| --- | --- |
| 0 | 设置成功 |
| -1 | 设置失败 |

### 3.16 设置串口流控与 MODEM 状态

```c
int CH9140Ble_Set_Modem(CH9140HANDLE *connection, int flow, int DTR, int RTS);
```

| 参数 | 说明 |
| --- | --- |
| `flow` | 硬件自动流控开关：1 = 开启，0 = 关闭 |
| `DTR` | 1 = 有效（低电平），0 = 无效（高电平） |
| `RTS` | 1 = 有效（低电平），0 = 无效（高电平） |

| 返回值 | 含义 |
| --- | --- |
| 0 | 成功 |
| -1 | 失败 |

### 3.17 显示串口 MODEM 状态

```c
void CH9140Ble_Get_Modem_state(const uint8_t *data, FunGetModemStateCallback getmodemstate);
```

解析模组上报的 MODEM 状态数据。

| 参数 | 说明 |
| --- | --- |
| `data` | 接收到的通知数据（通常来自 [§4.4](#44-通知回调)），用于解析 MODEM 状态 |
| `getmodemstate` | 获取 MODEM 状态的回调函数，见 [§4.5](#45-modem-状态回调) |

---

## 4. 回调函数

### 4.1 设备扫描回调

```c
typedef void (*FunDiscoverDeviceInfo)(const char *addr, const char *name,
                                     int8_t rssi, uint8_t *ChipVer);
```

| 参数 | 说明 |
| --- | --- |
| `addr` | 扫描到的设备地址 |
| `name` | 扫描到的设备名称 |
| `rssi` | 信号强度 |
| `ChipVer` | 芯片版本号（指针） |

### 4.2 连接状态回调

```c
typedef void (*FunConnectionStateCallback)(CH9140HANDLE *connection, int state);
```

连接状态变化时上报。

| 参数 | 说明 |
| --- | --- |
| `connection` | 设备连接句柄 |
| `state` | 1 = 连接成功，0 = 连接失败 |

### 4.3 断开连接状态回调

```c
/* 【头文件】 */
typedef void (*FunDisconnectionStateCallback)(void *user_data);

/* 厂商文档写法 */
typedef void (*FunDisconnectionStateCallback)(bool state);
```

上报设备连接断开事件。**【头文件】** 形参为 `void *user_data`（透传注册时可由库携带的用户数据指针），厂商文档写作 `bool state`（断连状态）。

### 4.4 通知回调

```c
typedef void (*FunRegisterNotifyCallback)(const uuid_t *uuid, const uint8_t *data,
                                          size_t data_length);
```

接收串口发来的数据。

| 参数 | 说明 |
| --- | --- |
| `uuid` | 特征标识 |
| `data` | 接收到的数据 |
| `data_length` | 数据长度 |

### 4.5 MODEM 状态回调

```c
typedef void (*FunGetModemStateCallback)(bool DCD, bool RI, bool DSR, bool CTS);
```

| 参数 | 说明 |
| --- | --- |
| `DCD` | 串口 DCD 输入状态 |
| `RI` | 串口 RI 输入状态 |
| `DSR` | 串口 DSR 输入状态 |
| `CTS` | 串口 CTS 输入状态 |

---

## 5. 典型调用顺序

### 5.1 顺序一（含服务/特征发现与按句柄读）

1. 扫描设备 `CH9140Ble_BLE_Scan`
2. 连接设备 `CH9140Ble_Connect`
3. 注册断开连接回调 `CH9140Ble_register_on_disconnect`
4. 获取设备 MTU 值 `CH9140Ble_Get_MTU`
5. 获取设备服务 `CH9140Ble_discover_primary`
6. 获取设备特征 `CH9140Ble_discover_characteristics`
7. 打开通知 `CH9140Ble_Open_Notification`
8. 设置波特率 `CH9140Ble_Set_SerialBaud`
9. 设置流控 `CH9140Ble_Set_Modem`
10. 读取特征值 `CH9140Ble_Read_Char_by_Handle`
11. 写入特征值 `CH9140Ble_Write_Characteristic`
12. 关闭通知 `CH9140Ble_Close_Notification`
13. 断开 BLE 设备连接 `CH9140Ble_Disconnect`

### 5.2 顺序二（纯串口透传，最小路径）

1. 扫描设备 `CH9140Ble_BLE_Scan`
2. 连接设备 `CH9140Ble_Connect`
3. 注册断开连接回调 `CH9140Ble_register_on_disconnect`
4. 获取设备 MTU 值 `CH9140Ble_Get_MTU`
5. 打开通知 `CH9140Ble_Open_Notification`
6. 设置波特率 `CH9140Ble_Set_SerialBaud`
7. 设置流控 `CH9140Ble_Set_Modem`
8. 发送数据 `CH9140BleUart_Write_Buffer`
9. 断开 BLE 设备连接 `CH9140Ble_Disconnect`

> 建立「可供 picocom 使用的文件设备」时，应在第 5～7 步之后、对上层暴露设备节点之前完成初始化，并在整个透传期间保持通知处于打开状态。

---

## 6. 返回值 / 错误码速查

| 函数 | 成功 | 失败 |
| --- | --- | --- |
| `CH9140BleGetBluetoothVer` | 0–11（版本号） | -1 超时，-2 蓝牙未打开 |
| `CH9140Ble_BLE_Scan` | 0 | 1 蓝牙未打开 |
| `CH9140Ble_Connect` | 非空句柄 | 参见连接回调 `state = 0` |
| `CH9140Ble_discover_primary` | 0 | -1 |
| `CH9140Ble_discover_characteristics` | 0 | -1 |
| `CH9140Ble_Read_Char_by_Handle` | 0 | 其他 |
| `CH9140Ble_Write_Characteristic` | 0 | 1 |
| `CH9140Ble_Open_Notification` / `_Close_Notification` | 0 | -1 |
| `CH9140Ble_Get_MTU` | 0 | 其他 |
| `CH9140BleUart_Write_Buffer` | > 0（实际字节数） | -1（长度 > 4096） |
| `CH9140Ble_Set_SerialBaud` | 0 | -1 |
| `CH9140Ble_Set_Modem` | 0 | -1 |

---

## 7. 文档与头文件 / 库的差异

### 7.1 签名差异（以头文件为准）

| 接口 | 厂商文档 | `src/lib/CH9140lib.h` |
| --- | --- | --- |
| `CH9140Ble_discover_characteristics` | `(connection, characteristics, count)` | 多出 `const char *ServiceHandle` 入参 |
| `CH9140Ble_Read_Char_by_Handle` | `size_t *length`（描述为“期望长度”） | `size_t *buffer_length`（指针，兼作实际长度回填） |
| `CH9140Ble_Write_Characteristic` | `const char *buffer` | `const void *buffer`（可传任意二进制） |
| `FunDisconnectionStateCallback` | `void (*)(bool state)` | `void (*)(void *user_data)` |

### 7.2 库中额外导出的辅助函数（头文件已声明，厂商文档未提及）

| 函数 | 用途 |
| --- | --- |
| `int gatt_uuid_to_string(const uuid_t *uuid, char *str, size_t size)` | 将 `uuid_t` 转成字符串，便于打印/匹配特征 |
| `int gatt_string_to_uuid(const char *str, size_t size, uuid_t *uuid)` | 字符串解析为 `uuid_t` |
| `int gatt_uuid_cmp(const uuid_t *uuid1, const uuid_t *uuid2)` | 比较两个 UUID |

另有一批内部实现符号（非头文件声明、供库内 D-Bus 后端使用），如 `gattlib_adapter_open`、`gattlib_adapter_scan_enable*`、`get_characteristic_from_uuid`、`disconnect_all_notifications`、`CH9140Ble_Read_Char_by_UUID`（static）、`CH9140_Write_Char_by_UUID`（static）等，不应直接在业务代码中调用。

### 7.3 UUID 的实际情况（已用 `nm`/反汇编核实）

库**内置了两个固定 UUID**（符号 `uuid_fff2`、`uuid_fff3`，位于 `.data`），并非由调用方指定：

| 常量 | 值 | 使用者 | 作用 |
| --- | --- | --- | --- |
| `uuid_fff2` | `"0xfff2"` | `CH9140Ble_Get_MTU`、`CH9140BleUart_Write_Buffer` | 数据特征（写 / 透传发送） |
| `uuid_fff3` | `"0xfff3"` | `CH9140Ble_Set_SerialBaud`、`CH9140Ble_Set_Modem` | 配置特征（串口参数、流控/Modem） |

而**通知（notify）特征 UUID 是运行期变量**：`.bss` 中的全局 `notify_uuid` 由 `CH9140Ble_Open_Notification(connection, uuid)` 写入，通知回调据此过滤数据。因此调用方**必须**传入 notify 特征 UUID。

CH914X 的厂商数据服务为 `0xfff0`，其中三个特征的实测属性（CH9141BLE2U，MTU 200）：

| 特征 | 属性位 | 含义 | 用途 |
| --- | --- | --- | --- |
| `0xfff1` | `0x12` | read + notify | **下行数据（通知）**，应作为 `CH9140Ble_Open_Notification` 的参数 |
| `0xfff2` | `0x0c` | write without response + write | 上行数据（库内部固定使用） |
| `0xfff3` | `0x1a` | read + write + notify | 配置（库内部固定使用） |

所以上层程序应把 notify UUID 默认取 `0xfff1`，并允许命令行覆盖；可用 `chuart features --mac <MAC>` 实测确认。

### 7.4 实测行为与必踩的坑

以下结论来自对本库的实际调用（CH9141BLE2U + BlueZ 5.72）与反汇编核对：

1. **传入的数组必须先清零。** `CH9140Ble_discover_primary()` / `CH9140Ble_discover_characteristics()` 只会填写它成功解析的条目，未解析的条目保持调用方内存原样。若数组未初始化，会读到栈上的随机值（实测属性位在两次运行间完全不一致）；`memset` 后结果稳定且正确。
2. **`ServiceHandle` 是“服务句柄的 4 位十六进制文本”**，例如 `"001f"`。库内部按 `<设备路径>/service<ServiceHandle>` 拼接 D-Bus 路径；而 `discover_primary` 是取路径最后 4 个字符按 `%x` 解析成 `attr_handle_start`。传别的格式会静默拿不到特征。
3. **连接前设备必须已被 BlueZ 发现。** 若设备已从 BlueZ 缓存中过期，`CH9140Ble_Connect()` 会立即失败并在 stderr 打印 `Device 'XX:..' cannot be found`。厂商的调用顺序把「扫描」放在第一步正是这个原因：连接失败后应重新扫描再连。
4. **`ChipVer` 是“裸指针”**，长度未文档化（`device_manager_on_device1_signal` 中指向一个 1 KB 的栈缓冲区，只填了前若干字节）。实测取第 1 字节得到 `0x43`。只读 1 字节是安全做法。
5. **库会直接往 stdout 写中文状态行**（GBK 编码，如 `BLEUART参数设置成功`、`BLEUARTModem设置成功`），以及往 stderr 写错误（`Failed to connect to DBus Bluez Device`、`Device connected` 等）。上层程序的 stdout 因此不干净，脚本解析需注意。
6. **`CH9140Ble_Get_Modem_state()` 自带帧头校验**：先判断 `data[0] == 0x88` 才解析（见反汇编 `11a79: movzbl (%rax),%eax; cmp $0x88`）。因此对每个通知数据都调用它是安全的，普通串口数据不会触发 Modem 回调（但仍建议先判断 `data_length >= 1`）。
7. **断开回调收到的是 `void *user_data`**，不是文档写的 `bool state`；`gattlib_call_disconnection_handler` 把连接结构偏移 `0x10` 的字段作为参数传入，本库 API 没有任何途径设置它，实测为 `NULL`。
8. **版本号表已过时**：`CH9140BleGetBluetoothVer()` 只定义到 11（V5.2），而实测适配器返回 12（HCI 5.3）。上层应把 >11 视为“未知但可用”，不要当错误。
9. **连接状态回调的时机**：`CH9140Ble_Connect()` 返回句柄不等于已连接，必须以回调 `state == 1` 为准；设备不存在时回调会以 `state == 0` 立即上报（不是超时）。

---

## 8. 链接与运行环境要求

### 8.1 编译期

- 需要 BlueZ 开发头文件（`bluetooth/bluetooth.h`、`bluetooth/sdp.h`、`bluetooth/sdp_lib.h`）。当前开发机 **未安装** `libbluetooth-dev`，编译前需安装：

  ```bash
  sudo apt install libbluetooth-dev libglib2.0-dev
  ```

- 链接：

  ```bash
  cc main.c -I src/lib -L src/lib -lCH9140lib -lglib-2.0 -lgio-2.0 -lgobject-2.0 -lbluetooth -lpthread -o chuart
  ```

- 运行前若未把 `.so` 安装到系统路径，需设置：

  ```bash
  export LD_LIBRARY_PATH=$PWD/src/lib:$LD_LIBRARY_PATH
  ```

### 8.2 运行期

- `bluez`（5.x）+ `dbus` 系统总线必须运行，且适配器已开机（`bluetoothctl power on` / `rfkill unblock bluetooth`）；
- 库内部使用 GLib 主循环线程分发回调，因此**回调不在调用线程执行**：上层在有界队列、文件描述符写出等共享状态上必须做线程同步；
- 断开事件（`register_on_disconnect`）可能在任何时刻到达，需要能安全地终止外层透传循环并清理设备节点。

---

## 9. 面向「BLE 串口 → 文件设备」封装的要点

本节为后续 CLI 程序（扫描/连接/把 BLE 串口暴露为 picocom 可用的设备节点）的约束清单，均由上文的接口语义推导而来：

1. **数据下行**：`CH9140Ble_register_notification` 注册一次，`CH9140Ble_Open_Notification(uuid)` 打开订阅；回调中的数据需原样写入设备节点的读端。
2. **数据上行**：设备节点被写入的数据 → `CH9140BleUart_Write_Buffer(..., mtu)`；单次上限 4096 字节，且应按 `CH9140Ble_Get_MTU` 的协商值分片。
3. **串口参数**：`CH9140Ble_Set_SerialBaud` / `CH9140Ble_Set_Modem` 只影响模组侧串口，本地设备节点侧的 `termios`（波特率等）通常无意义，应在文档中明确告知使用者（例如约定 `picocom -b <baud> <dev>` 中的波特率必须与模组侧一致/或由 `--baud` 参数写入模组）。
4. **MODEM 线**：`CH9140Ble_Set_Modem` 的 DTR/RTS 可由宿主程序映射；`CH9140Ble_Get_Modem_state` + `FunGetModemStateCallback` 提供 DCD/RI/DSR/CTS 反馈，可用于上报串口状态。
5. **连接状态机**：必须等待 `FunConnectionStateCallback` 的 `state == 1` 后才打开通知与创建设备节点；`state == 0` 或断开回调触发时要撤销设备节点并退出。
6. **UUID 来源**：见 [§7.3](#73-uuid-的实际情况已用-nm反汇编核实)，notify UUID 默认 `0xfff1`，数据/配置 UUID 由库内部固定。
