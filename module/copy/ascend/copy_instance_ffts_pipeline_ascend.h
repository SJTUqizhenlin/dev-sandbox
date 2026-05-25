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

#include <algorithm>
#include <array>
#include <chrono>
#include <limits>
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
    size_t totalBytes_ = 0;
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
        ASSERT(number_ > 0);
        ASSERT(size_ <= std::numeric_limits<size_t>::max() / number_);
        totalBytes_ = size_ * number_;
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
        totalBytes_ = 0;
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
    void* hostBase_ = nullptr;
    void* transferBase_ = nullptr;

    void Prepare(const std::vector<const CopyBuffer*>& srcBuffers,
                 const std::vector<const CopyBuffer*>& dstBuffers) override
    {
        ASSERT(srcBuffers.size() == 1);
        ASSERT(dstBuffers.size() == 1);
        const auto& src = *srcBuffers.front();
        const auto& dst = *dstBuffers.front();
        PrepareCommon(src, dst);

        hostBase_ = src[0];
        transferBase_ = (*transferBuffer_)[0];
        fftsCopies_.reserve(number_);
        for (size_t i = 0; i < number_; ++i) {
            auto* transfer = (*transferBuffer_)[i];
            fftsCopies_.push_back({dst[i], transfer, size_});
        }
    }

    void Cleanup() override
    {
        hostBase_ = nullptr;
        transferBase_ = nullptr;
        FftsPipelineCopyInstanceBase::Cleanup();
    }

    std::pair<size_t, size_t> DoCopyOnce() override
    {
        return MeasurePipeline([&]() {
            ASCEND_ASSERT(aclrtMemcpyAsync(transferBase_, totalBytes_, hostBase_, totalBytes_,
                                           ACL_MEMCPY_HOST_TO_DEVICE, stream_));
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

class H2DFFTSYuanrongPipelineCopyInstance : public CopyInstance {
protected:
    struct PipelineObjectRange {
        size_t firstFragment;
        size_t fragmentCount;
        size_t bytes;
    };

    static constexpr size_t kPipelineDepth = 2;

    const CopyBuffer* src_ = nullptr;
    const CopyBuffer* dst_ = nullptr;
    size_t configuredObjectFrags_ = 1;
    size_t objectFrags_ = 1;
    size_t deviceId_ = 0;
    size_t size_ = 0;
    size_t number_ = 0;
    size_t maxObjectBytes_ = 0;
    aclrtStream h2dStream_ = nullptr;
    aclrtStream fftsStream_ = nullptr;
    aclrtEvent totalStart_ = nullptr;
    aclrtEvent totalEnd_ = nullptr;
    std::array<aclrtEvent, kPipelineDepth> slotReady_{};
    std::array<aclrtEvent, kPipelineDepth> slotFree_{};
    std::array<std::unique_ptr<DeviceCopyBuffer>, kPipelineDepth> transferBuffers_;
    std::vector<PipelineObjectRange> objects_;
    std::vector<AscendD2DCopySpec> objectCopies_;
    std::vector<FftsD2DDispatcher> objectDispatchers_;

    void Prepare(const std::vector<const CopyBuffer*>& srcBuffers,
                 const std::vector<const CopyBuffer*>& dstBuffers) override
    {
        ASSERT(srcBuffers.size() == 1);
        ASSERT(dstBuffers.size() == 1);

        src_ = srcBuffers.front();
        dst_ = dstBuffers.front();
        ASSERT(src_ != nullptr);
        ASSERT(dst_ != nullptr);
        ASSERT(src_->Number() == dst_->Number());
        ASSERT(src_->Size() == dst_->Size());

        deviceId_ = AffinityDeviceId(*src_, *dst_);
        size_ = src_->Size();
        number_ = src_->Number();
        ASSERT(number_ > 0);
        ASSERT(size_ <= std::numeric_limits<size_t>::max() / number_);

        objectFrags_ = std::min(configuredObjectFrags_, number_);
        ASSERT(objectFrags_ > 0);
        ASSERT(size_ <= std::numeric_limits<size_t>::max() / objectFrags_);
        maxObjectBytes_ = size_ * objectFrags_;

        BuildObjectRanges();
        objectCopies_.reserve(objectFrags_);
        objectDispatchers_.resize(objects_.size());

        ASCEND_ASSERT(aclrtSetDevice(deviceId_));
        ASCEND_ASSERT(aclrtCreateStream(&h2dStream_));
        ASCEND_ASSERT(aclrtCreateStream(&fftsStream_));
        ASCEND_ASSERT(aclrtCreateEvent(&totalStart_));
        ASCEND_ASSERT(aclrtCreateEvent(&totalEnd_));

        for (size_t slot = 0; slot < kPipelineDepth; ++slot) {
            transferBuffers_[slot] =
                std::make_unique<DeviceCopyBuffer>(deviceId_, size_, objectFrags_);
            ASCEND_ASSERT(aclrtCreateEvent(&slotReady_[slot]));
            ASCEND_ASSERT(aclrtCreateEvent(&slotFree_[slot]));
            ASCEND_ASSERT(aclrtRecordEvent(slotFree_[slot], h2dStream_));
        }
    }

    void Cleanup() override
    {
        ASCEND_ASSERT(aclrtSetDevice(deviceId_));

        for (auto& event : slotReady_) {
            if (event != nullptr) {
                ASCEND_ASSERT(aclrtDestroyEvent(event));
                event = nullptr;
            }
        }
        for (auto& event : slotFree_) {
            if (event != nullptr) {
                ASCEND_ASSERT(aclrtDestroyEvent(event));
                event = nullptr;
            }
        }
        if (totalStart_ != nullptr) {
            ASCEND_ASSERT(aclrtDestroyEvent(totalStart_));
            totalStart_ = nullptr;
        }
        if (totalEnd_ != nullptr) {
            ASCEND_ASSERT(aclrtDestroyEvent(totalEnd_));
            totalEnd_ = nullptr;
        }
        if (h2dStream_ != nullptr) {
            ASCEND_ASSERT(aclrtDestroyStream(h2dStream_));
            h2dStream_ = nullptr;
        }
        if (fftsStream_ != nullptr) {
            ASCEND_ASSERT(aclrtDestroyStream(fftsStream_));
            fftsStream_ = nullptr;
        }

        for (auto& buffer : transferBuffers_) { buffer.reset(); }
        objectDispatchers_.clear();
        objectCopies_.clear();
        objects_.clear();
        src_ = nullptr;
        dst_ = nullptr;
        size_ = 0;
        number_ = 0;
        maxObjectBytes_ = 0;
    }

    std::pair<size_t, size_t> DoCopyOnce() override
    {
        using namespace std::chrono;

        ASCEND_ASSERT(aclrtSetDevice(deviceId_));
        ASCEND_ASSERT(aclrtRecordEvent(totalStart_, h2dStream_));
        ASCEND_ASSERT(aclrtStreamWaitEvent(fftsStream_, totalStart_));

        const auto submitStart = steady_clock::now();
        for (size_t objectIndex = 0; objectIndex < objects_.size(); ++objectIndex) {
            SubmitObject(objectIndex, objects_[objectIndex]);
        }
        const auto submitCost =
            static_cast<size_t>(duration_cast<microseconds>(steady_clock::now() - submitStart)
                                    .count());

        for (size_t slot = 0; slot < kPipelineDepth; ++slot) {
            ASCEND_ASSERT(aclrtStreamWaitEvent(h2dStream_, slotFree_[slot]));
        }
        ASCEND_ASSERT(aclrtRecordEvent(totalEnd_, h2dStream_));
        ASCEND_ASSERT(aclrtSynchronizeStream(h2dStream_));

        float copyCostMs = 0.0f;
        ASCEND_ASSERT(aclrtEventElapsedTime(&copyCostMs, totalStart_, totalEnd_));
        const auto copyCost = static_cast<size_t>(copyCostMs * 1000);
        return {copyCost, submitCost};
    }

    void BuildObjectRanges()
    {
        objects_.clear();
        for (size_t first = 0; first < number_; first += objectFrags_) {
            const auto fragmentCount = std::min(objectFrags_, number_ - first);
            objects_.push_back({first, fragmentCount, fragmentCount * size_});
        }
    }

    void SubmitObject(size_t objectIndex, const PipelineObjectRange& object)
    {
        const size_t slot = objectIndex % kPipelineDepth;
        auto& transfer = *transferBuffers_[slot];
        auto* transferBase = transfer[0];

        ASCEND_ASSERT(aclrtStreamWaitEvent(h2dStream_, slotFree_[slot]));
        ASCEND_ASSERT(aclrtMemcpyAsync(transferBase, maxObjectBytes_, (*src_)[object.firstFragment],
                                       object.bytes, ACL_MEMCPY_HOST_TO_DEVICE, h2dStream_));
        ASCEND_ASSERT(aclrtRecordEvent(slotReady_[slot], h2dStream_));

        ASCEND_ASSERT(aclrtStreamWaitEvent(fftsStream_, slotReady_[slot]));
        BuildObjectCopies(slot, object);

        auto& dispatcher = objectDispatchers_[objectIndex];
        const auto readyCount = dispatcher.BuildCopies(objectCopies_);
        ASSERT(readyCount > 0);
        dispatcher.Launch(fftsStream_, readyCount);

        ASCEND_ASSERT(aclrtRecordEvent(slotFree_[slot], fftsStream_));
    }

    void BuildObjectCopies(size_t slot, const PipelineObjectRange& object)
    {
        auto& transfer = *transferBuffers_[slot];
        objectCopies_.clear();
        objectCopies_.reserve(object.fragmentCount);
        for (size_t i = 0; i < object.fragmentCount; ++i) {
            objectCopies_.push_back({(*dst_)[object.firstFragment + i], transfer[i], size_});
        }
    }

public:
    H2DFFTSYuanrongPipelineCopyInstance(size_t iterations, bool affinitySrc,
                                        size_t objectFrags)
        : CopyInstance(iterations, affinitySrc),
          configuredObjectFrags_(objectFrags == 0 ? 1 : objectFrags)
    {
    }

    std::string Name() const override { return "H2D+YPipe"; }
};

class FFTSMergeD2HCopyInstance : public FftsPipelineCopyInstanceBase {
protected:
    void* transferBase_ = nullptr;
    void* hostBase_ = nullptr;

    void Prepare(const std::vector<const CopyBuffer*>& srcBuffers,
                 const std::vector<const CopyBuffer*>& dstBuffers) override
    {
        ASSERT(srcBuffers.size() == 1);
        ASSERT(dstBuffers.size() == 1);
        const auto& src = *srcBuffers.front();
        const auto& dst = *dstBuffers.front();
        PrepareCommon(src, dst);

        transferBase_ = (*transferBuffer_)[0];
        hostBase_ = dst[0];
        fftsCopies_.reserve(number_);
        for (size_t i = 0; i < number_; ++i) {
            auto* transfer = (*transferBuffer_)[i];
            fftsCopies_.push_back({transfer, src[i], size_});
        }
    }

    void Cleanup() override
    {
        transferBase_ = nullptr;
        hostBase_ = nullptr;
        FftsPipelineCopyInstanceBase::Cleanup();
    }

    std::pair<size_t, size_t> DoCopyOnce() override
    {
        return MeasurePipeline([&]() {
            SubmitFftsCopies();
            ASCEND_ASSERT(aclrtMemcpyAsync(hostBase_, totalBytes_, transferBase_, totalBytes_,
                                           ACL_MEMCPY_DEVICE_TO_HOST, stream_));
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
