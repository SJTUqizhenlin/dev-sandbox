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
#ifndef COPY_INSTANCE_FFTS_PIPELINE_ASCEND_H
#define COPY_INSTANCE_FFTS_PIPELINE_ASCEND_H

#include <chrono>
#include <memory>
#include <utility>
#include <vector>
#include "copy_buffer_ascend.h"
#include "copy_instance.h"
#include "error_handle_ascend.h"
#include "ffts_d2d_dispatcher_ascend.h"

class FftsPipelineCopyInstanceBase : public CopyInstance {
protected:
    size_t deviceId_ = 0;
    size_t size_ = 0;
    size_t number_ = 0;
    aclrtStream stream_ = nullptr;
    aclrtEvent totalStart_ = nullptr;
    aclrtEvent totalEnd_ = nullptr;
    std::unique_ptr<DeviceCopyBuffer> transferBuffer_;
    std::vector<AscendD2DCopySpec> fftsCopies_;
    FftsD2DDispatcher dispatcher_;

    FftsPipelineCopyInstanceBase(size_t iterations, bool affinitySrc)
        : CopyInstance(iterations, affinitySrc)
    {
    }

    void PrepareCommon(const CopyBuffer& src, const CopyBuffer& dst)
    {
        ASSERT(src.Number() == dst.Number());
        ASSERT(src.Size() == dst.Size());

        deviceId_ = AffinityDeviceId(src, dst);
        size_ = src.Size();
        number_ = src.Number();
        transferBuffer_ = std::make_unique<DeviceCopyBuffer>(deviceId_, size_, number_);

        ASCEND_ASSERT(aclrtSetDevice(deviceId_));
        ASCEND_ASSERT(aclrtCreateStream(&stream_));
        ASCEND_ASSERT(aclrtCreateEvent(&totalStart_));
        ASCEND_ASSERT(aclrtCreateEvent(&totalEnd_));
    }

    void Cleanup() override
    {
        ASCEND_ASSERT(aclrtSetDevice(deviceId_));
        if (totalStart_ != nullptr) {
            ASCEND_ASSERT(aclrtDestroyEvent(totalStart_));
            totalStart_ = nullptr;
        }
        if (totalEnd_ != nullptr) {
            ASCEND_ASSERT(aclrtDestroyEvent(totalEnd_));
            totalEnd_ = nullptr;
        }
        if (stream_ != nullptr) {
            ASCEND_ASSERT(aclrtDestroyStream(stream_));
            stream_ = nullptr;
        }
        fftsCopies_.clear();
        transferBuffer_.reset();
        size_ = 0;
        number_ = 0;
    }

    void SubmitFftsCopies()
    {
        const auto readyCount = dispatcher_.BuildCopies(fftsCopies_);
        ASSERT(readyCount > 0);
        dispatcher_.Launch(stream_, readyCount);
    }

    template <typename SubmitFunc>
    std::pair<size_t, size_t> MeasurePipeline(SubmitFunc submitFunc)
    {
        using namespace std::chrono;

        ASCEND_ASSERT(aclrtSetDevice(deviceId_));
        ASCEND_ASSERT(aclrtRecordEvent(totalStart_, stream_));

        const auto submitStart = steady_clock::now();
        submitFunc();
        const auto submitCost =
            static_cast<size_t>(duration_cast<microseconds>(steady_clock::now() - submitStart)
                                    .count());

        ASCEND_ASSERT(aclrtRecordEvent(totalEnd_, stream_));
        ASCEND_ASSERT(aclrtSynchronizeStream(stream_));

        float copyCostMs = 0.0f;
        ASCEND_ASSERT(aclrtEventElapsedTime(&copyCostMs, totalStart_, totalEnd_));
        const auto copyCost = static_cast<size_t>(copyCostMs * 1000);
        return {copyCost, submitCost};
    }
};

class H2DFFTSSplitCopyInstance : public FftsPipelineCopyInstanceBase {
protected:
    std::vector<void*> hostSrc_;
    std::vector<void*> transferDst_;

    void Prepare(const std::vector<const CopyBuffer*>& srcBuffers,
                 const std::vector<const CopyBuffer*>& dstBuffers) override
    {
        ASSERT(srcBuffers.size() == 1);
        ASSERT(dstBuffers.size() == 1);
        const auto& src = *srcBuffers.front();
        const auto& dst = *dstBuffers.front();
        PrepareCommon(src, dst);

        hostSrc_.reserve(number_);
        transferDst_.reserve(number_);
        fftsCopies_.reserve(number_);
        for (size_t i = 0; i < number_; ++i) {
            auto* transfer = (*transferBuffer_)[i];
            hostSrc_.push_back(src[i]);
            transferDst_.push_back(transfer);
            fftsCopies_.push_back({dst[i], transfer, size_});
        }
    }

    void Cleanup() override
    {
        hostSrc_.clear();
        transferDst_.clear();
        FftsPipelineCopyInstanceBase::Cleanup();
    }

    std::pair<size_t, size_t> DoCopyOnce() override
    {
        return MeasurePipeline([&]() {
            for (size_t i = 0; i < number_; ++i) {
                ASCEND_ASSERT(aclrtMemcpyAsync(transferDst_[i], size_, hostSrc_[i], size_,
                                               ACL_MEMCPY_HOST_TO_DEVICE, stream_));
            }
            SubmitFftsCopies();
        });
    }

public:
    H2DFFTSSplitCopyInstance(size_t iterations, bool affinitySrc)
        : FftsPipelineCopyInstanceBase(iterations, affinitySrc)
    {
    }

    std::string Name() const override { return "H2D+FFTS"; }
};

class FFTSMergeD2HCopyInstance : public FftsPipelineCopyInstanceBase {
protected:
    std::vector<void*> transferSrc_;
    std::vector<void*> hostDst_;

    void Prepare(const std::vector<const CopyBuffer*>& srcBuffers,
                 const std::vector<const CopyBuffer*>& dstBuffers) override
    {
        ASSERT(srcBuffers.size() == 1);
        ASSERT(dstBuffers.size() == 1);
        const auto& src = *srcBuffers.front();
        const auto& dst = *dstBuffers.front();
        PrepareCommon(src, dst);

        transferSrc_.reserve(number_);
        hostDst_.reserve(number_);
        fftsCopies_.reserve(number_);
        for (size_t i = 0; i < number_; ++i) {
            auto* transfer = (*transferBuffer_)[i];
            fftsCopies_.push_back({transfer, src[i], size_});
            transferSrc_.push_back(transfer);
            hostDst_.push_back(dst[i]);
        }
    }

    void Cleanup() override
    {
        transferSrc_.clear();
        hostDst_.clear();
        FftsPipelineCopyInstanceBase::Cleanup();
    }

    std::pair<size_t, size_t> DoCopyOnce() override
    {
        return MeasurePipeline([&]() {
            SubmitFftsCopies();
            for (size_t i = 0; i < number_; ++i) {
                ASCEND_ASSERT(aclrtMemcpyAsync(hostDst_[i], size_, transferSrc_[i], size_,
                                               ACL_MEMCPY_DEVICE_TO_HOST, stream_));
            }
        });
    }

public:
    FFTSMergeD2HCopyInstance(size_t iterations, bool affinitySrc)
        : FftsPipelineCopyInstanceBase(iterations, affinitySrc)
    {
    }

    std::string Name() const override { return "FFTS+D2H"; }
};

#endif  // COPY_INSTANCE_FFTS_PIPELINE_ASCEND_H
