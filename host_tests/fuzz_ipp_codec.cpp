// fuzz_ipp_codec.cpp - deterministic malformed IPP input smoke/fuzz test.
#include "assert.h"

#include <cstdint>

#include "airprint_core/ipp_codec.h"

using namespace ijpd::airprint;

int main() {
    uint32_t state = 0xA1B2C3D4u;
    uint8_t input[512] = {};
    for (uint32_t iteration = 0; iteration < 100000; ++iteration) {
        state = state * 1664525u + 1013904223u;
        const size_t length = (state % sizeof(input)) + 1;
        for (size_t i = 0; i < length; ++i) {
            state = state * 1664525u + 1013904223u;
            input[i] = static_cast<uint8_t>(state >> 24);
        }

        IppReader reader;
        const IppError begin = reader.begin(input, length);
        CHECK_TRUE(begin == IppError::Ok || begin == IppError::Truncated ||
                   begin == IppError::InvalidArgument);
        if (begin == IppError::Ok) {
            for (size_t step = 0; step < length + 2; ++step) {
                const IppReadResult result = reader.next();
                if (result.error != IppError::Ok) break;
                if (result.kind == IppReadKind::EndOfAttributes) break;
            }
        }
    }
    return ::ijpd::test::summary("fuzz_ipp_codec");
}
