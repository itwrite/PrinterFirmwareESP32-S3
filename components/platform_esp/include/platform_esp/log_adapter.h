// ============================================================================
// log_adapter.h —— 日志适配（Phase 5）
// 数据热路径禁止逐包打印；高频事件用计数器 + 周期汇总（见任务书第十章）。
// 不得记录敏感凭据（Wi-Fi 密码、令牌）。
// ============================================================================
#ifndef IJPD_PLATFORM_LOG_ADAPTER_H
#define IJPD_PLATFORM_LOG_ADAPTER_H

#include "esp_log.h"

#define IJPD_LOGE(tag, fmt, ...) ESP_LOGE(tag, fmt, ##__VA_ARGS__)
#define IJPD_LOGW(tag, fmt, ...) ESP_LOGW(tag, fmt, ##__VA_ARGS__)
#define IJPD_LOGI(tag, fmt, ...) ESP_LOGI(tag, fmt, ##__VA_ARGS__)
#define IJPD_LOGD(tag, fmt, ...) ESP_LOGD(tag, fmt, ##__VA_ARGS__)

#endif  // IJPD_PLATFORM_LOG_ADAPTER_H
