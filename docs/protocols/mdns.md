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

## 4. AirPrint 记录

默认构建会额外发布一个 canonical `_ipp._tcp` AirPrint service，资源路径为
`ipp/print`，端口为 TCP `631`。固件不发布 `_universal._sub._ipp._tcp` subtype，
避免同一实例被部分客户端显示为重复队列。若以 `AIRPRINT_ENABLE=OFF` 构建，
则不会启动 AirPrint 服务或发布这些记录。

## 5. 非记录项

当前不会发布：

- `AIRPRINT_ENABLE=OFF` 固件中的 `_ipp._tcp`
- `_universal._sub._ipp._tcp` AirPrint subtype
- AirPrint/IPP TXT
- TCP `9101` 数据端口
- RAW TCP 端口
- MQTT Broker/topic
- SoftAP 配网 HTTP 地址

默认固件启用 IPP/AirPrint。`AIRPRINT_ENABLE=OFF` 的精简 IJPD-only 构建不包含
AirPrint mDNS wrapper，也不会发布 IPP 服务记录。
