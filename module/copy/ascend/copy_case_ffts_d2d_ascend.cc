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
#include <cstring>
#include <limits>
#include <vector>
#include "copy_buffer_ascend.h"
#include "copy_case.h"
#include "copy_instance_ascend.h"
#include "copy_instance_ffts_ascend.h"
#include "copy_instance_ffts_pipeline_ascend.h"

namespace {

std::vector<uint8_t> MakePattern(size_t fragmentIndex, size_t size)
{
    std::vector<uint8_t> data(size);
    for (size_t i = 0; i < size; ++i) {
        data[i] = static_cast<uint8_t>((fragmentIndex * 17 + i * 31 + (i >> 8)) & 0xFF);
    }
    return data;
}

void CopyHostToDevice(const CopyBuffer& buffer, size_t index, const std::vector<uint8_t>& pattern)
{
    ASCEND_ASSERT(aclrtSetDevice(buffer.Device()));
    ASCEND_ASSERT(aclrtMemcpy(buffer[index], buffer.Size(), pattern.data(), pattern.size(),
                              ACL_MEMCPY_HOST_TO_DEVICE));
}

std::vector<uint8_t> CopyDeviceToHost(const CopyBuffer& buffer, size_t index)
{
    std::vector<uint8_t> data(buffer.Size());
    ASCEND_ASSERT(aclrtSetDevice(buffer.Device()));
    ASCEND_ASSERT(aclrtMemcpy(data.data(), data.size(), buffer[index], buffer.Size(),
                              ACL_MEMCPY_DEVICE_TO_HOST));
    return data;
}

void InitializePatternedBuffer(const CopyBuffer& buffer)
{
    for (size_t i = 0; i < buffer.Number(); ++i) {
        CopyHostToDevice(buffer, i, MakePattern(i, buffer.Size()));
    }
}

void InitializeHostPatternedBuffer(const CopyBuffer& buffer)
{
    for (size_t i = 0; i < buffer.Number(); ++i) {
        const auto pattern = MakePattern(i, buffer.Size());
        std::memcpy(buffer[i], pattern.data(), pattern.size());
    }
}

void ResetBuffer(const CopyBuffer& buffer)
{
    ASCEND_ASSERT(aclrtSetDevice(buffer.Device()));
    for (size_t i = 0; i < buffer.Number(); ++i) {
        ASCEND_ASSERT(aclrtMemset(buffer[i], buffer.Size(), 0, buffer.Size()));
    }
}

void ResetHostBuffer(const CopyBuffer& buffer)
{
    for (size_t i = 0; i < buffer.Number(); ++i) {
        std::memset(buffer[i], 0, buffer.Size());
    }
}

bool ValidatePatternedBuffer(const CopyBuffer& buffer)
{
    for (size_t i = 0; i < buffer.Number(); ++i) {
        auto actual = CopyDeviceToHost(buffer, i);
        auto expected = MakePattern(i, buffer.Size());
        if (actual.size() != expected.size() ||
            std::memcmp(actual.data(), expected.data(), expected.size()) != 0) {
            return false;
        }
    }
    return true;
}

bool ValidateHostPatternedBuffer(const CopyBuffer& buffer)
{
    for (size_t i = 0; i < buffer.Number(); ++i) {
        auto expected = MakePattern(i, buffer.Size());
        if (std::memcmp(buffer[i], expected.data(), expected.size()) != 0) { return false; }
    }
    return true;
}

size_t TotalBytes(const CopyBuffer& buffer)
{
    ASSERT(buffer.Number() > 0);
    ASSERT(buffer.Size() <= std::numeric_limits<size_t>::max() / buffer.Number());
    return buffer.Size() * buffer.Number();
}

void ValidateH2DFFTSSplitPreflight(const CopyBuffer& srcBuffer, const CopyBuffer& dstBuffer)
{
    ASSERT(srcBuffer.Size() == dstBuffer.Size());
    ASSERT(srcBuffer.Number() == dstBuffer.Number());

    DeviceCopyBuffer transferBuffer{dstBuffer.Device(), dstBuffer.Size(), dstBuffer.Number()};
    ResetBuffer(transferBuffer);
    ResetBuffer(dstBuffer);

    std::vector<AscendD2DCopySpec> copies;
    copies.reserve(dstBuffer.Number());
    for (size_t i = 0; i < dstBuffer.Number(); ++i) {
        copies.push_back({dstBuffer[i], transferBuffer[i], dstBuffer.Size()});
    }

    aclrtStream stream = nullptr;
    FftsD2DDispatcher dispatcher;
    ASCEND_ASSERT(aclrtSetDevice(dstBuffer.Device()));
    ASCEND_ASSERT(aclrtCreateStream(&stream));
    ASCEND_ASSERT(aclrtMemcpyAsync(transferBuffer[0], TotalBytes(transferBuffer), srcBuffer[0],
                                   TotalBytes(srcBuffer), ACL_MEMCPY_HOST_TO_DEVICE, stream));
    const auto readyCount = dispatcher.BuildCopies(copies);
    ASSERT(readyCount > 0);
    dispatcher.Launch(stream, readyCount);
    ASCEND_ASSERT(aclrtSynchronizeStream(stream));
    ASCEND_ASSERT(aclrtDestroyStream(stream));

    ASSERT(ValidatePatternedBuffer(dstBuffer));
}

void RunD2DFFTSPath(CopyResult* result, const CopyBuffer& srcBuffer, const CopyBuffer& dstBuffer,
                    const CopyCase::Context& ctx)
{
    ResetBuffer(dstBuffer);
    D2DFFTSCopyInstance instance{ctx.iter, false};
    result->Push(instance.DoCopy(&srcBuffer, &dstBuffer));
    ASSERT(ValidatePatternedBuffer(dstBuffer));
}

}  // namespace

DEFINE_COPY_CASE(AscendD2DMergeFftsCase, "ascend_d2d_merge_ffts",
                 "merge fragmented device buffers into one device buffer with ffts", ctx)
{
    CopyResult result;
    for (size_t device = 0; device < ctx.nDevice; device++) {
        FragmentedDeviceCopyBuffer srcBuffer{device, ctx.size, ctx.num};
        DeviceCopyBuffer dstBuffer{device, ctx.size, ctx.num};
        InitializePatternedBuffer(srcBuffer);

        RunD2DFFTSPath(&result, srcBuffer, dstBuffer, ctx);
    }
    result.Show("[[ " + Key() + " ]] " + Brief());
}

DEFINE_COPY_CASE(AscendH2DFFTSSplitCase, "ascend_h2d_ffts_split",
                 "copy host buffers to fragmented device buffers with h2d and ffts split", ctx)
{
    CopyResult result;
    for (size_t device = 0; device < ctx.nDevice; device++) {
        HostCopyBuffer srcBuffer{device, ctx.size, ctx.num};
        FragmentedDeviceCopyBuffer dstBuffer{device, ctx.size, ctx.num};
        InitializeHostPatternedBuffer(srcBuffer);
        ValidateH2DFFTSSplitPreflight(srcBuffer, dstBuffer);
        ResetBuffer(dstBuffer);

        H2DFFTSSplitCopyInstance instance{ctx.iter, false};
        result.Push(instance.DoCopy(&srcBuffer, &dstBuffer));
        ASSERT(ValidatePatternedBuffer(dstBuffer));
    }
    result.Show("[[ " + Key() + " ]] " + Brief());
}

DEFINE_COPY_CASE(AscendFFTSMergeD2HCase, "ascend_ffts_merge_d2h",
                 "copy fragmented device buffers to host buffers with ffts merge and d2h", ctx)
{
    CopyResult result;
    for (size_t device = 0; device < ctx.nDevice; device++) {
        FragmentedDeviceCopyBuffer srcBuffer{device, ctx.size, ctx.num};
        HostCopyBuffer dstBuffer{device, ctx.size, ctx.num};
        InitializePatternedBuffer(srcBuffer);
        ResetHostBuffer(dstBuffer);

        FFTSMergeD2HCopyInstance instance{ctx.iter, true};
        result.Push(instance.DoCopy(&srcBuffer, &dstBuffer));
        ASSERT(ValidateHostPatternedBuffer(dstBuffer));
    }
    result.Show("[[ " + Key() + " ]] " + Brief());
}

DEFINE_COPY_CASE(AscendD2DSplitFftsCase, "ascend_d2d_split_ffts",
                 "split one device buffer into fragmented device buffers with ffts", ctx)
{
    CopyResult result;
    for (size_t device = 0; device < ctx.nDevice; device++) {
        DeviceCopyBuffer srcBuffer{device, ctx.size, ctx.num};
        FragmentedDeviceCopyBuffer dstBuffer{device, ctx.size, ctx.num};
        InitializePatternedBuffer(srcBuffer);

        RunD2DFFTSPath(&result, srcBuffer, dstBuffer, ctx);
    }
    result.Show("[[ " + Key() + " ]] " + Brief());
}
