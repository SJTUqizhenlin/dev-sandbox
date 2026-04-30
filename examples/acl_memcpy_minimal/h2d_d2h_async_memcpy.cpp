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

void FillHostData(void* data, std::size_t size)
{
    unsigned char* p = static_cast<unsigned char*>(data);
    for (std::size_t i = 0; i < size; ++i) {
        p[i] = static_cast<unsigned char>(i & 0xff);
    }
}

bool RunOneDirection(const std::string& direction, void* dst, std::size_t destMax, const void* src,
                     std::size_t count, aclrtMemcpyKind kind, aclrtStream stream)
{
    for (int i = 0; i < kWarmupIterations; ++i) {
        // aclrtMemcpyAsync only submits work to the stream; it may return before the copy finishes.
        if (!CHECK_ACL(aclrtMemcpyAsync(dst, destMax, src, count, kind, stream))) {
            return false;
        }

        // aclrtSynchronizeStream waits until all previously submitted work in this stream completes.
        if (!CHECK_ACL(aclrtSynchronizeStream(stream))) {
            return false;
        }
    }

    double submitUs = 0.0;
    double totalUs = 0.0;
    for (int i = 0; i < kMeasureIterations; ++i) {
        auto submitBegin = std::chrono::steady_clock::now();
        if (!CHECK_ACL(aclrtMemcpyAsync(dst, destMax, src, count, kind, stream))) {
            return false;
        }
        auto submitEnd = std::chrono::steady_clock::now();
        if (!CHECK_ACL(aclrtSynchronizeStream(stream))) {
            return false;
        }
        auto totalEnd = std::chrono::steady_clock::now();

        submitUs += ElapsedUs(submitBegin, submitEnd);
        totalUs += ElapsedUs(submitBegin, totalEnd);
    }

    const double avgSubmitUs = submitUs / kMeasureIterations;
    const double avgTotalUs = totalUs / kMeasureIterations;
    const double avgWaitUs = avgTotalUs - avgSubmitUs;

    std::cout << direction << "," << count << "," << std::fixed << std::setprecision(3)
              << avgSubmitUs << "," << avgWaitUs << "," << avgTotalUs << ","
              << BandwidthMBps(count, avgTotalUs) << "\n";
    return true;
}

bool RunSize(std::size_t size, aclrtStream stream)
{
    void* hostSrc = nullptr;
    void* hostDst = nullptr;
    void* deviceBuf = nullptr;
    bool ok = true;

    // aclrtMallocHost allocates page-locked host memory suitable for DMA transfers.
    ok = ok && CHECK_ACL(aclrtMallocHost(&hostSrc, size));
    ok = ok && CHECK_ACL(aclrtMallocHost(&hostDst, size));

    // aclrtMalloc allocates memory on the current Ascend device.
    ok = ok && CHECK_ACL(aclrtMalloc(&deviceBuf, size, ACL_MEM_MALLOC_HUGE_FIRST));

    if (ok) {
        FillHostData(hostSrc, size);
        std::memset(hostDst, 0, size);
    }

    // H2D: dst is device memory, destMax is device buffer size, src is host memory,
    // count is the bytes to copy, kind selects Host-to-Device, stream carries the async work.
    if (ok) {
        ok = RunOneDirection("H2D", deviceBuf, size, hostSrc, size, ACL_MEMCPY_HOST_TO_DEVICE,
                             stream);
    }

    if (ok) {
        // D2H reads back from the same device buffer into host memory.
        ok = RunOneDirection("D2H", hostDst, size, deviceBuf, size, ACL_MEMCPY_DEVICE_TO_HOST,
                             stream);
    }

    // aclrtFree releases device memory; aclrtFreeHost releases host memory.
    if (deviceBuf != nullptr) {
        aclError ret = aclrtFree(deviceBuf);
        if (ret != ACL_SUCCESS) {
            std::cerr << "aclrtFree failed, error code: " << ret << "\n";
            ok = false;
        }
    }
    if (hostDst != nullptr) {
        aclError ret = aclrtFreeHost(hostDst);
        if (ret != ACL_SUCCESS) {
            std::cerr << "aclrtFreeHost(hostDst) failed, error code: " << ret << "\n";
            ok = false;
        }
    }
    if (hostSrc != nullptr) {
        aclError ret = aclrtFreeHost(hostSrc);
        if (ret != ACL_SUCCESS) {
            std::cerr << "aclrtFreeHost(hostSrc) failed, error code: " << ret << "\n";
            ok = false;
        }
    }

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

        std::cout << "direction,size_bytes,avg_submit_us,avg_wait_us,avg_total_us,"
                     "bandwidth_MBps\n";
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
