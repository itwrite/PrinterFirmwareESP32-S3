// test_control_plane.cpp —— 控制面汇入：多通道拆帧/超时/超长/路由/回发
#include "app/control_plane.h"
#include "assert.h"

#include <cstring>
#include <string>
#include <vector>

#include "core/command_router.h"

using namespace ijpd;

namespace {

struct TestSink {
    std::vector<std::string>* out;
    static Err send(void* user, const uint8_t* data, size_t len) {
        static_cast<TestSink*>(user)->out->emplace_back(
            reinterpret_cast<const char*>(data), len);
        return Err::OK;
    }
};

static int g_device_info_calls = 0;

Err hook_ok(void*, JsonWriter& w) {
    ++g_device_info_calls;
    return (w.begin_object() && w.kv_bool("ok", true) && w.end_object()) ? Err::OK
                                                                         : Err::INTERNAL_ERROR;
}

void install_router(CommandRouter& r) {
    CommandRouter::Hooks hk;
    hk.user = nullptr;
    hk.get_device_info = hook_ok;
    hk.get_print_status = hook_ok;
    r.set_hooks(hk);
    r.set_capabilities(Capabilities{false, false, false});
}

const char* kFrame = R"({"msg_id":"m1","cmd":"get_device_info","data":{}})";

}  // namespace

int main() {
    // ================= 1. 正常帧 → 响应经对应通道回发 =================
    {
        CommandRouter r;
        install_router(r);
        ControlPlane plane;
        CHECK_TRUE(plane.init(&r) == Err::OK);

        std::vector<std::string> out0, out1, outb;
        TestSink s0{&out0}, s1{&out1}, sb{&outb};
        plane.set_sink(0, {&s0, TestSink::send});
        plane.set_sink(1, {&s1, TestSink::send});
        plane.set_sink(ControlPlane::kChannelBle, {&sb, TestSink::send});

        // 通道 0：一次到达
        plane.feed(0, reinterpret_cast<const uint8_t*>(kFrame), std::strlen(kFrame), 100);
        const uint8_t hash = '#';
        plane.feed(0, &hash, 1, 100);
        CHECK_EQ(out0.size(), 1u);
        CHECK_TRUE(out0[0].find("\"success\":true") != std::string::npos);
        CHECK_TRUE(out0[0].back() == '#');
        CHECK_EQ(out1.size(), 0u);
        CHECK_EQ(outb.size(), 0u);

        // 通道 1：分多次（半包）
        plane.feed(1, reinterpret_cast<const uint8_t*>(kFrame), 10, 110);
        plane.feed(1, reinterpret_cast<const uint8_t*>(kFrame) + 10,
                   std::strlen(kFrame) - 10, 111);
        plane.feed(1, &hash, 1, 112);
        CHECK_EQ(out1.size(), 1u);
        CHECK_TRUE(out1[0].find("\"cmd\":\"get_device_info\"") != std::string::npos);

        // BLE 通道：20 字节模拟分片
        for (size_t i = 0; i < std::strlen(kFrame); i += 20) {
            const size_t n = std::min<size_t>(20, std::strlen(kFrame) - i);
            plane.feed(ControlPlane::kChannelBle,
                       reinterpret_cast<const uint8_t*>(kFrame) + i, n, 200);
        }
        plane.feed(ControlPlane::kChannelBle, &hash, 1, 200);
        CHECK_EQ(outb.size(), 1u);

        // 一次到达多条
        std::string two = std::string(kFrame) + "#" + kFrame + "#";
        plane.feed(0, reinterpret_cast<const uint8_t*>(two.data()), two.size(), 300);
        CHECK_EQ(out0.size(), 3u);
    }

    // ================= 2. 超长帧 -> FRAME_TOO_LARGE =================
    {
        CommandRouter r;
        install_router(r);
        ControlPlane plane;
        plane.init(&r);
        std::vector<std::string> out;
        TestSink s{&out};
        plane.set_sink(0, {&s, TestSink::send});

        std::string big(FrameSplitter::capacity() + 10, 'x');
        const std::string chunk = big + "#ok#";
        plane.feed(0, reinterpret_cast<const uint8_t*>(chunk.data()), chunk.size(), 0);
        CHECK_EQ(out.size(), 2u);  // 1 个 too_large + 1 个正常响应（顺序不保证）
        const std::string joined = out[0] + out[1];
        CHECK_TRUE(joined.find("\"code\":\"FRAME_TOO_LARGE\"") != std::string::npos);
        CHECK_TRUE(joined.find("\"msg_id\":null") != std::string::npos);
        CHECK_TRUE(joined.find("\"success\":false") != std::string::npos);  // "ok" 非法 JSON
    }

    // ================= 3. 重组超时 -> FRAME_TIMEOUT =================
    {
        CommandRouter r;
        install_router(r);
        ControlPlane plane;
        plane.init(&r);
        std::vector<std::string> out;
        TestSink s{&out};
        plane.set_sink(0, {&s, TestSink::send});

        plane.feed(0, reinterpret_cast<const uint8_t*>(kFrame), 20, 1000);
        CHECK_TRUE(plane.has_partial(0));
        plane.tick(1000 + cfg::CTRL_FRAME_TIMEOUT_MS - 1);
        CHECK_TRUE(plane.has_partial(0));       // 未超时
        CHECK_EQ(out.size(), 0u);
        plane.tick(1000 + cfg::CTRL_FRAME_TIMEOUT_MS);
        CHECK_TRUE(!plane.has_partial(0));      // 超时丢弃
        CHECK_EQ(out.size(), 1u);
        CHECK_TRUE(out[0].find("\"code\":\"FRAME_TIMEOUT\"") != std::string::npos);

        // 丢弃后新帧正常
        plane.feed(0, reinterpret_cast<const uint8_t*>(kFrame), std::strlen(kFrame),
                   5000);
        const uint8_t hash = '#';
        plane.feed(0, &hash, 1, 5000);
        CHECK_EQ(out.size(), 2u);
        CHECK_TRUE(out[1].find("\"success\":true") != std::string::npos);
    }

    // ================= 4. 通道隔离 =================
    {
        CommandRouter r;
        install_router(r);
        ControlPlane plane;
        plane.init(&r);
        std::vector<std::string> out0;
        TestSink s0{&out0};
        plane.set_sink(0, {&s0, TestSink::send});
        plane.set_sink(1, ControlPlane::Sink{});  // 通道 1 无 sink：帧正常处理，仅不回发

        plane.feed(0, reinterpret_cast<const uint8_t*>(kFrame), std::strlen(kFrame), 0);
        plane.feed(1, reinterpret_cast<const uint8_t*>(kFrame), std::strlen(kFrame), 0);
        const uint8_t hash = '#';
        plane.feed(0, &hash, 1, 0);
        plane.feed(1, &hash, 1, 0);
        CHECK_EQ(out0.size(), 1u);
        CHECK_EQ(g_device_info_calls >= 2, true);  // 两个通道都完成了路由
        // 无 sink 的通道半帧超时也不回发（不崩溃）
        plane.feed(1, reinterpret_cast<const uint8_t*>(kFrame), 5, 100);
        plane.tick(100 + cfg::CTRL_FRAME_TIMEOUT_MS + 1);
        CHECK_EQ(out0.size(), 1u);
    }

    // ================= 5. init 参数校验 =================
    {
        ControlPlane plane;
        CHECK_TRUE(plane.init(nullptr) == Err::INVALID_ARGUMENT);
        // 未 init 时 feed 安全无操作
        plane.feed(0, reinterpret_cast<const uint8_t*>("x"), 1, 0);
        plane.tick(0);
    }

    return ::ijpd::test::summary("test_control_plane");
}
