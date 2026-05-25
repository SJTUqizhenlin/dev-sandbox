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
#include <algorithm>
#include <cstdint>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>
#include <vector>
#include "copy_buffer_ascend.h"
#include "copy_case.h"
#include "copy_instance_ascend.h"
#include "copy_instance_ffts_ascend.h"
#include "copy_instance_ffts_host_direct_ascend.h"
#include "copy_instance_ffts_pipeline_ascend.h"

namespace {

bool FftsValidationEnabled()
{
    const char* value = std::getenv("COPY_FFTS_VALIDATE");
    if (value == nullptr) { return false; }
    return std::strcmp(value, "1") == 0 || std::strcmp(value, "true") == 0 ||
           std::strcmp(value, "TRUE") == 0 || std::strcmp(value, "on") == 0 ||
           std::strcmp(value, "ON") == 0;
}

size_t ReadFftsPipelineObjectFrags()
{
    constexpr size_t kDefaultObjectFrags = 8;
    const char* value = std::getenv("COPY_FFTS_PIPELINE_OBJECT_FRAGS");
    if (value == nullptr || value[0] == '\0') { return kDefaultObjectFrags; }

    char* end = nullptr;
    errno = 0;
    const auto parsed = std::strtoull(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0' || parsed == 0) {
        return kDefaultObjectFrags;
    }
    if (parsed > static_cast<unsigned long long>(std::numeric_limits<size_t>::max())) {
        return std::numeric_limits<size_t>::max();
    }
    return static_cast<size_t>(parsed);
}

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

void* HostAddress(const CopyBuffer& buffer, size_t index)
{
    if (const auto* mapped = dynamic_cast<const MallocHostRegisterCopyBuffer*>(&buffer)) {
        return mapped->HostAt(index);
    }
    if (const auto* mapped = dynamic_cast<const MallocHostRegisterV2CopyBuffer*>(&buffer)) {
        return mapped->HostAt(index);
    }
    return buffer[index];
}

void InitializeHostPatternedBuffer(const CopyBuffer& buffer)
{
    for (size_t i = 0; i < buffer.Number(); ++i) {
        const auto pattern = MakePattern(i, buffer.Size());
        std::memcpy(HostAddress(buffer, i), pattern.data(), pattern.size());
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
        std::memset(HostAddress(buffer, i), 0, buffer.Size());
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
        if (std::memcmp(HostAddress(buffer, i), expected.data(), expected.size()) != 0) {
            return false;
        }
    }
    return true;
}

void ValidateDeviceBufferIfEnabled(const CopyBuffer& buffer)
{
    if (FftsValidationEnabled()) { ASSERT(ValidatePatternedBuffer(buffer)); }
}

void ValidateHostBufferIfEnabled(const CopyBuffer& buffer)
{
    if (FftsValidationEnabled()) { ASSERT(ValidateHostPatternedBuffer(buffer)); }
}

void RunD2DFFTSPath(CopyResult* result, const CopyBuffer& srcBuffer, const CopyBuffer& dstBuffer,
                    const CopyCase::Context& ctx)
{
    ResetBuffer(dstBuffer);
    D2DFFTSCopyInstance instance{ctx.iter, false};
    result->Push(instance.DoCopy(&srcBuffer, &dstBuffer));
    ValidateDeviceBufferIfEnabled(dstBuffer);
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
        ResetBuffer(dstBuffer);

        H2DFFTSSplitCopyInstance instance{ctx.iter, false};
        result.Push(instance.DoCopy(&srcBuffer, &dstBuffer));
        ValidateDeviceBufferIfEnabled(dstBuffer);
    }
    result.Show("[[ " + Key() + " ]] " + Brief());
}

DEFINE_COPY_CASE(AscendH2DFFTSYuanrongPipelineCase, "ascend_h2d_ffts_yuanrong_pipeline",
                 "copy host buffers to fragmented device buffers with yuanrong-style h2d and ffts pipeline",
                 ctx)
{
    CopyResult result;
    const auto objectFrags = ReadFftsPipelineObjectFrags();
    const auto effectiveObjectFrags = ctx.num == 0 ? objectFrags : std::min(objectFrags, ctx.num);
    for (size_t device = 0; device < ctx.nDevice; device++) {
        HostCopyBuffer srcBuffer{device, ctx.size, ctx.num};
        FragmentedDeviceCopyBuffer dstBuffer{device, ctx.size, ctx.num};
        InitializeHostPatternedBuffer(srcBuffer);
        ResetBuffer(dstBuffer);

        H2DFFTSYuanrongPipelineCopyInstance instance{ctx.iter, false, objectFrags};
        result.Push(instance.DoCopy(&srcBuffer, &dstBuffer));
        ValidateDeviceBufferIfEnabled(dstBuffer);
    }
    result.Show("[[ " + Key() + " ]] " + Brief() +
                " [object_frags=" + std::to_string(effectiveObjectFrags) + "]");
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
        ValidateHostBufferIfEnabled(dstBuffer);
    }
    result.Show("[[ " + Key() + " ]] " + Brief());
}

DEFINE_COPY_CASE(AscendH2DFFTSDirectCase, "ascend_h2d_ffts_direct",
                 "copy host buffers directly to fragmented device buffers with ffts", ctx)
{
    CopyResult result;
    for (size_t device = 0; device < ctx.nDevice; device++) {
        HostCopyBuffer srcBuffer{device, ctx.size, ctx.num};
        FragmentedDeviceCopyBuffer dstBuffer{device, ctx.size, ctx.num};
        InitializeHostPatternedBuffer(srcBuffer);
        ResetBuffer(dstBuffer);

        H2DFFTSDirectCopyInstance instance{ctx.iter, false};
        result.Push(instance.DoCopy(&srcBuffer, &dstBuffer));
        ValidateDeviceBufferIfEnabled(dstBuffer);
    }
    result.Show("[[ " + Key() + " ]] " + Brief());
}

DEFINE_COPY_CASE(AscendFFTSDirectD2HCase, "ascend_ffts_d2h_direct",
                 "copy fragmented device buffers directly to host buffers with ffts", ctx)
{
    CopyResult result;
    for (size_t device = 0; device < ctx.nDevice; device++) {
        FragmentedDeviceCopyBuffer srcBuffer{device, ctx.size, ctx.num};
        HostCopyBuffer dstBuffer{device, ctx.size, ctx.num};
        InitializePatternedBuffer(srcBuffer);
        ResetHostBuffer(dstBuffer);

        FFTSDirectD2HCopyInstance instance{ctx.iter, true};
        result.Push(instance.DoCopy(&srcBuffer, &dstBuffer));
        ValidateHostBufferIfEnabled(dstBuffer);
    }
    result.Show("[[ " + Key() + " ]] " + Brief());
}

DEFINE_COPY_CASE(AscendRegH2DFFTSDirectCase, "ascend_reg_h2d_ffts_direct",
                 "copy registered host buffers directly to fragmented device buffers with ffts",
                 ctx)
{
    CopyResult result;
    for (size_t device = 0; device < ctx.nDevice; device++) {
        MallocHostRegisterCopyBuffer srcBuffer{device, ctx.size, ctx.num};
        FragmentedDeviceCopyBuffer dstBuffer{device, ctx.size, ctx.num};
        InitializeHostPatternedBuffer(srcBuffer);
        ResetBuffer(dstBuffer);

        H2DFFTSDirectCopyInstance instance{ctx.iter, false};
        result.Push(instance.DoCopy(&srcBuffer, &dstBuffer));
        ValidateDeviceBufferIfEnabled(dstBuffer);
    }
    result.Show("[[ " + Key() + " ]] " + Brief());
}

DEFINE_COPY_CASE(AscendFFTSDirectD2HRegCase, "ascend_ffts_d2h_reg_direct",
                 "copy fragmented device buffers directly to registered host buffers with ffts",
                 ctx)
{
    CopyResult result;
    for (size_t device = 0; device < ctx.nDevice; device++) {
        FragmentedDeviceCopyBuffer srcBuffer{device, ctx.size, ctx.num};
        MallocHostRegisterCopyBuffer dstBuffer{device, ctx.size, ctx.num};
        InitializePatternedBuffer(srcBuffer);
        ResetHostBuffer(dstBuffer);

        FFTSDirectD2HCopyInstance instance{ctx.iter, true};
        result.Push(instance.DoCopy(&srcBuffer, &dstBuffer));
        ValidateHostBufferIfEnabled(dstBuffer);
    }
    result.Show("[[ " + Key() + " ]] " + Brief());
}

DEFINE_COPY_CASE(AscendRegV2H2DFFTSDirectCase, "ascend_regv2_h2d_ffts_direct",
                 "copy v2 registered host buffers directly to fragmented device buffers with ffts",
                 ctx)
{
    CopyResult result;
    for (size_t device = 0; device < ctx.nDevice; device++) {
        MallocHostRegisterV2CopyBuffer srcBuffer{device, ctx.size, ctx.num};
        FragmentedDeviceCopyBuffer dstBuffer{device, ctx.size, ctx.num};
        InitializeHostPatternedBuffer(srcBuffer);
        ResetBuffer(dstBuffer);

        H2DFFTSDirectCopyInstance instance{ctx.iter, false};
        result.Push(instance.DoCopy(&srcBuffer, &dstBuffer));
        ValidateDeviceBufferIfEnabled(dstBuffer);
    }
    result.Show("[[ " + Key() + " ]] " + Brief());
}

DEFINE_COPY_CASE(AscendFFTSDirectD2HRegV2Case, "ascend_ffts_d2h_regv2_direct",
                 "copy fragmented device buffers directly to v2 registered host buffers with ffts",
                 ctx)
{
    CopyResult result;
    for (size_t device = 0; device < ctx.nDevice; device++) {
        FragmentedDeviceCopyBuffer srcBuffer{device, ctx.size, ctx.num};
        MallocHostRegisterV2CopyBuffer dstBuffer{device, ctx.size, ctx.num};
        InitializePatternedBuffer(srcBuffer);
        ResetHostBuffer(dstBuffer);

        FFTSDirectD2HCopyInstance instance{ctx.iter, true};
        result.Push(instance.DoCopy(&srcBuffer, &dstBuffer));
        ValidateHostBufferIfEnabled(dstBuffer);
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
