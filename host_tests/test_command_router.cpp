// test_command_router.cpp —— 信封校验 / schema 严格校验 / start_print 语义 /
//                            能力门控 / hook 分发 / 响应封装
#include "app/device_info.h"
#include "assert.h"

#include <cstring>
#include <string>

#include "core/command_router.h"
#include "core/command_schema.h"

using namespace ijpd;

namespace {

// ---- 测试钩子（明确命名：仅用于主机侧验证路由行为，非固件功能） ----
struct TestHooks {
    static StartPrintParams last_start;
    static bool start_called;
    static CancelPrintParams last_cancel;
    static bool cancel_called;
    static FeedPaperParams last_feed;
    static bool feed_called;
    static bool device_info_called;
    static bool status_called;
    static bool cut_called;
    static Err hook_err;             // hook 主动返回的错误
    static bool partial_write_fail;  // 先写一半再报错（验证响应重建）

    static void reset() {
        last_start = StartPrintParams{};
        start_called = cancel_called = feed_called = device_info_called = false;
        status_called = cut_called = false;
        hook_err = Err::OK;
        partial_write_fail = false;
    }

    static Err write_ok(JsonWriter& w) {
        return (w.begin_object() && w.kv_bool("ok", true) && w.end_object()) ? Err::OK
                                                                             : Err::INTERNAL_ERROR;
    }

    static Err h_device_info(void*, JsonWriter& w) {
        device_info_called = true;
        if (hook_err != Err::OK) return hook_err;
        return write_ok(w);
    }
    static Err h_start(void*, const StartPrintParams& p, JsonWriter& w) {
        last_start = p;
        start_called = true;
        if (partial_write_fail) {
            w.kv_i64("partial", 1);  // 故意半写
            return Err::HAL_FAULT;
        }
        if (hook_err != Err::OK) return hook_err;
        return (w.begin_object() && w.kv_cstr("job_id", p.job_id) &&
                w.kv_i64("nominal", p.nominal_packet_bytes) &&
                w.kv_i64("last", p.last_packet_bytes) && w.kv_bool("lz4", p.compression == Compression::Lz4Block) &&
                w.kv_bool("crc", p.integrity == Integrity::Crc32) &&
                w.kv_bool("cut_after", p.cutter_after_finish) && w.end_object())
                   ? Err::OK
                   : Err::INTERNAL_ERROR;
    }
    static Err h_cancel(void*, const CancelPrintParams& p, JsonWriter& w) {
        last_cancel = p;
        cancel_called = true;
        if (hook_err != Err::OK) return hook_err;
        return (w.begin_object() && w.kv_bool("had_id", p.has_job_id) &&
                w.kv_cstr("job_id", p.has_job_id ? p.job_id : "") && w.end_object())
                   ? Err::OK
                   : Err::INTERNAL_ERROR;
    }
    static Err h_status(void*, JsonWriter& w) {
        status_called = true;
        if (hook_err != Err::OK) return hook_err;
        return write_ok(w);
    }
    static Err h_feed(void*, const FeedPaperParams& p, JsonWriter& w) {
        last_feed = p;
        feed_called = true;
        if (hook_err != Err::OK) return hook_err;
        return (w.begin_object() && w.kv_i64("moved_um", p.distance_um) && w.end_object())
                   ? Err::OK
                   : Err::INTERNAL_ERROR;
    }
    static Err h_cut(void*, JsonWriter& w) {
        cut_called = true;
        if (hook_err != Err::OK) return hook_err;
        return write_ok(w);
    }

    static void install(CommandRouter& r) {
        CommandRouter::Hooks hk;
        hk.user = nullptr;
        hk.get_device_info = h_device_info;
        hk.start_print = h_start;
        hk.cancel_print = h_cancel;
        hk.get_print_status = h_status;
        hk.feed_paper = h_feed;
        hk.cut_paper = h_cut;
        r.set_hooks(hk);
        r.set_capabilities(Capabilities{true, true, false});  // 切纸默认不支持
    }
};

StartPrintParams TestHooks::last_start{};
bool TestHooks::start_called = false;
CancelPrintParams TestHooks::last_cancel{};
bool TestHooks::cancel_called = false;
FeedPaperParams TestHooks::last_feed{};
bool TestHooks::feed_called = false;
bool TestHooks::device_info_called = false;
bool TestHooks::status_called = false;
bool TestHooks::cut_called = false;
Err TestHooks::hook_err = Err::OK;
bool TestHooks::partial_write_fail = false;

// 处理一帧并返回响应字符串（含 '#'）
struct Out {
    Err code;
    std::string resp;
};

Out run(CommandRouter& r, const std::string& frame, size_t cap = 0) {
    static char fbuf[cfg::MAX_CONTROL_FRAME_SIZE + 16];
    static char rbuf[cfg::MAX_RESPONSE_FRAME_SIZE];
    std::memcpy(fbuf, frame.data(), frame.size());
    size_t n = 0;
    const Err e = r.handle(fbuf, frame.size(), rbuf, cap != 0 ? cap : sizeof(rbuf), &n);
    return Out{e, std::string(rbuf, n)};
}

const char* kStartPrintValid =
    R"({"msg_id":"job-create-001","cmd":"start_print","data":{)"
    R"("job_id":"job-001","total_bytes_uncompressed":120000,"total_packets":30,)"
    R"("compression":"lz4_block","integrity":"crc32","bytes_per_row":1200,)"
    R"("total_rows":100,"pixel_bits":1,"image_planes":4,)"
    R"("channel_order":["cyan","magenta","yellow","black"],)"
    R"("cutter_after_finish":false}})";

}  // namespace

int main() {
    // ================= 1. 正常分发：get_device_info =================
    {
        CommandRouter r;
        TestHooks::reset();
        TestHooks::install(r);
        const Out out = run(r, R"({"msg_id":"m1","cmd":"get_device_info","data":{}})");
        CHECK_TRUE(out.code == Err::OK);
        CHECK_TRUE(TestHooks::device_info_called);
        CHECK_TRUE(out.resp ==
                   std::string(R"({"msg_id":"m1","cmd":"get_device_info","data":{"success":true,"result":{"ok":true},"error":null}})") + "#");
    }

    // ================= 2. msg_id 含 '#' -> 响应转义 \u0023 =================
    {
        CommandRouter r;
        TestHooks::reset();
        TestHooks::install(r);
        const Out out = run(r, R"({"msg_id":"a\u0023b","cmd":"get_print_status","data":{}})");
        CHECK_TRUE(out.code == Err::OK);
        CHECK_TRUE(out.resp ==
                   std::string(R"({"msg_id":"a\u0023b","cmd":"get_print_status","data":{"success":true,"result":{"ok":true},"error":null}})") + "#");
    }

    // ================= 3. start_print 完整合法：参数与派生量 =================
    {
        CommandRouter r;
        TestHooks::reset();
        TestHooks::install(r);
        const Out out = run(r, kStartPrintValid);
        CHECK_TRUE(out.code == Err::OK);
        CHECK_TRUE(TestHooks::start_called);
        CHECK_EQ_STR(TestHooks::last_start.job_id, "job-001");
        CHECK_EQ(TestHooks::last_start.total_bytes_uncompressed, 120000);
        CHECK_EQ(TestHooks::last_start.total_packets, 30);
    CHECK_TRUE(TestHooks::last_start.compression == Compression::Lz4Block);
        CHECK_TRUE(TestHooks::last_start.transport == StartPrintTransport::Tcp);
        CHECK_TRUE(TestHooks::last_start.integrity == Integrity::Crc32);
        CHECK_EQ(TestHooks::last_start.bytes_per_row, 1200);
        CHECK_EQ(TestHooks::last_start.total_rows, 100);
        CHECK_EQ(TestHooks::last_start.channel_order_count, 4);
        CHECK_TRUE(TestHooks::last_start.channel_order[0] == RasterChannel::Cyan);
        CHECK_TRUE(TestHooks::last_start.channel_order[1] == RasterChannel::Magenta);
        CHECK_TRUE(TestHooks::last_start.channel_order[2] == RasterChannel::Yellow);
        CHECK_TRUE(TestHooks::last_start.channel_order[3] == RasterChannel::Black);
        CHECK_EQ(TestHooks::last_start.nominal_packet_bytes, 4000);
        CHECK_EQ(TestHooks::last_start.last_packet_bytes, 4000);
        CHECK_TRUE(out.resp.find("\"nominal\":4000") != std::string::npos);
        CHECK_TRUE(out.resp.find("\"last\":4000") != std::string::npos);
    }

    // ================= 4. 派生量：非整除 + 缺省字段 =================
    {
        CommandRouter r;
        TestHooks::reset();
        TestHooks::install(r);
        // 100000 = 1000 × 100 行；30 包 -> nominal 3334，last 3294
        const Out out = run(
            r, R"({"msg_id":"m","cmd":"start_print","data":{"job_id":"j2",)"
               R"("total_bytes_uncompressed":100000,"total_packets":30,)"
               R"("compression":"none","bytes_per_row":1000,"total_rows":100}})");
        CHECK_TRUE(out.code == Err::OK);
        CHECK_EQ(TestHooks::last_start.nominal_packet_bytes, 3334);
        CHECK_EQ(TestHooks::last_start.last_packet_bytes, 3314);  // 100000-3334*29
        CHECK_TRUE(TestHooks::last_start.integrity == Integrity::None);   // 缺省 none
        CHECK_TRUE(!TestHooks::last_start.cutter_after_finish);           // 缺省 false
    }
    {
        // 单包任务
        CommandRouter r;
        TestHooks::reset();
        TestHooks::install(r);
        const Out out = run(
            r, R"({"msg_id":"m","cmd":"start_print","data":{"job_id":"j3",)"
               R"("total_bytes_uncompressed":500,"total_packets":1,)"
               R"("compression":"none","bytes_per_row":5,"total_rows":100}})");
        CHECK_TRUE(out.code == Err::OK);
        CHECK_EQ(TestHooks::last_start.nominal_packet_bytes, 500);
        CHECK_EQ(TestHooks::last_start.last_packet_bytes, 500);
    }
    {
        CommandRouter r;
        TestHooks::reset();
        TestHooks::install(r);
        const Out out = run(
            r, R"({"msg_id":"m","cmd":"start_print","data":{"job_id":"mqtt-job",)"
               R"("total_bytes_uncompressed":8,"total_packets":1,"compression":"none",)"
               R"("transport":"mqtt","bytes_per_row":1,"total_rows":8}})");
        CHECK_TRUE(out.code == Err::OK);
        CHECK_TRUE(TestHooks::last_start.transport == StartPrintTransport::Mqtt);
    }

    // ================= 5. 语义校验失败（交叉字段） =================
    {
        struct Case { const char* json; const char* want_msg_part; };
        const Case cases[] = {
            // bpr*rows != total
            {R"({"msg_id":"m","cmd":"start_print","data":{"job_id":"j","total_bytes_uncompressed":999,"total_packets":1,"compression":"none","bytes_per_row":10,"total_rows":100}})",
             "must equal"},
            // total_packets > total_bytes
            {R"({"msg_id":"m","cmd":"start_print","data":{"job_id":"j","total_bytes_uncompressed":5,"total_packets":6,"compression":"none","bytes_per_row":1,"total_rows":5}})",
             "must not exceed"},
            // total_bytes > packets * 65535（1 包装不下 65536）
            {R"({"msg_id":"m","cmd":"start_print","data":{"job_id":"j","total_bytes_uncompressed":65536,"total_packets":1,"compression":"none","bytes_per_row":64,"total_rows":1024}})",
             "max packet size"},
        };
        for (const auto& c : cases) {
            CommandRouter r;
            TestHooks::reset();
            TestHooks::install(r);
            const Out out = run(r, c.json);
            CHECK_TRUE(out.code == Err::INVALID_ARGUMENT);
            CHECK_TRUE(std::strstr(r.last_error_message(), c.want_msg_part) != nullptr);
            CHECK_TRUE(!TestHooks::start_called);
        }
    }
    {
        // 边界合法：1 包恰 65535 字节
        CommandRouter r;
        TestHooks::reset();
        TestHooks::install(r);
        const Out out = run(
            r, R"({"msg_id":"m","cmd":"start_print","data":{"job_id":"j",)"
               R"("total_bytes_uncompressed":65535,"total_packets":1,)"
               R"("compression":"none","bytes_per_row":65535,"total_rows":1}})");
        CHECK_TRUE(out.code == Err::OK);
        CHECK_EQ(TestHooks::last_start.nominal_packet_bytes, 65535);
    }

    // ================= 6. schema 校验失败（全字段基线 + 单字段变异） =================
    {
        const std::string base =
            std::string(R"({"msg_id":"m","cmd":"start_print","data":{"job_id":"j",)")
            + R"("total_bytes_uncompressed":120000,"total_packets":30,)"
            + R"("compression":"lz4_block","integrity":"crc32","bytes_per_row":1200,)"
            + R"("total_rows":100,"cutter_after_finish":false}})";
        struct CaseB { std::string json; const char* msg_part; };
        const auto mut = [](std::string b, const char* from, const char* to) {
            const size_t at = b.find(from);
            CHECK_TRUE(at != std::string::npos);
            return b.replace(at, std::strlen(from), to);
        };
        const CaseB cases2[] = {
            {std::string(R"({"msg_id":"m","cmd":"start_print","data":{"total_bytes_uncompressed":1,)")
                 + R"("total_packets":1,"compression":"none","bytes_per_row":1,"total_rows":1}})",
             "'job_id' is required"},
            {base.substr(0, base.size() - 2) + R"(,"extra":1}})",
             "'extra' is not a recognized field"},
            {mut(base, R"("total_packets":30)", R"("total_packets":"30")"),
             "'total_packets' must be an integer"},
            {mut(base, R"("total_packets":30)", R"("total_packets":0)"),
             "'total_packets' is below the minimum"},
            {mut(base, R"("total_packets":30)", R"("total_packets":65536)"),
             "'total_packets' is above the maximum"},
            {mut(base, R"("total_bytes_uncompressed":120000)", R"("total_bytes_uncompressed":67108865)"),
             "'total_bytes_uncompressed' is above the maximum"},
            {mut(base, R"("compression":"lz4_block")", R"("compression":"zlib")"),
             "'compression' is not one of the allowed values"},
            {mut(base, R"("integrity":"crc32")", R"("integrity":"md5")"),
             "'integrity' is not one of the allowed values"},
            {mut(base, R"("total_rows":100)", R"("total_rows":100,"channel_order":"cyan")"),
             "'channel_order' must be an array"},
            {mut(base, R"("job_id":"j")", R"("job_id":"job-\u00231")"),
             "may only contain"},
            {mut(base, R"("job_id":"j")",
                 R"("job_id":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa")"),
             "is too long"},
            {R"({"msg_id":"m","cmd":"get_device_info","data":{"x":1}})",
             "'x' is not a recognized field"},
            {R"({"msg_id":"m","cmd":"cut_paper","data":{"force":true}})",
             "'force' is not a recognized field"},
            {R"({"msg_id":"m","cmd":"feed_paper","data":{}})",
             "'distance_um' is required"},
            {R"({"msg_id":"m","cmd":"feed_paper","data":{"distance_um":0}})",
             "below the minimum"},
            {R"({"msg_id":"m","cmd":"feed_paper","data":{"distance_um":254001}})",
             "above the maximum"},
        };
        for (const auto& c : cases2) {
            CommandRouter r;
            TestHooks::reset();
            TestHooks::install(r);
            const Out out = run(r, c.json);
            CHECK_TRUE(out.code == Err::INVALID_ARGUMENT);
            CHECK_TRUE(std::strstr(r.last_error_message(), c.msg_part) != nullptr);
            CHECK_TRUE(out.resp.find("\"success\":false") != std::string::npos);
            CHECK_TRUE(out.resp.find("\"code\":\"INVALID_ARGUMENT\"") != std::string::npos);
        }
    }

    // ================= 8. channel_order semantic validation =================
    {
        CommandRouter r;
        TestHooks::reset();
        TestHooks::install(r);
        const Out out = run(
            r, R"({"msg_id":"m","cmd":"start_print","data":{"job_id":"j",)"
               R"("total_bytes_uncompressed":8,"total_packets":1,"compression":"none",)"
               R"("bytes_per_row":1,"total_rows":8,"image_planes":2,)"
               R"("channel_order":["cyan","cyan"]}})");
        CHECK_TRUE(out.code == Err::INVALID_ARGUMENT);
        CHECK_TRUE(std::strstr(r.last_error_message(), "duplicates") != nullptr);
    }

    // ================= 7. 信封校验失败 / 未知命令 =================
    {
        struct Case { std::string json; Err want; };
        const Case cases[] = {
            {R"({"msg_id":"m","cmd":"start_print"})", Err::INVALID_ARGUMENT},            // 2 键
            {R"({"msg_id":"m","cmd":"start_print","data":{},"x":1})", Err::INVALID_ARGUMENT}, // 4 键
            {R"({"msg_id":1,"cmd":"get_device_info","data":{}})", Err::INVALID_ARGUMENT},      // 类型
            {R"({"msg_id":"","cmd":"get_device_info","data":{}})", Err::INVALID_ARGUMENT},     // 空
            {std::string(R"({"msg_id":")") + std::string(41, 'a') +
                 R"(","cmd":"get_device_info","data":{}})",
             Err::INVALID_ARGUMENT},                                                            // 41 字节
            {R"({"msg_id":"m","cmd":"","data":{}})", Err::INVALID_ARGUMENT},                   // 空 cmd
            {R"({"msg_id":"m","cmd":42,"data":{}})", Err::INVALID_ARGUMENT},                   // cmd 类型
            {R"({"msg_id":"m","cmd":"get_device_info","data":[]})", Err::INVALID_ARGUMENT},    // data 数组
            {R"({"msg_id":"m","cmd":"reboot","data":{}})", Err::UNKNOWN_COMMAND},              // 白名单外
            {R"([1,2,3])", Err::INVALID_ARGUMENT},                                             // 非对象根
            {R"({"a":1} garbage)", Err::INVALID_JSON},                                          // 尾随
            {R"({"a":1.5})", Err::INVALID_JSON},                                                // 浮点
        };
        for (const auto& c : cases) {
            CommandRouter r;
            TestHooks::reset();
            TestHooks::install(r);
            const Out out = run(r, c.json);
            CHECK_TRUE(out.code == c.want);
            CHECK_TRUE(out.resp.back() == '#');
            CHECK_TRUE(out.resp.find("\"success\":false") != std::string::npos);
        }
        // INVALID_JSON 时 msg_id/cmd 均为 null
        {
            CommandRouter r;
            TestHooks::install(r);
            const Out out = run(r, "{bad json");
            CHECK_TRUE(out.code == Err::INVALID_JSON);
            CHECK_TRUE(out.resp.find(R"("msg_id":null)") != std::string::npos);
            CHECK_TRUE(out.resp.find(R"("cmd":null)") != std::string::npos);
        }
    }

    // ================= 8. 能力门控 UNSUPPORTED =================
    {
        // 切纸默认不支持（install 使用 cutter=false）
        CommandRouter r;
        TestHooks::reset();
        TestHooks::install(r);
        Out out = run(r, R"({"msg_id":"m","cmd":"cut_paper","data":{}})");
        CHECK_TRUE(out.code == Err::UNSUPPORTED);
        CHECK_TRUE(!TestHooks::cut_called);
        CHECK_TRUE(out.resp.find("\"code\":\"UNSUPPORTED\"") != std::string::npos);

        // 打印能力关闭
        TestHooks::reset();
        TestHooks::install(r);
        r.set_capabilities(Capabilities{false, true, false});
        out = run(r, kStartPrintValid);
        CHECK_TRUE(out.code == Err::UNSUPPORTED);
        CHECK_TRUE(!TestHooks::start_called);

        // 走纸能力关闭
        TestHooks::reset();
        TestHooks::install(r);
        r.set_capabilities(Capabilities{true, false, true});
        out = run(r, R"({"msg_id":"m","cmd":"feed_paper","data":{"distance_um":1000}})");
        CHECK_TRUE(out.code == Err::UNSUPPORTED);
        CHECK_TRUE(!TestHooks::feed_called);

        // 走纸开启且合法
        TestHooks::reset();
        TestHooks::install(r);
        out = run(r, R"({"msg_id":"m","cmd":"feed_paper","data":{"distance_um":254000}})");
        CHECK_TRUE(out.code == Err::OK);
        CHECK_TRUE(TestHooks::feed_called);
        CHECK_EQ(TestHooks::last_feed.distance_um, 254000);
    }

    // ================= 9. hook 错误传播与半写重建 =================
    {
        CommandRouter r;
        TestHooks::reset();
        TestHooks::install(r);
        TestHooks::hook_err = Err::HAL_FAULT;
        const Out out = run(r, R"({"msg_id":"m9","cmd":"get_print_status","data":{}})");
        CHECK_TRUE(out.code == Err::HAL_FAULT);
        CHECK_TRUE(out.resp ==
                   std::string(R"({"msg_id":"m9","cmd":"get_print_status","data":{"success":false,"result":null,"error":{"code":"HAL_FAULT","message":"HAL fault"}}})") + "#");

        // hook 先写一半再失败：响应必须被完整重建，无残留
        TestHooks::reset();
        TestHooks::install(r);
        TestHooks::partial_write_fail = true;
        const Out out2 = run(r, kStartPrintValid);
        CHECK_TRUE(out2.code == Err::HAL_FAULT);
        CHECK_TRUE(out2.resp.find("partial") == std::string::npos);
        CHECK_TRUE(out2.resp.find("\"success\":false") != std::string::npos);
    }

    // ================= 10. 响应缓冲不足 -> INTERNAL_ERROR =================
    {
        CommandRouter r;
        TestHooks::reset();
        // 10a. hook 观察到溢出并返回 INTERNAL_ERROR：错误响应回显 msg_id/cmd
        CommandRouter::Hooks hk;
        hk.user = nullptr;
        hk.get_device_info = [](void*, JsonWriter& w) -> Err {
            if (!w.begin_object()) return Err::INTERNAL_ERROR;
            for (int i = 0; i < 64; ++i) {
                char key[8];
                key[0] = 'k';
                key[1] = static_cast<char>('0' + (i / 10));
                key[2] = static_cast<char>('0' + (i % 10));
                key[3] = '\0';
                if (!w.kv_i64(key, i)) return Err::INTERNAL_ERROR;
            }
            return w.end_object() ? Err::OK : Err::INTERNAL_ERROR;
        };
        r.set_hooks(hk);
        const Out out = run(r, R"({"msg_id":"m","cmd":"get_device_info","data":{}})", 200);
        CHECK_TRUE(out.code == Err::INTERNAL_ERROR);
        CHECK_TRUE(out.resp.find("\"msg_id\":\"m\"") != std::string::npos);
        CHECK_TRUE(out.resp.find("\"code\":\"INTERNAL_ERROR\"") != std::string::npos);
        CHECK_TRUE(out.resp.find("\"success\":false") != std::string::npos);

        // 10b. hook 无视失败恒返回 OK：封装器检出溢出 -> 最小响应（msg_id/cmd null）
        CommandRouter::Hooks hk2;
        hk2.user = nullptr;
        hk2.get_device_info = [](void*, JsonWriter& w) -> Err {
            (void)w.begin_object();
            for (int i = 0; i < 64; ++i) {
                char key[8];
                key[0] = 'k';
                key[1] = static_cast<char>('0' + (i / 10));
                key[2] = static_cast<char>('0' + (i % 10));
                key[3] = '\0';
                (void)w.kv_i64(key, i);  // 忽略失败（测试场景）
            }
            (void)w.end_object();
            return Err::OK;
        };
        CommandRouter r2;
        r2.set_hooks(hk2);
        const Out out2b = run(r2, R"({"msg_id":"m","cmd":"get_device_info","data":{}})", 200);
        CHECK_TRUE(out2b.code == Err::INTERNAL_ERROR);
        CHECK_TRUE(out2b.resp ==
                   std::string(R"({"msg_id":null,"cmd":null,"data":{"success":false,"result":null,"error":{"code":"INTERNAL_ERROR","message":"response serialization failed"}}})") + "#");

        // 10c. 极小缓冲：连最小响应都放不下 -> 空响应（传输层丢弃）
        const Out out3 = run(r, R"({"msg_id":"m","cmd":"get_device_info","data":{}})", 16);
        CHECK_TRUE(out3.code == Err::INTERNAL_ERROR);
        CHECK_EQ(out3.resp.size(), 0u);
    }

    // ================= 11. cancel_print：带/不带 job_id =================
    {
        CommandRouter r;
        TestHooks::reset();
        TestHooks::install(r);
        Out out = run(r, R"({"msg_id":"m","cmd":"cancel_print","data":{}})");
        CHECK_TRUE(out.code == Err::OK && TestHooks::cancel_called);
        CHECK_TRUE(!TestHooks::last_cancel.has_job_id);

        out = run(r, R"({"msg_id":"m","cmd":"cancel_print","data":{"job_id":"job-42"}})");
        CHECK_TRUE(out.code == Err::OK);
        CHECK_TRUE(TestHooks::last_cancel.has_job_id);
        CHECK_EQ_STR(TestHooks::last_cancel.job_id, "job-42");
    }

    // ================= 12. schema 单一数据源一致性 =================
    {
        CHECK_EQ(command_count(), 6u);
        CHECK_TRUE(find_command("start_print", 11) != nullptr);
        CHECK_TRUE(find_command("nope", 4) == nullptr);
        // 每个命令的 cmd 与 find 互查
        for (size_t i = 0; i < command_count(); ++i) {
            const CommandSpec* c = command_at(i);
            CHECK_TRUE(c != nullptr);
            CHECK_TRUE(find_command(c->cmd, std::strlen(c->cmd)) == c);
        }
    }

    // ================= 13. 端到端：真 device_info result 组装 =================
    {
        CommandRouter r;
        CommandRouter::Hooks hk;
        hk.user = nullptr;
        hk.get_device_info = [](void* user, JsonWriter& w) -> Err {
            (void)user;
            DeviceInfo di{};
            di.device_id = "IJPD-TEST-01";
            di.model = "IJPD-P61-MOCK";
            di.fw_version = "0.1.0-phase2";
            di.hal_mode = "mock";
            di.psram_available = true;
            di.psram_sizes_valid = false;
            di.cap_paper_feed = true;
            di.cap_cutter = false;
            di.cap_mock_print = true;
            return write_device_info_result(w, di);
        };
        r.set_hooks(hk);
        r.set_capabilities(Capabilities{true, true, false});
        const Out out = run(r, R"({"msg_id":"di","cmd":"get_device_info","data":{}})");
        CHECK_TRUE(out.code == Err::OK);
        CHECK_TRUE(out.resp.find("\"protocol_version\":\"1.0\"") != std::string::npos);
        CHECK_TRUE(out.resp.find("\"data_port\":9101") != std::string::npos);
        CHECK_TRUE(out.resp.find("\"code\"") == std::string::npos);  // 无错误对象
        CHECK_TRUE(out.resp.size() > 2000u);  // 元数据量级
    }

    return ::ijpd::test::summary("test_command_router");
}
