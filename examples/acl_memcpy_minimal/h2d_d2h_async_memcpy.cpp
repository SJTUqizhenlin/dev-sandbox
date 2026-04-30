#include <acl/acl.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

namespace {

constexpr int kDeviceId = 0;
constexpr int kWarmupIterations = 5;
constexpr int kMeasureIterations = 50;
constexpr std::size_t kBufferCount = 8;

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
    const std::size_t mb = 1024 * 1024;
    const std::size_t kb = 1024;
    if (bytes % mb == 0) {
        return std::to_string(bytes / mb) + " MB";
    }
    if (bytes % kb == 0) {
        return std::to_string(bytes / kb) + " KB";
    }
    return std::to_string(bytes) + " B";
}

struct CopyBuffers {
    std::vector<void*> hostSrc;
    std::vector<void*> hostDst;
    std::vector<void*> device;
};

struct Timing {
    double submitUs = 0.0;
    double copyUs = 0.0;
};

void PrintTableHeader()
{
    std::cout << "AscendCL aclrtMemcpyAsync H2D/D2H benchmark\n"
              << "warmup=" << kWarmupIterations << ", iterations=" << kMeasureIterations
              << ", buffers_per_iteration=" << kBufferCount << ", device=" << kDeviceId
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

bool AllocateBuffers(std::size_t size, CopyBuffers* buffers)
{
    buffers->hostSrc.assign(kBufferCount, nullptr);
    buffers->hostDst.assign(kBufferCount, nullptr);
    buffers->device.assign(kBufferCount, nullptr);

    for (std::size_t i = 0; i < kBufferCount; ++i) {
        // aclrtMallocHost allocates page-locked host memory suitable for DMA transfers.
        if (!CHECK_ACL(aclrtMallocHost(&buffers->hostSrc[i], size))) {
            return false;
        }
        if (!CHECK_ACL(aclrtMallocHost(&buffers->hostDst[i], size))) {
            return false;
        }

        // aclrtMalloc allocates memory on the current Ascend device.
        if (!CHECK_ACL(aclrtMalloc(&buffers->device[i], size, ACL_MEM_MALLOC_HUGE_FIRST))) {
            return false;
        }

        FillHostData(buffers->hostSrc[i], size);
        std::memset(buffers->hostDst[i], 0, size);
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
    for (void* ptr : buffers->hostDst) {
        if (ptr != nullptr) {
            aclError ret = aclrtFreeHost(ptr);
            if (ret != ACL_SUCCESS) {
                std::cerr << "aclrtFreeHost(hostDst) failed, error code: " << ret << "\n";
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

bool RunSize(std::size_t size, aclrtStream stream)
{
    CopyBuffers buffers;
    bool ok = true;
    ok = AllocateBuffers(size, &buffers);

    // H2D: dst is device memory, destMax is device buffer size, src is host memory,
    // count is the bytes to copy, kind selects Host-to-Device, stream carries the async work.
    if (ok) {
        ok = RunOneDirection("H2D", buffers.device, buffers.hostSrc, size,
                             ACL_MEMCPY_HOST_TO_DEVICE, stream);
    }

    if (ok) {
        // D2H reads back from the same device buffer into host memory.
        ok = RunOneDirection("D2H", buffers.hostDst, buffers.device, size,
                             ACL_MEMCPY_DEVICE_TO_HOST, stream);
    }

    // aclrtFree releases device memory; aclrtFreeHost releases host memory.
    ok = FreeBuffers(&buffers) && ok;

    return ok;
}

}  // namespace

int main()
{
    // aclInit initializes AscendCL runtime state for this process.
    if (!CHECK_ACL(aclInit(nullptr))) {
        std::cerr << "aclInit failed. Check that Ascend runtime is installed and configured.\n";
        return 1;
    }

    bool ok = true;
    aclrtStream stream = nullptr;

    // aclrtSetDevice selects which Ascend device subsequent runtime calls use.
    if (!CHECK_ACL(aclrtSetDevice(kDeviceId))) {
        std::cerr << "aclrtSetDevice(" << kDeviceId << ") failed.\n";
        ok = false;
    }

    if (ok) {
        // aclrtCreateStream creates an asynchronous execution queue for memcpy operations.
        if (!CHECK_ACL(aclrtCreateStream(&stream))) {
            std::cerr << "aclrtCreateStream failed.\n";
            ok = false;
        }
    }

    if (ok) {
        const std::vector<std::size_t> sizes = {
            4 * 1024,       8 * 1024,       16 * 1024,      32 * 1024,
            64 * 1024,      128 * 1024,     256 * 1024,     512 * 1024,
            1024 * 1024,    4 * 1024 * 1024, 16 * 1024 * 1024,
        };

        PrintTableHeader();
        for (std::size_t size : sizes) {
            if (!RunSize(size, stream)) {
                ok = false;
                break;
            }
        }
    }

    if (stream != nullptr) {
        aclError ret = aclrtDestroyStream(stream);
        if (ret != ACL_SUCCESS) {
            std::cerr << "aclrtDestroyStream failed, error code: " << ret << "\n";
            ok = false;
        }
    }

    aclError resetRet = aclrtResetDevice(kDeviceId);
    if (!CHECK_ACL(resetRet)) {
        ok = false;
    }

    aclError finalizeRet = aclFinalize();
    if (!CHECK_ACL(finalizeRet)) {
        ok = false;
    }

    return ok ? 0 : 1;
}
