/**
 * MIT License
 *
 * Copyright (c) 2026 Mag1c.H
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 * */
#include <cstdint>
#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>
#include "ascend/ffts_d2d_dispatcher_ascend.h"

namespace {

constexpr size_t kCopyCount = 64;
constexpr size_t kLaneCount = 8;
constexpr size_t kCopySize = 37 * 1024;

bool Check(bool condition, const std::string& message)
{
    if (!condition) { std::cerr << message << '\n'; }
    return condition;
}

void SetEnv(const char* name, const char* value)
{
#ifdef _WIN32
    _putenv_s(name, value);
#else
    setenv(name, value, 1);
#endif
}

std::vector<AscendFftsCopySpec> MakeCopies(size_t count)
{
    std::vector<AscendFftsCopySpec> copies;
    copies.reserve(count);
    constexpr uintptr_t kDstBase = 0x100000000ULL;
    constexpr uintptr_t kSrcBase = 0x200000000ULL;
    constexpr uintptr_t kStride = 0x100000ULL;
    for (size_t i = 0; i < count; ++i) {
        auto* dst = reinterpret_cast<void*>(kDstBase + i * kStride);
        const auto* src = reinterpret_cast<const void*>(kSrcBase + i * kStride);
        copies.push_back({dst, src, kCopySize});
    }
    return copies;
}

bool CheckEightLaneDependencyGraph()
{
    SetEnv(kFftsMaxReadyLanesEnv, "8");

    FftsD2DDispatcher dispatcher;
    const auto copies = MakeCopies(kCopyCount);
    const uint16_t readyCount = dispatcher.BuildCopies(copies);

    bool ok = true;
    ok = Check(readyCount == kLaneCount, "readyCount should be 8") && ok;
    ok = Check(dispatcher.ContextCountForTest() == kCopyCount,
               "dispatcher should build 64 contexts") &&
         ok;
    ok = Check(dispatcher.DescBufForTest() == &dispatcher.ContextForTest(0),
               "task descriptor buffer should point to the built contexts") &&
         ok;
    ok = Check(dispatcher.DescBufLenForTest() == sizeof(rtFftsPlusComCtx_t) * kCopyCount,
               "task descriptor buffer length should cover all contexts") &&
         ok;

    for (size_t i = 0; i < kCopyCount; ++i) {
        const auto& ctx = dispatcher.ContextForTest(i);
        const bool hasPredecessor = i >= kLaneCount;
        const bool hasSuccessor = i + kLaneCount < kCopyCount;
        const auto expectedSuccessor = static_cast<uint16_t>(i + kLaneCount);

        ok = Check(ctx.contextType == RT_CTX_TYPE_SDMA,
                   "context " + std::to_string(i) + " should be SDMA") &&
             ok;
        ok = Check(ctx.predCntInit == (hasPredecessor ? 1 : 0),
                   "context " + std::to_string(i) + " predCntInit mismatch") &&
             ok;
        ok = Check(ctx.predCnt == (hasPredecessor ? 1 : 0),
                   "context " + std::to_string(i) + " predCnt mismatch") &&
             ok;
        ok = Check(ctx.successorNum == (hasSuccessor ? 1 : 0),
                   "context " + std::to_string(i) + " successorNum mismatch") &&
             ok;
        if (hasSuccessor) {
            ok = Check(ctx.successorList[0] == expectedSuccessor,
                       "context " + std::to_string(i) + " successor mismatch") &&
                 ok;
        }
    }

    return ok;
}

}  // namespace

int main()
{
    if (!CheckEightLaneDependencyGraph()) { return EXIT_FAILURE; }
    std::cout << "FFTS dispatcher dependency graph test passed\n";
    return EXIT_SUCCESS;
}
