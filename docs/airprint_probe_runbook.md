# AirPrint Query / Print-Job 真机联调

本 runbook 验证显式构建的 AirPrint service。它支持
`Get-Printer-Attributes`、受限 `image/urf` `Print-Job`、`Get-Jobs`、
`Get-Job-Attributes` 和 `Cancel-Job` 的 logical Mock 路径。默认固件仍关闭
AirPrint；该路径不代表真实喷头点火、Ricoh 控制器时序或物理输出。

## 1. 需要准备

- ESP32-S3 板和可刷写的 USB 数据线。
- macOS 上的串口路径，例如 `/dev/cu.usbmodemXXXX`。
- 如果设备没有已保存的 STA Wi-Fi 凭据，probe 变体会启动 SoftAP；已有凭据时会优先连接 STA。
- Mac 与 ESP32-S3 位于同一个 Wi-Fi 网段。
- 记录设备 IP 或 `.local` 主机名；不要把 Wi-Fi 密码、NVS 内容或其他凭据提交到 evidence。

## 2. 构建和刷写

先只构建，确认工具链和依赖正确：

```bash
tools/idf.sh -B build-airprint-probe build -DAIRPRINT_ENABLE=ON
```

确认串口后刷写并打开 monitor：

```bash
ESPPORT=/dev/cu.usbmodemXXXX \
tools/idf.sh -B build-airprint-probe flash monitor
```

串口日志应看到：

```text
AirPrint service started on TCP 631 (print_jobs=enabled)
```

显式镜像仍保留默认 IJPD TCP/BLE/Wi-Fi 行为；不要把该镜像当作生产固件。

## 3. 配网

`AIRPRINT_ENABLE=ON` 的 probe 变体支持与默认固件相同的按需 SoftAP 配网：没有有效的
已保存 STA 凭据，或已保存凭据连接失败时，会创建 `IJPD-Setup-XXXX` 热点，并在
`192.168.4.1:80` 提供配网 HTTP 门户。已有凭据且 STA 连接成功时不会启动 SoftAP；
这是正常行为，不会同时运行 AP 和 STA。凭据不得写入源码、git、串口日志或 evidence。

设备启动后从串口日志、路由器 DHCP 列表或 mDNS 查询确认 STA 地址。正常固件的
配网协议细节见 [protocols/provisioning-http.md](protocols/provisioning-http.md)。

## 4. mDNS 和查询验证

在 Mac 上执行：

```bash
dns-sd -B _ipp._tcp local
dns-sd -L "RICOH MH5440 Mock" _ipp._tcp local
```

然后保存一个原始查询 evidence bundle：

```bash
python3 tools/airprint_query.py \
    --host ijpd-xxxx.local \
    --printer-uri ipp://ijpd-xxxx.local/ipp/print \
    --output-dir evidence/query-001 \
    --requested-attribute printer-name \
    --requested-attribute operations-supported
```

检查 `evidence/query-001/metadata.json`、`request.ipp`、`response.ipp` 和对应的
SHA-256。也可以使用设备 IP 替代 `.local` 主机名。

CUPS 标准查询可用以下命令验证：

```bash
NO_PROXY=ijpd-71d5d0.local,172.20.10.3 \
ipptool -tv ipp://ijpd-71d5d0.local:631/ipp/print \
    get-printer-attributes.test
```

当前 probe 已在 ESP32-S3 真机上通过该查询；返回的 capability response 包含
`document-format-supported` 的 `mimeMediaType`、`urf-supported` 的多值 keyword、
`media-col-default` collection 和 `printer-more-info` URI。

显式 Print-Job variant 的 `operations-supported` 还包括 `Print-Job`、
`Validate-Job`、`Cancel-Job`、`Get-Job-Attributes` 和 `Get-Jobs`。

## 5. Print-Job 和 Job 查询

使用保存的真实 golden body 发送一页受支持的 Apple Raster：

```bash
NO_PROXY=<device-ip> no_proxy=<device-ip> \
curl --noproxy <device-ip> --max-time 180 \
    -H 'Content-Type: application/ipp' \
    --data-binary @evidence/golden-samples/urf-photo-preview-707738/ipp_body.bin \
    http://<device-ip>:631/ipp/print -o /tmp/airprint-response.ipp
```

响应应为 HTTP `200` 和 IPP `successful-ok`。随后先获取 Job 列表：

```bash
ipptool -4 -tv ipp://<device-ip>:631/ipp/print get-jobs.test
```

再用 `Get-Jobs` 返回的完整 `job-uri`，或直接用 `job-id`，发送
`Get-Job-Attributes`。两种 selector 都必须得到 `job-state=completed`。不能把
打印机 URI 当作 job URI；例如 `ipp://<device-ip>:631/ipp/print` 不是
`.../job/<id>`，服务应对此返回 `client-error-not-found`。

当前真机验证结果：Job `1`/`2` 的 `job-uri` selector 和 `job-id=1` selector
均返回 HTTP `200`、IPP `successful-ok`，并包含 Job attributes；错误 URI 返回
IPP `client-error-not-found`。

彩色照片路径支持 Apple Raster 8-bit `sRGB/RGB/AdobeRGB/CMYK` 页头；服务将其
转换为 logical CMYK8 Mock 行。mDNS 只发布一个 `_ipp._tcp` 服务，不再主动发布
`_universal._sub._ipp._tcp` subtype。

## 6. 显示同步验证

AirPrint 的每个 Apple Raster page 都在开始时建立一个 `DisplayService` 任务；解码器
同步回调每得到一行 CMYK8 数据，就在对应的 `submitRaster` HAL 调用边界更新一次预览。
因此显示路径不会等待完整 HTTP body 才开始刷新。页面完成、取消、HTTP 中断、HAL 行提交
失败和 `waitUntilIdle` 失败都会结束显示任务；显示初始化不可用时，打印路径继续以 Mock
HAL 降级运行，不会因为 LCD 不可用而拒绝 Print-Job。多页文档按页建立和结束显示任务。

主机侧可先验证 decoder/Print-Job 回归和两个固件变体：

```bash
cmake --build host_tests/build
ctest --test-dir host_tests/build --output-on-failure
tools/idf.sh -B build build
tools/idf.sh -B build-airprint-probe build -DAIRPRINT_ENABLE=ON
git diff --check
```

真机验证仍需刷写最新 `build-airprint-probe` 镜像，然后发送 golden `Print-Job` 或从
iPhone 选择照片。观察 LCD 是否按 CMYK 通道逐行刷新，同时确认 Job 仍为 `completed`、
串口没有 reset/panic；该验证只能证明当前 Mock/显示路径，不能证明真实 Ricoh 喷头输出。

## 7. 保存 macOS/iOS 查询证据

如果需要记录系统客户端的真实查询顺序，在触发系统打印界面前启动抓包。先确认
实际 Wi-Fi 接口名，再替换下面的 `en0` 和设备 IP：

```bash
sudo tcpdump -i en0 -s 0 -w evidence/airprint-query.pcap \
    host <device-ip> and tcp port 631
```

可保存的辅助证据：

```bash
dns-sd -B _ipp._tcp local > evidence/dns-sd-browse.txt
```

macOS/iOS 触发查询后停止 `tcpdump`。当前 service 会返回 capability 查询，接收
受限 `image/urf` `Print-Job`，并在提交完成后保留最近 Job，供客户端继续轮询。
当前已支持受限 `Transfer-Encoding: chunked`，可再次触发 macOS/iOS 系统打印来
捕获完整 document body。服务把验证过的 Mono8/SW/RGB/CMYK Apple Raster 行转换成
logical CMYK8 Mock 行，不把文档送入真实喷头。`_universal` subtype 与 canonical
`_ipp._tcp` 是同一 service instance，不应作为两个打印机实现。

## 8. 当前不要做的事

- 不要根据 pcap 文件名、网上结构体或单个字节模式推断 URF/Apple Raster layout。
- 不要把未人工确认的页面尺寸、DPI、stride、颜色或像素 CRC 写入 manifest。
- 不要刷写默认构建目录 `build-default` 作为 AirPrint probe。
- 不要把 logical Mock 的完成状态描述成真实喷头已经打印。

## 9. 交给下一阶段的 evidence

至少保留：

- `evidence/query-001/` 完整目录。
- `airprint-query.pcap`。
- `dns-sd` browse/resolve 输出。
- probe 固件串口日志，尤其是启动、Wi-Fi、TCP 631 和 mDNS 行。
- macOS/iOS 型号、系统版本、打印界面操作步骤和触发时间。

当前已经有两组真实 `image/urf` body，已保存为
`evidence/golden-samples/urf-photo-preview-707738/` 和
`evidence/golden-samples/urf-photo-preview-722531/`。下一步是使用独立分析工具确认
 header/page descriptor/stride/颜色和像素 CRC；已验证的 Mono8/SW 子集已进入
 logical Mock bridge，其他颜色/位深和真实硬件 pixel contract 仍不得假设。
