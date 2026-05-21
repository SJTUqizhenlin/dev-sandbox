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
#ifndef COPY_INSTANCE_FFTS_ASCEND_H
#define COPY_INSTANCE_FFTS_ASCEND_H

#include <vector>
#include "copy_instance_ascend.h"
#include "ffts_d2d_dispatcher_ascend.h"

class D2DFFTSCopyInstance : public AscendCopyInstanceBase {
protected:
    void CopyInternal(const AscendStreamContext& ctx) override
    {
        std::vector<AscendD2DCopySpec> copies;
        copies.reserve(ctx.src.size());
        for (size_t i = 0; i < ctx.src.size(); ++i) {
            copies.push_back({ctx.dst[i], ctx.src[i], ctx.size});
        }

        FftsD2DDispatcher dispatcher;
        const auto readyCount = dispatcher.BuildCopies(copies);
        ASSERT(readyCount > 0);
        dispatcher.Launch(ctx.stream, readyCount);
    }

    void SynchronizeInternal(const AscendStreamContext& ctx) override
    {
        ASCEND_ASSERT(aclrtSynchronizeStream(ctx.stream));
    }

public:
    D2DFFTSCopyInstance(size_t iterations, bool affinitySrc)
        : AscendCopyInstanceBase(iterations, affinitySrc)
    {
    }

    std::string Name() const override { return "FFTS"; }
};

#endif  // COPY_INSTANCE_FFTS_ASCEND_H
