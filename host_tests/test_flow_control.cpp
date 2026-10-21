// test_flow_control.cpp —— 高低水位滞回
#include "core/flow_control.h"
#include "assert.h"

using namespace ijpd;

int main() {
    // 构造校验
    {
        FlowControl fc;
        CHECK_TRUE(FlowControl::make(4, 2, &fc) == Err::OK);
        CHECK_TRUE(FlowControl::make(2, 4, &fc) == Err::INVALID_ARGUMENT);  // low >= high
        CHECK_TRUE(FlowControl::make(4, 0, &fc) == Err::INVALID_ARGUMENT);
        CHECK_TRUE(FlowControl::make(0, 0, nullptr) == Err::INVALID_ARGUMENT);
        CHECK_TRUE(FlowControl::make(1, 1, &fc) == Err::INVALID_ARGUMENT);
    }

    // 滞回行为（high=4, low=2）
    {
        FlowControl fc;
        CHECK_TRUE(FlowControl::make(4, 2, &fc) == Err::OK);
        CHECK_TRUE(fc.update(0));   // 空闲：读
        CHECK_TRUE(fc.update(3));   // 未达高水位：读
        CHECK_TRUE(!fc.paused());
        CHECK_TRUE(!fc.update(4));  // 达到高水位：暂停
        CHECK_TRUE(fc.paused());
        CHECK_TRUE(!fc.update(3));  // 滞回区间：维持暂停
        CHECK_TRUE(!fc.update(4));
        CHECK_TRUE(fc.update(2));   // 回落到低水位：恢复
        CHECK_TRUE(!fc.paused());
        CHECK_TRUE(fc.update(3));   // 滞回区间：维持恢复
    }

    // 重复 make 重置状态
    {
        FlowControl fc;
        CHECK_TRUE(FlowControl::make(6, 2, &fc) == Err::OK);
        CHECK_TRUE(!fc.update(6));
        CHECK_TRUE(fc.paused());
        CHECK_TRUE(FlowControl::make(2, 1, &fc) == Err::OK);
        CHECK_TRUE(!fc.paused());
        CHECK_TRUE(fc.update(1));
    }

    return ::ijpd::test::summary("test_flow_control");
}
