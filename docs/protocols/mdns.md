# mDNS / DNS-SD 发现协议

## 1. 启用条件

当 Wi-Fi STA 获得 IP 且 `services.ijpd_tcp=true` 时，固件自动初始化 mDNS。
默认固件满足该条件。SoftAP 配网阶段尚未获得 STA IP 时不会广播该服务。

## 2. 服务记录

| 记录 | 当前值 |
|---|---|
| Service type | `_ijpd._tcp` |
| Port | `9100` |
| Instance name | 设备 ID，例如 `IJPD-71D5D0` |
| Hostname | `ijpd-` + MAC 后三字节小写，例如 `ijpd-71d5d0.local` |

TXT：

```text
device_id=IJPD-71D5D0
fw=0.3.0
```

## 3. 客户端流程

1. 浏览 `_ijpd._tcp`。
2. 读取 SRV 的 target 和 port。
3. 解析 target 的 A/AAAA 地址。
4. 连接 SRV 端口，默认是 TCP `9100`。
5. 发送 [IJPD 控制协议](ijpd-control.md) 的 `get_device_info`。

不要假设数据端口会被 mDNS 单独广播。当前 mDNS 只发布控制端口；数据端口
从 `start_print` 响应的 `data_port` 读取。

## 4. AirPrint opt-in 记录

显式 `AIRPRINT_ENABLE=ON` 构建会额外发布一个 `_ipp._tcp` AirPrint service，
并在同一 service instance 上附加 `_universal._sub._ipp._tcp` subtype。subtype
不是第二个 printer/service，目的是满足 iOS 16+ 的 AirPrint discovery。

## 5. 非记录项

当前不会发布：

- 默认固件中的 `_ipp._tcp`
- AirPrint/IPP TXT
- TCP `9101` 数据端口
- RAW TCP 端口
- MQTT Broker/topic
- SoftAP 配网 HTTP 地址

默认固件不实现 IPP/AirPrint。仓库中的 opt-in AirPrint mDNS wrapper 只在显式
`AIRPRINT_ENABLE=ON` 构建中存在，当前不会由默认 app 启动。
