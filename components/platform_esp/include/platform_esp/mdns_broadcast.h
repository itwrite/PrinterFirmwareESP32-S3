// ============================================================================
// mdns_broadcast.h —— mDNS 服务广播(_ijpd._tcp)
//
// App 侧 NWBrowser 浏览 "_ijpd._tcp" 自动发现打印机(文档 §2);
// 服务实例名 = device_id(如 IJPD-71D5D0),TXT 附带设备基础信息。
// ============================================================================
#ifndef IJPD_PLATFORM_MDNS_BROADCAST_H
#define IJPD_PLATFORM_MDNS_BROADCAST_H

#include <cstdint>

#include "core/errors.h"

namespace ijpd {

// 初始化 mDNS 并广播控制通道服务(TCP 9100)。
// hostname/device_id/fw 均为 NUL 结尾字符串;重复调用安全(仅首次生效)。
Err mdns_broadcast_start(const char* hostname, const char* device_id,
                         const char* fw_version) noexcept;

}  // namespace ijpd

#endif  // IJPD_PLATFORM_MDNS_BROADCAST_H
