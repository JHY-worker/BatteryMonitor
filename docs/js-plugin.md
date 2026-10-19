# BatteryMonitor JS 插件开发指南

本文介绍如何为 BatteryMonitor 编写 Windows HID 电量插件。仓库中的
[`plugins/razer/razer-basilisk-x-hyperspeed.js`](../plugins/razer/razer-basilisk-x-hyperspeed.js)
已经在 Razer Basilisk X HyperSpeed 2.4G 接收器上通过真机验证，本文以它为主要示例。

JS 插件适合通过 HID output/input report 或 feature report 查询电量的设备。蓝牙 GATT、
XInput、需要文件、网络或进程访问的协议不在当前插件 API 的支持范围内。

## 安装和启用

也可以在「设置 → 插件」中点击「从 GitHub 更新」，从官方
`JKWTCN/BatteryMonitor` 仓库的 `master` 分支获取 `plugins/` 下的 JS 插件。
程序会先下载并校验全部插件，再安装到 exe 同级的 `plugins/` 目录，保留子目录结构。
同路径的本地文件（包括手动修改）会被覆盖，其他本地插件不会删除。
内容相同的文件会跳过；更新完成后需要重启 BatteryMonitor 才能加载新插件。
下载失败不会改动现有插件；安装失败时会尝试恢复原文件。
更新需要网络连接和程序目录的写入权限；GitHub 限流或网络不可用时会显示失败原因。

1. 打开 `BatteryMonitor.exe` 所在目录下的 `plugins\`。目录不存在时请自行创建。
2. 把插件 `.js` 文件放入该目录或任意层级的子目录；加载器会递归扫描。
3. 确保扩展名是小写 `.js`，且文件名不以 `.` 或 `_` 开头。
4. 重启 BatteryMonitor。插件只在程序启动时发现和加载。

使用 CMake 构建时，仓库的整个 `plugins\` 目录会自动复制到 exe 所在目录。已验证的
Razer 插件会保持以下目录结构：

```text
plugins\razer\razer-basilisk-x-hyperspeed.js
    -> <exe目录>\plugins\razer\razer-basilisk-x-hyperspeed.js
```

插件总开关默认开启。插件 ID 是相对于 `plugins\` 根目录、不含 `.js` 的路径，路径分隔符
统一为 `/`。例如上述插件的 ID 是 `razer/razer-basilisk-x-hyperspeed`。因此不同厂商
子目录中可以存在同名插件而不会冲突。

在「设置 → 插件」中点击「管理插件…」，在独立窗口中使用「启用插件」总开关，
也可以分别勾选每个已安装插件。搜索框支持按插件 ID（名称）或文件路径筛选，
不区分大小写，清空搜索即可显示全部插件；窗口底部显示筛选后的数量。
总开关和所有单独插件默认启用，通过 GitHub 下载新增的插件同样默认启用。
选择会自动保存，重启程序后生效。更新已有插件会保留之前的启用或禁用选择，
关闭总开关也不会清除单个插件的选择。插件列表会在打开管理窗口和下载结束后刷新，
刷新时会保留搜索条件。

> 插件拥有向匹配 HID 设备发送数据包的权限。错误的命令可能改变设备配置或导致设备暂时
> 无响应，请只安装可信来源的插件，并先在非关键设备上验证新协议。

## 插件结构

### 最低宿主版本

新插件应在文件的**首个非空行**声明最低支持的 BatteryMonitor 版本：

```js
// @min-host-version: 1.4
```

支持 `主版本.次版本` 或 `主版本.次版本.构建号`，例如 `1.4.261019`。
比较时逐段按整数比较，省略的构建号视为 `0`，因此 `1.10` 高于 `1.9`。
使用新增宿主 API 时，请将这里设为首次提供该 API 的程序版本；这不是插件自身的版本号。

宿主在创建 JS 引擎之前读取声明：版本不足时停用插件并记录要求版本、当前版本和升级提示，
不会执行模块顶层代码、导出函数或 HID 操作。声明格式错误时也停用；没有声明的旧插件
继续按原方式加载。声明只允许版本数字，不支持 `v` 前缀、版本范围或预发布后缀。
可使用 UTF-8 BOM 和前导空行，但声明必须放在其他注释和代码之前。

此检查从支持该声明的宿主版本开始生效，无法使此前已发布且没有版本检查的程序自动跳过插件。

### 导出函数

插件是一个 UTF-8 编码的 ES module。它必须导出 `VendorId()`、`ProductId()` 和
`GetBattery()`；`Name()` 可选。

```js
// @min-host-version: 1.4
export function Name()      { return "My Battery Device"; }
export function VendorId()  { return 0x1234; }
export function ProductId() { return [0x0001, 0x0002]; }

export function GetBattery() {
    device.write([0x00, 0x04, 0x59], 65);
    const response = device.read(65, 500);
    if (!response || response[1] !== 0x04) {
        return null;
    }
    return { percentage: response[2], charging: response[3] !== 0 };
}
```

| 导出函数         | 必填 | 返回值                                             |
| ---------------- | ---- | -------------------------------------------------- |
| `VendorId()`   | 是   | 一个 USB VID，或非空 VID 数组；每项为`1..0xffff` |
| `ProductId()`  | 是   | 一个 USB PID，或非空 PID 数组；每项为`1..0xffff` |
| `GetBattery()` | 是   | 电量对象、电量对象数组，或`null`/`undefined`   |
| `Name()`       | 否   | 日志中显示的插件名称；缺省为`js:<插件 ID>`       |

宿主按 VID/PID 枚举接口、打开句柄、调用 `GetBattery()`，随后强制关闭句柄。插件不负责
设备枚举和句柄生命周期。同一设备有多个 HID 接口时，宿主会尝试候选接口并记住上次成功
的路径。

## 返回电量

`GetBattery()` 返回的单设备对象支持以下字段：

| 字段           | 类型    | 说明                                               |
| -------------- | ------- | -------------------------------------------------- |
| `percentage` | number  | `0..100` 的精确电量                              |
| `level`      | string  | `"empty"`、`"low"`、`"medium"` 或 `"full"` |
| `charging`   | boolean | 可选，默认`false`                                |
| `wired`      | boolean | 可选，默认`false`                                |
| `name`       | string  | 可选，覆盖 HID product string                      |

`percentage` 和有效的 `level` 至少提供一个。只有离散电量档位的设备可以这样返回：

```js
return { level: "medium", charging: false };
```

一个接收器管理多个子设备时，可返回对象数组。每项还可以提供最长 64 字节的字符串 `id`，
作为稳定的设备 ID 后缀：

```js
return [
    { id: "mouse", name: "Mouse", percentage: 72 },
    { id: "keyboard", name: "Keyboard", percentage: 48, charging: true },
];
```

返回 `null`、`undefined` 或空数组表示本轮没有读到设备，这属于正常路径。抛出异常则会记入
失败计数。

## `device` API

宿主只注入一个全局 `device` 对象。所有长度都包含 Report ID 字节；无编号报告通常以
`0x00` 作为第一个字节。

### Output/Input report

```js
device.write(packet[, length])
device.read([length[, timeoutMs]])
```

- `packet` 可以是普通数组或 `Uint8Array`，每项必须在 `0..255` 内。
- `write` 会把短包补零到 `length`，并返回实际写入字节数。省略长度时使用接口报告长度。
- `read` 返回 `Uint8Array`；超时或无数据返回 `null`。默认及最大超时均为 500 ms。
- 单个报告长度上限为 4096 字节。HID 错误和非法参数会抛出异常。

### Feature report

```js
device.sendFeature(packet[, length])
device.getFeature([length])
```

已验证的 Basilisk X 插件使用 90 字节 Razer payload，加一个 Report ID，因而总长为 91：

```js
const REPORT_SIZE = 90;
const REPORT_WITH_ID = 91;
const request = [0x00].concat(
    makeRequest(CLASS_BATTERY, CMD_GET_BATTERY, DATA_SIZE)
);

device.sendFeature(request, REPORT_WITH_ID);
device.pause(31);
const response = device.getFeature(REPORT_WITH_ID);
```

`sendFeature` 的补零、长度和返回值规则与 `write` 相同。`getFeature` 返回
`Uint8Array`，失败时抛出异常。设备或驱动可能返回带或不带 Report ID 的数据；已验证示例
同时兼容两种布局，并在解析前校验响应长度、状态、命令类和命令号。

### 辅助方法

| API                                         | 说明                                            |
| ------------------------------------------- | ----------------------------------------------- |
| `device.pause(ms)`                        | 同步等待，最大 1000 ms                          |
| `device.log(...values)`                   | 写入 VERBOSE 日志                               |
| `device.logWarn(...values)`               | 写入 WARNING 日志                               |
| `device.logError(...values)`              | 写入 ERROR 日志                                 |
| `device.productId()`                      | 当前接口的 PID                                  |
| `device.usagePage()` / `device.usage()` | 当前顶层 HID usage 信息                         |
| `device.name()`                           | HID product string                              |
| `device.path()`                           | 当前 HID 接口路径，适合诊断，不应作为跨机器标识 |

## 已验证示例解析

Basilisk X 插件展示了 feature report 协议的推荐结构：

1. `VendorId()` 返回 Razer VID `0x1532`，`ProductId()` 只匹配 `0x0083`。
2. `makeRequest()` 构造固定长度 payload，并按协议计算 CRC。
3. `send()` 负责发送、等待、读取、兼容 Report ID 布局并校验响应，最多重试 5 次。
4. `GetBattery()` 只负责调用协议函数并把原始 `0..255` 电量转换成 `0..100`。

这种分层能把传输、校验和业务解析分开。新增插件时建议先完整校验帧头、长度、命令回显和
校验和，再读取电量字段；不要仅凭一个字节位置就接受响应。

## 调试和故障处理

日志位于 `%APPDATA%\BatteryMonitor\BatteryMonitor.log`。搜索 `[js:<插件 ID>]`、
`discovered plugin`、`loaded` 或 `GetBattery threw`。

常见问题：

- 没有发现插件：确认插件位于 exe 同级 `plugins\` 目录或其子目录、扩展名为小写
  `.js`，然后重启程序。
- 插件加载失败：检查三个必填导出、VID/PID 范围和 JavaScript 语法。
- 能打开设备但没有电量：记录 `usagePage()`、`usage()` 和 `path()`，检查 Report ID、报告
  总长及响应偏移。
- feature 调用失败：同一物理设备可能暴露多个接口；在 `GetBattery()` 内捕获预期的接口
  错误并返回 `null`，让宿主继续尝试其他接口。
- 出现重复设备：不要同时用原生 provider 和 JS 插件支持同一个 VID/PID。

单轮执行总时限为 10 秒，单轮最多返回 32 台设备。插件内存上限为 16 MB，栈上限为
256 KB。若插件抛异常且整轮没有读到任何设备，连续 5 轮后会在当前进程中自动停用；重启
程序后会重新尝试加载。

## 沙箱限制

插件没有文件系统、网络和进程执行能力，不提供 `std`、`os`、`fetch` 或模块加载器。
`Date` 被移除，`Math.random()` 固定返回 `0`。`GetBattery()` 必须是同步函数；轮询周期、
设备枚举、重试时机和句柄关闭均由宿主管理。
