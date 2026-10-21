# CLion 烧录环境使用指南

本项目已在 CLion 2026.2 + ESP-IDF v5.4.1 (`~/esp/esp-idf`) 下配置完成，构建目录复用 `build/`（与命令行 `idf.py` 完全一致）。

## 已完成的配置

| 项目 | 内容 |
|---|---|
| CMake Profile `ESP-IDF` | `.idea/workspace.xml` 中注入完整 IDF 环境（IDF_PATH / 工具链 / Python venv / OpenOCD），生成目录 `build/` |
| idf.py 包装脚本 | `tools/idf.sh`（自动 source export.sh 后调用 idf.py） |
| 运行配置 `.run/` | Flash and Monitor / Flash / Monitor / Menuconfig / Erase Flash / ESP32-S3 Debug (OpenOCD) |

## 日常使用

1. **编译**：CLion 右上角选择 profile `ESP-IDF`，点 Build（或 ⌘F9）。
2. **烧录 + 看日志**：运行配置下拉框选 `Flash and Monitor`，点 Run（⌃R）。监视器退出按 `Ctrl + ]`。
3. **改配置**：运行 `Menuconfig`。
4. **指定串口**（默认自动检测；如有多个串口设备才需要）：
   ```zsh
   ESPPORT=/dev/cu.usbmodemXXXX tools/idf.sh flash
   ```
5. **命令行同样可用**：`tools/idf.sh build` / `flash` / `monitor` / `erase-flash`。

## 在线调试（JTAG，可选）

运行配置 `ESP32-S3 Debug (OpenOCD)` 已指向 espressif 版 OpenOCD 和板卡脚本
`board/esp32s3-builtin.cfg`（使用 ESP32-S3 内置 USB-JTAG，只需一根 USB 线）。

首次调试前需在 CLion 中设置一次 GDB 路径：

```
Settings → Build, Execution, Deployment → Toolchains → Default → Debugger
→ 设为 /Users/zzpzero/.espressif/tools/xtensa-esp-elf-gdb/14.2_20240403/xtensa-esp-elf-gdb/bin/xtensa-esp32s3-elf-gdb
```

然后 Debug 运行该配置即可（会自动编译→烧录→断点调试）。

- 若使用外部 JTAG 调试器（如 FTDI），把运行配置里的脚本改为 `board/esp32s3-ftdi.cfg`。
- 调试依赖 `ESP_ROM_ELF_DIR`（已在 Profile 环境中设置，可查看 ROM 内符号）。

## 串口监视

也可用 CLion 自带 Serial Monitor 插件（底部工具窗口），端口选 `/dev/cu.usbmodem*`，波特率 115200。

## 故障排查

- **CMake 报找不到 IDF/工具链**：确认 `~/esp/esp-idf` 与 `~/.espressif/tools/` 存在；Settings → Build → CMake 中确认 `ESP-IDF` profile 已启用且环境未丢失。
- **烧录找不到端口**：插好板子后 `ls /dev/cu.usbmodem* /dev/cu.usbserial*`，用 `ESPPORT=... tools/idf.sh flash`。
- **进不了 USB-JTAG 调试**：确认板子是 ESP32-S3 且用数据线连接（非充电线）；sdkconfig 中 `CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG` 未被关闭。
