# SoftAP HTTP 配网协议

## 1. 何时出现

Wi-Fi 服务启用且设备没有可用的已保存 STA 凭据，或已保存凭据连接失败时，
设备进入 SoftAP 配网模式。设备也支持通过 IJPD 控制命令
`start_provisioning` 主动切换到 SoftAP，因此更换路由器时不需要先清除旧凭据。

## 2. SoftAP 参数

| 项目 | 值 |
|---|---|
| SSID | `IJPD-Setup-XXXX`，`XXXX` 为 SoftAP MAC 最后两字节的大写十六进制 |
| 密码 | `12345678` |
| 信道 | `6` |
| 最大客户端数 | `1` |
| 网关 | `192.168.4.1` |
| HTTP 地址 | `http://192.168.4.1:80` |
| 监听 backlog | `2` |

该 HTTP 服务是明文 HTTP，仅适用于设备 SoftAP 内的初始配置。没有 captive
DNS；第三方 App 应直接访问固定地址。

## 3. API

### `GET /`

返回内嵌的 HTML 配网页面。

### `GET /scan`

返回 JSON 数组：

```json
[
  {"ssid":"Office-WiFi","rssi":-48,"auth":1},
  {"ssid":"Guest","rssi":-72,"auth":0}
]
```

字段：

- `ssid`：Wi-Fi SSID。
- `rssi`：信号强度，单位 dBm。
- `auth`：`1` 表示非开放网络，`0` 表示开放网络。

最多返回 `30` 个 AP。扫描在 AP 模式下可能不可用，此时服务返回一个提示
对象而不是 HTTP 错误，例如：

```json
[{"ssid":"(Scan unavailable, please enter manually)","rssi":-100,"auth":0}]
```

### `POST /connect`

请求体使用 `application/x-www-form-urlencoded` 风格：

```text
ssid=Office-WiFi&pass=p%40ssword%231
```

支持 `%XX` URL 解码和 `+` 空格转换。成功响应：

```json
{"ok":true,"ip":"rebooting"}
```

设备会把凭据写入 NVS：

```text
namespace: ijpd_wifi
key:       ssid
key:       pass
```

响应发出后约 `2` 秒重启。下次启动使用保存的凭据尝试 STA 连接，等待约
`12` 秒；失败则重新进入 SoftAP。

参数缺失或 SSID 为空时返回：

```json
{"ok":false,"msg":"..."}
```

## 4. 其他路径

除上述路径外，服务返回 HTTP `302 Found` 并重定向到 `/`。

## 5. 限制和安全

- HTTP 请求单次最多读取约 `2047` 字节。
- 当前实现没有完整的 HTTP body 累积、严格 `Content-Length` 校验或请求超时。
- SSID/password 使用固定大小缓冲，过长输入可能被截断。
- 凭据通过明文 HTTP 传输，但 SoftAP 默认使用 WPA2-PSK。
- 配网门户启动时，IJPD TCP `9100` 和 `9101` 监听器仍可能存在；第三方应先完成
  配网，再使用 STA IP 连接打印协议。
- 正常 STA 运行时，Wi-Fi 断线会自动重连，退避约为 `1..8` 秒。
- `start_provisioning` 仅在没有活动打印任务、数据传输或 RAW 任务时接受；响应发送后
  设备切换到 SoftAP，控制连接会断开，客户端应连接 `IJPD-Setup-XXXX` 后继续配网。
