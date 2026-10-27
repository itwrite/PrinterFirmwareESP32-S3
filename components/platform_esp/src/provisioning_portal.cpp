// provisioning_portal.cpp —— SoftAP Web 配网门户
#include "platform_esp/provisioning_portal.h"

#include <cstring>
#include <unistd.h>

#include "esp_event.h"
#include "esp_system.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "lwip/sockets.h"
#include "nvs.h"

#include "esp_mac.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "platform_esp/log_adapter.h"

namespace ijpd {
namespace {

constexpr const char* kTag = "portal";
constexpr uint16_t kPortalPort = 80;
constexpr int kMaxStations = 2;

// ---- 配网页面 HTML（单文件，内嵌 CSS/JS，手机友好） ----
constexpr const char kPortalHtml[] = R"html(<!DOCTYPE html>
<html lang="zh">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1,user-scalable=no">
<title>IJPD Printer Setup</title>
<style>
*{margin:0;padding:0;box-sizing:border-box;font-family:-apple-system,sans-serif}
body{background:#f5f5f5;min-height:100vh;padding:20px;display:flex;justify-content:center}
.card{background:#fff;border-radius:16px;padding:24px;width:100%;max-width:400px;
      box-shadow:0 2px 12px rgba(0,0,0,.08);margin-top:40px}
h1{font-size:22px;color:#1a1a1a;text-align:center;margin-bottom:8px}
.sub{font-size:14px;color:#888;text-align:center;margin-bottom:24px}
.ssid{display:flex;align-items:center;justify-content:space-between;padding:14px 16px;
      border:1px solid #e0e0e0;border-radius:10px;margin-bottom:10px;cursor:pointer;
      transition:all .15s}
.ssid:active{background:#f0f0f0}
.ssid.sel{border-color:#007AFF;background:#F0F7FF}
.ssid .name{font-size:15px;font-weight:500;color:#333}
.ssid .rssi{font-size:12px;color:#999}
.ssid .lock{font-size:12px;color:#bbb;margin-left:6px}
input[type=password]{width:100%;padding:14px 16px;border:1px solid #e0e0e0;
      border-radius:10px;font-size:16px;margin:12px 0;outline:none;-webkit-appearance:none}
input[type=password]:focus{border-color:#007AFF}
.btn{width:100%;padding:14px;background:#007AFF;color:#fff;border:none;border-radius:10px;
     font-size:16px;font-weight:600;cursor:pointer;margin-top:8px}
.btn:active{background:#0055CC}
.btn:disabled{background:#ccc}
.status{padding:12px;border-radius:8px;font-size:14px;margin-top:12px;display:none}
.status.ok{background:#E8F5E9;color:#2E7D32;display:block}
.status.err{background:#FFEBEE;color:#C62828;display:block}
.status.wait{background:#FFF3E0;color:#E65100;display:block}
.spinner{display:inline-block;width:14px;height:14px;border:2px solid #E65100;
         border-top-color:transparent;border-radius:50%;animation:spin 1s linear infinite;
         margin-right:6px;vertical-align:middle}
@keyframes spin{to{transform:rotate(360deg)}}
.refresh{font-size:13px;color:#007AFF;text-align:center;margin-bottom:16px;cursor:pointer}
</style>
</head>
<body>
<div class="card">
  <h1>打印机 Wi-Fi 设置</h1>
  <p class="sub">选择你的 Wi-Fi 网络</p>
  <div class="refresh" onclick="scan()">↻ 刷新网络列表</div>
  <div id="list"><p class="sub" style="margin:20px 0">扫描中...</p></div>
  <div style="margin:16px 0 8px;text-align:center">
    <a href="#" onclick="manual()" style="color:#007AFF;font-size:14px;text-decoration:none">或手动输入网络名称 ↓</a>
  </div>
  <div id="manualBox" style="display:block">
    <input type="text" id="mssid" placeholder="网络名称 (SSID)" style="width:100%;padding:14px 16px;border:1px solid #e0e0e0;border-radius:10px;font-size:16px;margin:4px 0;outline:none">
  </div>
  <input type="password" id="pass" placeholder="Wi-Fi 密码" style="display:none">
  <button class="btn" id="btn" onclick="connect()" style="display:none" disabled>连接</button>
  <div id="status" class="status"></div>
</div>
<script>
var sel='';
function scan(){
  document.getElementById('list').innerHTML='<p class="sub" style="margin:20px 0">扫描中...</p>';
  fetch('/scan',{signal:AbortSignal.timeout(15000)}).then(r=>r.json()).then(d=>{
    var h='';
    d.sort((a,b)=>b.rssi-a.rssi);
    for(var i=0;i<d.length;i++){
      var s=d[i];
      var lock=s.auth>0?'🔒':'';
      h+='<div class="ssid" data-ssid="'+s.ssid.replace(/"/g,'&quot;')+'">'
        +'<span class="name">'+s.ssid+'</span>'
        +'<span><span class="rssi">'+s.rssi+' dBm</span>'
        +'<span class="lock">'+lock+'</span></span></div>';
    }
    document.getElementById('list').innerHTML=h||'<p class="sub">未找到网络</p>';
    document.querySelectorAll('.ssid').forEach(el=>{
      el.onclick=()=>pick(el.dataset.ssid);
    });
  }).catch(e=>{document.getElementById('list').innerHTML='<p class="sub">扫描失败('+e.message+')，请刷新</p>'});
}
function pick(s){
  sel=s;
  document.querySelectorAll('.ssid').forEach(el=>el.classList.remove('sel'));
  event.currentTarget.classList.add('sel');
  document.getElementById('pass').style.display='block';
  document.getElementById('pass').placeholder=s+' 的密码';
  document.getElementById('btn').style.display='block';
  document.getElementById('btn').disabled=false;
}
function manual(){
  var box=document.getElementById('manualBox');
  box.style.display=box.style.display==='none'?'block':'none';
  if(box.style.display==='block'){
    document.getElementById('mssid').focus();
    document.getElementById('mssid').oninput=function(){
      sel=this.value.trim();
      if(sel){
        document.getElementById('pass').style.display='block';
        document.getElementById('pass').placeholder=sel+' 的密码';
        document.getElementById('btn').style.display='block';
        document.getElementById('btn').disabled=false;
      }
    };
  }
}
function connect(){
  var p=document.getElementById('pass').value;
  if(!p){alert('请输入密码');return}
  var st=document.getElementById('status');
  st.className='status wait';
  st.innerHTML='<span class="spinner"></span>正在连接 '+sel+' ...';
  document.getElementById('btn').disabled=true;
  fetch('/connect',{method:'POST',
    headers:{'Content-Type':'application/x-www-form-urlencoded'},
    body:'ssid='+encodeURIComponent(sel)+'&pass='+encodeURIComponent(p)
  }).then(r=>r.json()).then(d=>{
    if(d.ok){st.className='status ok';
      st.innerHTML='✅ 连接成功！设备 IP: '+d.ip+'<br>请重新连接到你的 Wi-Fi 网络。';
    }else{st.className='status err';
      st.innerHTML='❌ '+d.msg+'<br>请检查密码后重试。';
      document.getElementById('btn').disabled=false;
    }
  }).catch(()=>{st.className='status err';
    st.innerHTML='❌ 请求失败，请重试';
    document.getElementById('btn').disabled=false;
  });
}
scan();
manual();  // 默认展开手动输入框
</script>
</body>
</html>)html";

// URL 解码
int url_decode(char* dst, size_t dst_cap, const char* src, size_t src_len) {
    size_t di = 0;
    for (size_t si = 0; si < src_len && di < dst_cap - 1; ++si) {
        if (src[si] == '%' && si + 2 < src_len) {
            auto hex = [](char c) -> int {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                return -1;
            };
            const int hi = hex(src[si + 1]);
            const int lo = hex(src[si + 2]);
            if (hi >= 0 && lo >= 0) {
                dst[di++] = static_cast<char>((hi << 4) | lo);
                si += 2;
                continue;
            }
        }
        if (src[si] == '+') {
            dst[di++] = ' ';
        } else {
            dst[di++] = src[si];
        }
    }
    dst[di] = '\0';
    return static_cast<int>(di);
}

// ---- 扫描缓存（门户启动时预扫描；/scan 直接返回缓存） ----
char g_scan_cache[3072] = "[]";  // 缓存的扫描结果 JSON
volatile bool g_scan_ready = false;

void do_background_scan() noexcept {
    // 等待 Wi-Fi 射频稳定
    vTaskDelay(pdMS_TO_TICKS(5000));

    for (int attempt = 0; attempt < 3; ++attempt) {
        esp_wifi_scan_stop();
        vTaskDelay(pdMS_TO_TICKS(500));

        wifi_scan_config_t scan_cfg = {};
        IJPD_LOGI(kTag, "background scan attempt %d...", attempt + 1);
        esp_err_t sr = esp_wifi_scan_start(&scan_cfg, true);
        if (sr != ESP_OK) {
            IJPD_LOGE(kTag, "scan err=%d", sr);
            continue;
        }

        uint16_t count = 0;
        esp_wifi_scan_get_ap_num(&count);
        if (count == 0) {
            IJPD_LOGW(kTag, "scan found 0, retry");
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        if (count > 25) count = 25;

        wifi_ap_record_t aps[25];
        uint16_t real = count;
        esp_wifi_scan_get_ap_records(&real, aps);

        size_t jl = 1;
        g_scan_cache[0] = '[';
        for (int i = 0; i < real; ++i) {
            if (aps[i].ssid[0] == ' ') continue;
            const char* ssid = reinterpret_cast<char*>(aps[i].ssid);
            if (strstr(ssid, "IJPD-Setup") != nullptr) continue;

            char esc[65] = {0};
            size_t el = 0;
            for (size_t j = 0; j < strlen(ssid) && el < 62; ++j) {
                if (ssid[j] == '"' || ssid[j] == '\\') esc[el++] = '\\';
                esc[el++] = ssid[j];
            }
            jl += snprintf(g_scan_cache + jl, sizeof(g_scan_cache) - jl,
                           "%s{\"ssid\":\"%s\",\"rssi\":%d,\"auth\":%d}",
                           (jl > 1) ? "," : "", esc, aps[i].rssi,
                           static_cast<int>(aps[i].authmode != WIFI_AUTH_OPEN));
        }
        g_scan_cache[jl] = ']';
        g_scan_cache[jl + 1] = ' ';
        g_scan_ready = true;
        IJPD_LOGI(kTag, "scan cached %u APs: %s", real, g_scan_cache);
        return;
    }
    // 三次失败：返回空
    strcpy(g_scan_cache, "[]");
    g_scan_ready = true;
    IJPD_LOGW(kTag, "scan failed 3x, cache=empty");
}

}  // namespace

// ---------------------------------------------------------------- NVS

bool ProvisioningPortal::has_saved_credentials() noexcept {
    nvs_handle_t h;
    if (nvs_open(kNvsNamespace, NVS_READONLY, &h) != ESP_OK) return false;
    size_t len = 0;
    const bool has = nvs_get_str(h, kNvsKeySsid, nullptr, &len) == ESP_OK && len > 1;
    nvs_close(h);
    return has;
}

bool ProvisioningPortal::try_connect_saved() noexcept {
    nvs_handle_t h;
    if (nvs_open(kNvsNamespace, NVS_READONLY, &h) != ESP_OK) return false;

    char ssid[33] = {0};
    char pass[65] = {0};
    size_t slen = sizeof(ssid);
    size_t plen = sizeof(pass);
    if (nvs_get_str(h, kNvsKeySsid, ssid, &slen) != ESP_OK ||
        nvs_get_str(h, kNvsKeyPass, pass, &plen) != ESP_OK) {
        nvs_close(h);
        return false;
    }
    nvs_close(h);

    wifi_config_t cfg = {};
    strncpy(reinterpret_cast<char*>(cfg.sta.ssid), ssid, sizeof(cfg.sta.ssid) - 1);
    strncpy(reinterpret_cast<char*>(cfg.sta.password), pass, sizeof(cfg.sta.password) - 1);
    cfg.sta.pmf_cfg.capable = true;
    cfg.sta.pmf_cfg.required = false;

    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_config(WIFI_IF_STA, &cfg);
    esp_wifi_start();
    esp_wifi_connect();

    // 等待连接（最多 12 秒）
    for (int i = 0; i < 120; ++i) {
        vTaskDelay(pdMS_TO_TICKS(100));
        wifi_ap_record_t ap;
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
            IJPD_LOGI(kTag, "STA connected to %s", ssid);
            return true;
        }
    }
    IJPD_LOGW(kTag, "STA connect timeout");
    return false;
}

// ---------------------------------------------------------------- SoftAP + HTTP

Err ProvisioningPortal::start() noexcept {
    if (active_) return Err::OK;

    // AP 名称带 MAC 后缀
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    char ap_name[24];
    snprintf(ap_name, sizeof(ap_name), "IJPD-Setup-%02X%02X", mac[4], mac[5]);

    // 彻底停止 Wi-Fi（清除所有 STA 状态/重试），纯 AP 模式重启。
    // 注意:不能改用 APSTA 以支持扫描 —— 实测 SoftAP 有客户端关联并传输时,
    // WiFi 库 beacon 定时器崩溃(ieee80211_hostap_send_beacon_process
    // LoadProhibited)。配网用手动输入 SSID 即可。
    esp_wifi_stop();
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_wifi_set_mode(WIFI_MODE_AP);  // 纯 AP，无 STA

    wifi_config_t ap_cfg = {};
    strncpy(reinterpret_cast<char*>(ap_cfg.ap.ssid), ap_name, sizeof(ap_cfg.ap.ssid) - 1);
    strncpy(reinterpret_cast<char*>(ap_cfg.ap.password), "12345678", sizeof(ap_cfg.ap.password) - 1);
    ap_cfg.ap.ssid_len = strlen(ap_name);
    ap_cfg.ap.channel = 6;
    ap_cfg.ap.max_connection = kMaxStations;
    ap_cfg.ap.authmode = WIFI_AUTH_WPA2_PSK;
    ap_cfg.ap.ssid_hidden = 0;

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    // HTTP 服务器
    server_fd_ = lwip_socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (server_fd_ < 0) return Err::OUT_OF_MEMORY;

    int opt = 1;
    lwip_setsockopt(server_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);  // 192.168.4.1
    addr.sin_port = htons(kPortalPort);
    if (lwip_bind(server_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 ||
        lwip_listen(server_fd_, 2) != 0) {
        lwip_close(server_fd_);
        server_fd_ = -1;
        return Err::INTERNAL_ERROR;
    }

    active_ = true;
    running_ = true;
    if (xTaskCreatePinnedToCore(task_entry_, "portal", 8192, this, 5, nullptr, 0) != pdPASS) {
        active_ = false;
        running_ = false;
        lwip_close(server_fd_);
        server_fd_ = -1;
        return Err::OUT_OF_MEMORY;
    }

    // 后台扫描已禁用（会断开 AP 客户端；使用手动输入 SSID 替代）
    // xTaskCreatePinnedToCore(
    //     [](void*) { do_background_scan(); vTaskDelete(nullptr); },
    //     "scan", 4096, nullptr, 3, nullptr, 0);

    IJPD_LOGI(kTag, "portal started: AP=%s IP=192.168.4.1 port=%d", ap_name, kPortalPort);
    return Err::OK;
}

void ProvisioningPortal::stop() noexcept {
    running_ = false;
    active_ = false;
    if (server_fd_ >= 0) {
        lwip_close(server_fd_);
        server_fd_ = -1;
    }
    IJPD_LOGI(kTag, "portal stopped");
}

void ProvisioningPortal::task_entry_(void* arg) noexcept {
    static_cast<ProvisioningPortal*>(arg)->portal_loop_();
    vTaskDelete(nullptr);
}

void ProvisioningPortal::portal_loop_() noexcept {
    while (running_) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(server_fd_, &rfds);
        timeval tv{1, 0};

        const int n = lwip_select(server_fd_ + 1, &rfds, nullptr, nullptr, &tv);
        if (n <= 0 || !running_) continue;

        sockaddr_in peer{};
        socklen_t plen = sizeof(peer);
        const int fd = lwip_accept(server_fd_, reinterpret_cast<sockaddr*>(&peer), &plen);
        if (fd >= 0) {
            handle_client_(fd);
            lwip_close(fd);
        }
    }
}

void ProvisioningPortal::handle_client_(int fd) noexcept {
    // 读取请求（最多 2048）
    char buf[2048] = {0};
    const ssize_t n = lwip_recv(fd, buf, sizeof(buf) - 1, 0);
    if (n <= 0) return;
    buf[n] = '\0';

    // 解析方法与路径
    char method[8] = {0};
    char path[32] = {0};
    sscanf(buf, "%7s %31s", method, path);

    if (strcmp(method, "GET") == 0 && strcmp(path, "/") == 0) {
        send_html_(fd);
    } else if (strcmp(method, "GET") == 0 && strcmp(path, "/scan") == 0) {
        send_scan_json_(fd);
    } else if (strcmp(method, "POST") == 0 && strcmp(path, "/connect") == 0) {
        // 找到 body（在 \r\n\r\n 之后）
        const char* body = strstr(buf, "\r\n\r\n");
        if (body != nullptr) {
            handle_connect_(fd, body + 4, strlen(body + 4));
        }
    } else {
        send_redirect_(fd, "/");
    }
}

void ProvisioningPortal::send_html_(int fd) noexcept {
    char header[128];
    const int hl = snprintf(header, sizeof(header),
                             "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\n"
                             "Content-Length: %zu\r\nConnection: close\r\n\r\n",
                             strlen(kPortalHtml));
    lwip_send(fd, header, hl, 0);
    lwip_send(fd, kPortalHtml, strlen(kPortalHtml), 0);
}

void ProvisioningPortal::send_scan_json_(int fd) noexcept {
    // 纯 AP 模式扫描返回 ESP_FAIL(不支持)。门户以手动输入 SSID 为主,
    // 扫描失败时返回空列表,前端显示"未找到网络"并保留手动输入框。
    wifi_scan_config_t scan_cfg = {};  // 全零 = 默认全信道主动扫描
    esp_err_t sr = esp_wifi_scan_start(&scan_cfg, true);
    if (sr != ESP_OK) {
        IJPD_LOGW(kTag, "scan unsupported in AP mode (err=%d), use manual input", sr);
        send_json_(fd, R"json([{"ssid":"(Scan unavailable, please enter manually)","rssi":-100,"auth":0}])json");
        return;
    }

    uint16_t count = 0;
    esp_wifi_scan_get_ap_num(&count);
    if (count == 0) {
        send_json_(fd, R"json([{"ssid":"0 APs found - try manual input","rssi":-100,"auth":0}])json");
        return;
    }

    if (count > 30) count = 30;
    IJPD_LOGI(kTag, "scan found %u APs", count);

    // 构建 JSON
    char json[4096] = "[";
    size_t jl = 1;
    if (count > 0) {
        wifi_ap_record_t aps[30];
        uint16_t real = count;
        esp_wifi_scan_get_ap_records(&real, aps);
        for (int i = 0; i < real; ++i) {
            if (aps[i].ssid[0] == '\0') continue;
            const char* ssid = reinterpret_cast<char*>(aps[i].ssid);
            if (strstr(ssid, "IJPD-Setup") != nullptr) continue;
            // 转义 JSON 特殊字符
            char esc[65] = {0};
            size_t el = 0;
            for (size_t j = 0; j < strlen(ssid) && el < 63; ++j) {
                if (ssid[j] == '"' || ssid[j] == '\\') {
                    if (el < 62) { esc[el++] = '\\'; }
                }
                esc[el++] = ssid[j];
            }
            jl += snprintf(json + jl, sizeof(json) - jl,
                           "%s{\"ssid\":\"%s\",\"rssi\":%d,\"auth\":%d}",
                           (jl > 1) ? "," : "", esc, aps[i].rssi,
                           static_cast<int>(aps[i].authmode != WIFI_AUTH_OPEN));
        }
    }
    json[jl] = ']';
    json[jl + 1] = '\0';

    IJPD_LOGI(kTag, "scan json: %s", json);
    send_json_(fd, json);
}

void ProvisioningPortal::handle_connect_(int fd, const char* body,
                                         size_t body_len) noexcept {
    // 解析 ssid=xxx&pass=yyy
    char ssid[65] = {0};
    char pass[65] = {0};

    const char* ssid_p = strstr(body, "ssid=");
    const char* pass_p = strstr(body, "pass=");
    if (ssid_p == nullptr || pass_p == nullptr) {
        send_json_(fd, R"json({"ok":false,"msg":"参数缺失"})json");
        return;
    }

    ssid_p += 5;
    pass_p += 5;
    const char* ssid_end = strchr(ssid_p, '&');
    const size_t ssid_raw_len = (ssid_end != nullptr) ? (ssid_end - ssid_p) : strlen(ssid_p);
    url_decode(ssid, sizeof(ssid), ssid_p, ssid_raw_len);
    url_decode(pass, sizeof(pass), pass_p, strlen(pass_p));

    if (strlen(ssid) == 0) {
        send_json_(fd, R"json({"ok":false,"msg":"SSID 不能为空"})json");
        return;
    }

    IJPD_LOGI(kTag, "connect: ssid=%s pass_len=%d", ssid, (int)strlen(pass));

    // 保存到 NVS
    nvs_handle_t h;
    if (nvs_open(kNvsNamespace, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_str(h, kNvsKeySsid, ssid);
        nvs_set_str(h, kNvsKeyPass, pass);
        nvs_commit(h);
        nvs_close(h);
        IJPD_LOGI(kTag, "credentials saved to NVS");
    }

    // 立即响应（客户端收到后设备将重启并尝试连接）
    send_json_(fd, R"json({"ok":true,"ip":"rebooting"})json");

    // 延迟重启（让响应先发出去）
    IJPD_LOGI(kTag, "rebooting in 2s...");
    vTaskDelay(pdMS_TO_TICKS(2000));
    esp_restart();
}

void ProvisioningPortal::send_json_(int fd, const char* json) noexcept {
    char header[128];
    const int hl = snprintf(header, sizeof(header),
                             "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
                             "Content-Length: %zu\r\nConnection: close\r\n"
                             "Access-Control-Allow-Origin: *\r\n\r\n",
                             strlen(json));
    lwip_send(fd, header, hl, 0);
    lwip_send(fd, json, strlen(json), 0);
}

void ProvisioningPortal::send_redirect_(int fd, const char* path) noexcept {
    const char* resp = "HTTP/1.1 302 Found\r\nLocation: /\r\nConnection: close\r\n\r\n";
    lwip_send(fd, resp, strlen(resp), 0);
}

}  // namespace ijpd
