#include <acl/acl.h>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr int kDeviceId = 0;
constexpr int kAllDeviceCount = 8;
constexpr int kWarmupIterations = 5;
constexpr int kMeasureIterations = 50;
constexpr std::size_t kDefaultIoSize = 64 * 1024;
constexpr std::size_t kDefaultBufferCount = 1024;

struct Options {
    std::size_t ioSize = kDefaultIoSize;
    std::size_t bufferCount = kDefaultBufferCount;
    bool showHelp = false;
};

bool CheckAcl(aclError ret, const char* expr, const char* file, int line)
{
    if (ret == ACL_SUCCESS) {
        return true;
    }

    std::cerr << "ACL call failed: " << expr << "\n"
              << "  at " << file << ":" << line << "\n"
              << "  error code: " << ret << "\n";
    const char* msg = aclGetRecentErrMsg();
    if (msg != nullptr) {
        std::cerr << "  message: " << msg << "\n";
    }
    return false;
}

#define CHECK_ACL(expr) CheckAcl((expr), #expr, __FILE__, __LINE__)

double ElapsedUs(std::chrono::steady_clock::time_point begin,
                 std::chrono::steady_clock::time_point end)
{
    return std::chrono::duration<double, std::micro>(end - begin).count();
}

double BandwidthMBps(std::size_t bytes, double avgTotalUs)
{
    const double seconds = avgTotalUs / 1000000.0;
    const double megabytes = static_cast<double>(bytes) / (1024.0 * 1024.0);
    return megabytes / seconds;
}

std::string SizeLabel(std::size_t bytes)
{
    const std::size_t gb = 1024ull * 1024ull * 1024ull;
    const std::size_t mb = 1024 * 1024;
    const std::size_t kb = 1024;
    if (bytes % gb == 0) {
        return std::to_string(bytes / gb) + " GB";
    }
    if (bytes % mb == 0) {
        return std::to_string(bytes / mb) + " MB";
    }
    if (bytes % kb == 0) {
        return std::to_string(bytes / kb) + " KB";
    }
    return std::to_string(bytes) + " B";
}

void PrintUsage(const char* prog)
{
    std::cout << "Usage: " << (prog != nullptr ? prog : "h2d_async_memcpy")
              << " [-s <io_size>] [-n <buffer_count>]\n"
              << "\n"
              << "Options:\n"
              << "  -s <io_size>       Bytes per buffer. Suffixes K/M/G are supported."
              << " Default: " << SizeLabel(kDefaultIoSize) << "\n"
              << "  -n <buffer_count>  Number of buffers per measurement iteration."
              << " Default: " << kDefaultBufferCount << "\n"
              << "  -h                 Show this help message.\n";
}

bool ParseSize(const std::string& text, std::size_t* value)
{
    if (text.empty() || value == nullptr) {
        return false;
    }

    std::string number = text;
    std::size_t multiplier = 1;
    const char suffix = static_cast<char>(std::toupper(static_cast<unsigned char>(text.back())));
    if (suffix == 'K' || suffix == 'M' || suffix == 'G') {
        number.pop_back();
        if (suffix == 'K') {
            multiplier = 1024ull;
        } else if (suffix == 'M') {
            multiplier = 1024ull * 1024ull;
        } else {
            multiplier = 1024ull * 1024ull * 1024ull;
        }
    }
    if (number.empty()) {
        return false;
    }

    std::size_t parsed = 0;
    try {
        std::size_t pos = 0;
        parsed = std::stoull(number, &pos, 10);
        if (pos != number.size()) {
            return false;
        }
    } catch (...) {
        return false;
    }

    if (parsed == 0) {
        return false;
    }
    *value = parsed * multiplier;
    return true;
}

bool ParseArgs(int argc, char const* argv[], Options* options)
{
    if (options == nullptr) {
        return false;
    }
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "-h" || arg == "--help") {
            PrintUsage(argv[0]);
            options->showHelp = true;
            return false;
        }
        if (arg == "-s") {
            if (i + 1 >= argc || !ParseSize(argv[++i], &options->ioSize)) {
                std::cerr << "Invalid value for -s.\n";
                PrintUsage(argv[0]);
                return false;
            }
            continue;
        }
        if (arg == "-n") {
            if (i + 1 >= argc || !ParseSize(argv[++i], &options->bufferCount)) {
                std::cerr << "Invalid value for -n.\n";
                PrintUsage(argv[0]);
                return false;
            }
            continue;
        }

        std::cerr << "Unknown option: " << arg << "\n";
        PrintUsage(argv[0]);
        return false;
    }
    return true;
}

struct CopyBuffers {
    std::vector<void*> hostSrc;
    std::vector<void*> device;
};

struct Timing {
    double submitUs = 0.0;
    double copyUs = 0.0;
};

struct DeviceTimings {
    bool ok = false;
    std::vector<double> submitUs;
    std::vector<double> copyUs;
};

class Barrier {
public:
    explicit Barrier(int count) : threshold_(count), count_(count) {}

    void Wait()
    {
        std::unique_lock<std::mutex> lock(mutex_);
        const int generation = generation_;
        if (--count_ == 0) {
            generation_++;
            count_ = threshold_;
            cv_.notify_all();
            return;
        }
        cv_.wait(lock, [&] { return generation != generation_; });
    }

private:
    std::mutex mutex_;
    std::condition_variable cv_;
    int threshold_;
    int count_;
    int generation_ = 0;
};

double Average(const std::vector<double>& values)
{
    double sum = 0.0;
    for (double value : values) {
        sum += value;
    }
    return values.empty() ? 0.0 : sum / static_cast<double>(values.size());
}

void PrintTableHeader(const std::string& title, std::size_t bufferCount)
{
    std::cout << title << "\n"
              << "warmup=" << kWarmupIterations << ", iterations=" << kMeasureIterations
              << ", buffers_per_iteration=" << bufferCount
              << "\n\n";

    std::cout << std::left << std::setw(8) << "Dir" << std::right << std::setw(12) << "Size"
              << std::setw(8) << "Count" << std::setw(14) << "Submit(us)" << std::setw(14)
              << "Wait(us)" << std::setw(14) << "Copy(us)" << std::setw(16) << "BW(MB/s)"
              << "\n";
    std::cout << std::string(86, '-') << "\n";
}

void PrintTableRow(const std::string& direction, std::size_t bytes, std::size_t count,
                   double avgSubmitUs, double avgWaitUs, double avgCopyUs)
{
    std::cout << std::left << std::setw(8) << direction << std::right << std::setw(12)
              << SizeLabel(bytes) << std::setw(8) << count << std::fixed << std::setprecision(3)
              << std::setw(14) << avgSubmitUs << std::setw(14) << avgWaitUs << std::setw(14)
              << avgCopyUs << std::setprecision(2) << std::setw(16)
              << BandwidthMBps(bytes * count, avgCopyUs) << "\n";
}

void FillHostData(void* data, std::size_t size)
{
    unsigned char* p = static_cast<unsigned char*>(data);
    for (std::size_t i = 0; i < size; ++i) {
        p[i] = static_cast<unsigned char>(i & 0xff);
    }
}

bool MeasureCopyOnce(const std::vector<void*>& dst, const std::vector<void*>& src,
                     std::size_t size, aclrtMemcpyKind kind, aclrtStream stream, aclrtEvent start,
                     aclrtEvent end, Timing* timing)
{
    // The original benchmark records events around a batch of async copies.
    if (!CHECK_ACL(aclrtRecordEvent(start, stream))) {
        return false;
    }

    auto submitBegin = std::chrono::steady_clock::now();
    for (std::size_t i = 0; i < src.size(); ++i) {
        // dst: destination address; destMax: destination capacity; src: source address;
        // count: bytes to copy; kind: copy direction; stream: async execution queue.
        if (!CHECK_ACL(aclrtMemcpyAsync(dst[i], size, src[i], size, kind, stream))) {
            return false;
        }
    }
    auto submitEnd = std::chrono::steady_clock::now();

    if (!CHECK_ACL(aclrtRecordEvent(end, stream))) {
        return false;
    }
    if (!CHECK_ACL(aclrtSynchronizeStream(stream))) {
        return false;
    }

    float copyMs = 0.0f;
    if (!CHECK_ACL(aclrtEventElapsedTime(&copyMs, start, end))) {
        return false;
    }
    timing->submitUs = ElapsedUs(submitBegin, submitEnd);
    timing->copyUs = static_cast<double>(copyMs) * 1000.0;
    return true;
}

bool RunOneDirection(const std::string& direction, const std::vector<void*>& dst,
                     const std::vector<void*>& src, std::size_t size, aclrtMemcpyKind kind,
                     aclrtStream stream)
{
    aclrtEvent start = nullptr;
    aclrtEvent end = nullptr;
    if (!CHECK_ACL(aclrtCreateEvent(&start))) {
        return false;
    }
    if (!CHECK_ACL(aclrtCreateEvent(&end))) {
        aclrtDestroyEvent(start);
        return false;
    }

    bool ok = true;
    for (int i = 0; i < kWarmupIterations; ++i) {
        Timing timing;
        if (!MeasureCopyOnce(dst, src, size, kind, stream, start, end, &timing)) {
            ok = false;
            break;
        }
    }

    double submitUs = 0.0;
    double copyUs = 0.0;
    if (ok) {
        for (int i = 0; i < kMeasureIterations; ++i) {
            Timing timing;
            if (!MeasureCopyOnce(dst, src, size, kind, stream, start, end, &timing)) {
                ok = false;
                break;
            }
            submitUs += timing.submitUs;
            copyUs += timing.copyUs;
        }
    }

    if (start != nullptr) {
        ok = CHECK_ACL(aclrtDestroyEvent(start)) && ok;
    }
    if (end != nullptr) {
        ok = CHECK_ACL(aclrtDestroyEvent(end)) && ok;
    }

    if (!ok) {
        return false;
    }

    const double avgSubmitUs = submitUs / kMeasureIterations;
    const double avgCopyUs = copyUs / kMeasureIterations;
    const double avgWaitUs = avgCopyUs > avgSubmitUs ? avgCopyUs - avgSubmitUs : 0.0;

    PrintTableRow(direction, size, src.size(), avgSubmitUs, avgWaitUs, avgCopyUs);
    return ok;
}

bool AllocateBuffers(std::size_t size, std::size_t bufferCount, CopyBuffers* buffers)
{
    buffers->hostSrc.assign(bufferCount, nullptr);
    buffers->device.assign(bufferCount, nullptr);

    for (std::size_t i = 0; i < bufferCount; ++i) {
        // aclrtMallocHost allocates page-locked host memory suitable for DMA transfers.
        if (!CHECK_ACL(aclrtMallocHost(&buffers->hostSrc[i], size))) {
            return false;
        }

        // aclrtMalloc allocates memory on the current Ascend device.
        if (!CHECK_ACL(aclrtMalloc(&buffers->device[i], size, ACL_MEM_MALLOC_HUGE_FIRST))) {
            return false;
        }

        FillHostData(buffers->hostSrc[i], size);
    }
    return true;
}

bool FreeBuffers(CopyBuffers* buffers)
{
    bool ok = true;
    for (void* ptr : buffers->device) {
        if (ptr != nullptr) {
            aclError ret = aclrtFree(ptr);
            if (ret != ACL_SUCCESS) {
                std::cerr << "aclrtFree failed, error code: " << ret << "\n";
                ok = false;
            }
        }
    }
    for (void* ptr : buffers->hostSrc) {
        if (ptr != nullptr) {
            aclError ret = aclrtFreeHost(ptr);
            if (ret != ACL_SUCCESS) {
                std::cerr << "aclrtFreeHost(hostSrc) failed, error code: " << ret << "\n";
                ok = false;
            }
        }
    }
    return ok;
}

bool RunSize(std::size_t size, std::size_t bufferCount, aclrtStream stream)
{
    CopyBuffers buffers;
    bool ok = true;
    ok = AllocateBuffers(size, bufferCount, &buffers);

    // H2D: dst is device memory, destMax is device buffer size, src is host memory,
    // count is the bytes to copy, kind selects Host-to-Device, stream carries the async work.
    if (ok) {
        ok = RunOneDirection("H2D", buffers.device, buffers.hostSrc, size,
                              ACL_MEMCPY_HOST_TO_DEVICE, stream);
    }

    // aclrtFree releases device memory; aclrtFreeHost releases host memory.
    ok = FreeBuffers(&buffers) && ok;

    return ok;
}

bool RunSingleDevice(const Options& options)
{
    bool ok = true;
    aclrtStream stream = nullptr;

    if (!CHECK_ACL(aclrtSetDevice(kDeviceId))) {
        std::cerr << "aclrtSetDevice(" << kDeviceId << ") failed.\n";
        return false;
    }
    if (!CHECK_ACL(aclrtCreateStream(&stream))) {
        std::cerr << "aclrtCreateStream failed.\n";
        return false;
    }

    PrintTableHeader("AscendCL aclrtMemcpyAsync single-device H2D benchmark",
                     options.bufferCount);
    ok = RunSize(options.ioSize, options.bufferCount, stream);

    if (stream != nullptr) {
        aclError ret = aclrtDestroyStream(stream);
        if (ret != ACL_SUCCESS) {
            std::cerr << "aclrtDestroyStream failed, error code: " << ret << "\n";
            ok = false;
        }
    }
    return ok;
}

void RunAllDeviceWorker(int deviceId, const Options& options, Barrier* beginBarrier,
                        Barrier* endBarrier, DeviceTimings* result)
{
    if (beginBarrier == nullptr || endBarrier == nullptr || result == nullptr) {
        return;
    }

    bool ok = true;
    aclrtStream stream = nullptr;
    aclrtEvent start = nullptr;
    aclrtEvent end = nullptr;
    CopyBuffers buffers;

    ok = CHECK_ACL(aclrtSetDevice(deviceId));
    if (ok) {
        ok = CHECK_ACL(aclrtCreateStream(&stream));
    }
    if (ok) {
        ok = CHECK_ACL(aclrtCreateEvent(&start));
    }
    if (ok) {
        ok = CHECK_ACL(aclrtCreateEvent(&end));
    }
    if (ok) {
        ok = AllocateBuffers(options.ioSize, options.bufferCount, &buffers);
    }

    for (int i = 0; i < kWarmupIterations; ++i) {
        beginBarrier->Wait();
        if (ok) {
            Timing timing;
            ok = MeasureCopyOnce(buffers.device, buffers.hostSrc, options.ioSize,
                                 ACL_MEMCPY_HOST_TO_DEVICE, stream, start, end, &timing);
        }
        endBarrier->Wait();
    }

    result->submitUs.assign(kMeasureIterations, 0.0);
    result->copyUs.assign(kMeasureIterations, 0.0);
    for (int i = 0; i < kMeasureIterations; ++i) {
        beginBarrier->Wait();
        if (ok) {
            Timing timing;
            ok = MeasureCopyOnce(buffers.device, buffers.hostSrc, options.ioSize,
                                 ACL_MEMCPY_HOST_TO_DEVICE, stream, start, end, &timing);
            result->submitUs[i] = timing.submitUs;
            result->copyUs[i] = timing.copyUs;
        }
        endBarrier->Wait();
    }

    if (ok) {
        result->ok = true;
    }

    ok = FreeBuffers(&buffers) && ok;
    if (end != nullptr) {
        ok = CHECK_ACL(aclrtDestroyEvent(end)) && ok;
    }
    if (start != nullptr) {
        ok = CHECK_ACL(aclrtDestroyEvent(start)) && ok;
    }
    if (stream != nullptr) {
        ok = CHECK_ACL(aclrtDestroyStream(stream)) && ok;
    }
    result->ok = result->ok && ok;
}

bool RunAllDevices(const Options& options)
{
    Barrier beginBarrier(kAllDeviceCount);
    Barrier endBarrier(kAllDeviceCount);
    std::vector<DeviceTimings> results(kAllDeviceCount);
    std::vector<std::thread> threads;
    threads.reserve(kAllDeviceCount);

    for (int device = 0; device < kAllDeviceCount; ++device) {
        threads.emplace_back(RunAllDeviceWorker, device, std::cref(options), &beginBarrier,
                             &endBarrier, &results[device]);
    }
    for (auto& thread : threads) {
        thread.join();
    }

    bool ok = true;
    for (int device = 0; device < kAllDeviceCount; ++device) {
        if (!results[device].ok) {
            std::cerr << "device " << device << " failed during all-device H2D test.\n";
            ok = false;
        }
    }
    if (!ok) {
        return false;
    }

    std::vector<double> maxSubmitUs(kMeasureIterations, 0.0);
    std::vector<double> maxCopyUs(kMeasureIterations, 0.0);
    for (int iter = 0; iter < kMeasureIterations; ++iter) {
        for (const auto& result : results) {
            maxSubmitUs[iter] = std::max(maxSubmitUs[iter], result.submitUs[iter]);
            maxCopyUs[iter] = std::max(maxCopyUs[iter], result.copyUs[iter]);
        }
    }

    const double avgSubmitUs = Average(maxSubmitUs);
    const double avgCopyUs = Average(maxCopyUs);
    const double avgWaitUs = avgCopyUs > avgSubmitUs ? avgCopyUs - avgSubmitUs : 0.0;

    PrintTableHeader("AscendCL aclrtMemcpyAsync 8-device simultaneous H2D benchmark",
                     options.bufferCount);
    PrintTableRow("H2D_ALL8", options.ioSize, options.bufferCount * kAllDeviceCount,
                  avgSubmitUs, avgWaitUs, avgCopyUs);
    return true;
}

}  // namespace

int main(int argc, char const* argv[])
{
    Options options;
    if (!ParseArgs(argc, argv, &options)) {
        return options.showHelp ? 0 : 1;
    }

    // aclInit initializes AscendCL runtime state for this process.
    if (!CHECK_ACL(aclInit(nullptr))) {
        std::cerr << "aclInit failed. Check that Ascend runtime is installed and configured.\n";
        return 1;
    }

    bool ok = true;

    // aclrtSetDevice selects which Ascend device subsequent runtime calls use.
    if (ok) {
        ok = RunSingleDevice(options);
    }

    if (ok) {
        ok = RunAllDevices(options);
    }

    for (int device = 0; device < kAllDeviceCount; ++device) {
        if (CHECK_ACL(aclrtSetDevice(device))) {
            ok = CHECK_ACL(aclrtResetDevice(device)) && ok;
        } else {
            ok = false;
        }
    }

    aclError finalizeRet = aclFinalize();
    if (!CHECK_ACL(finalizeRet)) {
        ok = false;
    }

    return ok ? 0 : 1;
}
