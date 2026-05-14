#include <acl/acl.h>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cerrno>
#include <csignal>
#include <functional>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#ifndef _WIN32
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace {

constexpr int kDeviceId = 0;
constexpr int kAllDeviceCount = 8;
constexpr std::size_t kMultiStreamCount = 4;
constexpr int kWarmupIterations = 5;
constexpr int kDefaultMeasureIterations = 128;
constexpr std::size_t kDefaultIoSize = 64 * 1024;
constexpr std::size_t kDefaultBufferCount = 1024;

std::vector<int> DefaultDevices()
{
    std::vector<int> devices;
    devices.reserve(kAllDeviceCount);
    for (int device = 0; device < kAllDeviceCount; ++device) {
        devices.push_back(device);
    }
    return devices;
}

enum class TestType {
    All,
    SingleStream,
    Batch,
    MultiStream,
    All8SingleStream,
    All8Process,
};

struct Options {
    std::size_t ioSize = kDefaultIoSize;
    std::size_t bufferCount = kDefaultBufferCount;
    std::size_t iterations = kDefaultMeasureIterations;
    std::vector<int> devices = DefaultDevices();
    TestType testType = TestType::SingleStream;
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

bool InitAclReference()
{
    const aclError ret = aclInit(nullptr);
    if (ret == ACL_SUCCESS || ret == ACL_ERROR_REPEAT_INITIALIZE) {
        return true;
    }
    return CheckAcl(ret, "aclInit(nullptr)", __FILE__, __LINE__);
}

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

std::string NormalizeName(std::string text)
{
    for (char& ch : text) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        if (ch == '-') {
            ch = '_';
        }
    }
    return text;
}

std::string DeviceListLabel(const std::vector<int>& devices)
{
    std::string label;
    for (std::size_t i = 0; i < devices.size(); ++i) {
        if (i > 0) {
            label += ",";
        }
        label += std::to_string(devices[i]);
    }
    return label;
}

const char* TestTypeName(TestType type)
{
    switch (type) {
        case TestType::All:
            return "all";
        case TestType::SingleStream:
            return "single_stream";
        case TestType::Batch:
            return "batch";
        case TestType::MultiStream:
            return "multi_stream";
        case TestType::All8SingleStream:
            return "all8_single_stream";
        case TestType::All8Process:
            return "all8_process";
    }
    return "unknown";
}

bool ParseTestType(const std::string& text, TestType* type)
{
    if (type == nullptr) {
        return false;
    }
    const std::string name = NormalizeName(text);
    if (name == "all") {
        *type = TestType::All;
        return true;
    }
    if (name == "single" || name == "single_stream" || name == "ss") {
        *type = TestType::SingleStream;
        return true;
    }
    if (name == "batch" || name == "batch_stream" || name == "batch_async") {
        *type = TestType::Batch;
        return true;
    }
    if (name == "multi" || name == "multi_stream" || name == "multistream" ||
        name == "ms" || name == "ms4" || name == "ms48") {
        *type = TestType::MultiStream;
        return true;
    }
    if (name == "all8" || name == "all8_single_stream" || name == "multi_device" ||
        name == "multi_card" || name == "all_device" || name == "all_devices") {
        *type = TestType::All8SingleStream;
        return true;
    }
    if (name == "all8_process" || name == "all8_proc" || name == "multi_process" ||
        name == "multi_proc" || name == "process") {
        *type = TestType::All8Process;
        return true;
    }
    return false;
}

void PrintUsage(const char* prog)
{
    std::cout << "Usage: " << (prog != nullptr ? prog : "h2d_async_memcpy")
              << " [-t <test_type>] [-s <io_size>] [-n <buffer_count>] [-i <iterations>]"
              << " [-d <device_list>]\n"
              << "\n"
              << "Options:\n"
              << "  -t <test_type>     Test to run. Default: "
              << TestTypeName(TestType::SingleStream) << "\n"
              << "                     all, single_stream, batch, multi_stream,"
              << " all8_single_stream, all8_process\n"
              << "  -s <io_size>       Bytes per buffer. Suffixes K/M/G are supported."
              << " Default: " << SizeLabel(kDefaultIoSize) << "\n"
              << "  -n <buffer_count>  Number of buffers per measurement iteration."
              << " Default: " << kDefaultBufferCount << "\n"
              << "  -i <iterations>    Number of measured iterations."
              << " Default: " << kDefaultMeasureIterations << "\n"
              << "  -d <device_list>   Devices for all8_single_stream/all8_process."
              << " Use comma or space separated IDs. Default: "
              << DeviceListLabel(DefaultDevices()) << "\n"
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

bool ParseDeviceId(const std::string& text, int* device)
{
    if (text.empty() || device == nullptr) {
        return false;
    }
    try {
        std::size_t pos = 0;
        const int parsed = std::stoi(text, &pos, 10);
        if (pos != text.size() || parsed < 0) {
            return false;
        }
        *device = parsed;
        return true;
    } catch (...) {
        return false;
    }
}

bool ParseDeviceList(const std::string& text, std::vector<int>* devices)
{
    if (text.empty() || devices == nullptr) {
        return false;
    }

    std::vector<int> parsedDevices;
    std::size_t begin = 0;
    while (begin < text.size()) {
        while (begin < text.size() &&
               (text[begin] == ',' ||
                std::isspace(static_cast<unsigned char>(text[begin])) != 0)) {
            ++begin;
        }
        if (begin >= text.size()) {
            break;
        }

        std::size_t end = begin;
        while (end < text.size() && text[end] != ',' &&
               std::isspace(static_cast<unsigned char>(text[end])) == 0) {
            ++end;
        }

        const std::string token = text.substr(begin, end - begin);
        int device = 0;
        if (!ParseDeviceId(token, &device)) {
            return false;
        }
        if (std::find(parsedDevices.begin(), parsedDevices.end(), device) !=
            parsedDevices.end()) {
            return false;
        }
        parsedDevices.push_back(device);
        begin = end + 1;
    }

    if (parsedDevices.empty()) {
        return false;
    }
    *devices = std::move(parsedDevices);
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
        if (arg == "-t") {
            if (i + 1 >= argc || !ParseTestType(argv[++i], &options->testType)) {
                std::cerr << "Invalid value for -t.\n";
                PrintUsage(argv[0]);
                return false;
            }
            continue;
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
        if (arg == "-i") {
            if (i + 1 >= argc || !ParseSize(argv[++i], &options->iterations)) {
                std::cerr << "Invalid value for -i.\n";
                PrintUsage(argv[0]);
                return false;
            }
            continue;
        }
        if (arg == "-d") {
            if (i + 1 >= argc) {
                std::cerr << "Invalid value for -d.\n";
                PrintUsage(argv[0]);
                return false;
            }
            std::string deviceList = argv[++i];
            while (i + 1 < argc && argv[i + 1][0] != '-') {
                deviceList += ",";
                deviceList += argv[++i];
            }
            if (!ParseDeviceList(deviceList, &options->devices)) {
                std::cerr << "Invalid value for -d.\n";
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
    void* hostBase = nullptr;
    void* deviceBase = nullptr;
    std::vector<void*> hostSrc;
    std::vector<void*> device;
};

struct BatchCopyPlan {
    std::vector<size_t> destMaxs;
    std::vector<size_t> sizes;
    std::vector<aclrtMemcpyBatchAttr> attrs;
    std::vector<size_t> attrIndexes;
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

struct ProcessResultHeader {
    int ok = 0;
    std::uint64_t iterations = 0;
};

struct StreamTask {
    aclrtStream stream = nullptr;
    aclrtEvent finish = nullptr;
    std::vector<void*> src;
    std::vector<void*> dst;
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

void PrintTableHeader(const std::string& title, const Options& options)
{
    std::cout << title << "\n"
              << "warmup=" << kWarmupIterations << ", iterations=" << options.iterations
              << ", buffers_per_iteration=" << options.bufferCount;
    if (options.testType == TestType::All8SingleStream ||
        options.testType == TestType::All8Process) {
        std::cout << ", devices=" << DeviceListLabel(options.devices);
    }
    std::cout << "\n\n";

    std::cout << std::left << std::setw(8) << "Dir" << std::right << std::setw(12) << "Size"
              << std::setw(8) << "Count" << std::setw(14) << "Submit(us)" << std::setw(14)
              << "Wait(us)" << std::setw(14) << "Copy(us)" << std::setw(16)
              << "Submit/IO(us)" << std::setw(14) << "Copy/IO(us)" << std::setw(16)
              << "BW(MB/s)" << "\n";
    std::cout << std::string(116, '-') << "\n";
}

void PrintTableRow(const std::string& direction, std::size_t bytes, std::size_t count,
                   double avgSubmitUs, double avgWaitUs, double avgCopyUs)
{
    const auto countAsDouble = static_cast<double>(count);
    const auto submitPerIoUs = avgSubmitUs / countAsDouble;
    const auto copyPerIoUs = avgCopyUs / countAsDouble;
    std::cout << std::left << std::setw(8) << direction << std::right << std::setw(12)
              << SizeLabel(bytes) << std::setw(8) << count << std::fixed << std::setprecision(3)
              << std::setw(14) << avgSubmitUs << std::setw(14) << avgWaitUs
              << std::setw(14) << avgCopyUs << std::setw(16) << submitPerIoUs
              << std::setw(14) << copyPerIoUs << std::setprecision(2) << std::setw(16)
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
                      aclrtStream stream, std::size_t iterations)
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
        for (std::size_t i = 0; i < iterations; ++i) {
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

    const double avgSubmitUs = submitUs / static_cast<double>(iterations);
    const double avgCopyUs = copyUs / static_cast<double>(iterations);
    const double avgWaitUs = avgCopyUs > avgSubmitUs ? avgCopyUs - avgSubmitUs : 0.0;

    PrintTableRow(direction, size, src.size(), avgSubmitUs, avgWaitUs, avgCopyUs);
    return ok;
}

bool AllocateBuffers(std::size_t size, std::size_t bufferCount, CopyBuffers* buffers)
{
    const std::size_t total = size * bufferCount;
    buffers->hostSrc.assign(bufferCount, nullptr);
    buffers->device.assign(bufferCount, nullptr);

    // Match the main copy benchmark: allocate one large host/device buffer and
    // slice it into fixed-size copy entries.
    if (!CHECK_ACL(aclrtMallocHost(&buffers->hostBase, total))) {
        return false;
    }
    if (!CHECK_ACL(aclrtMalloc(&buffers->deviceBase, total, ACL_MEM_MALLOC_HUGE_FIRST))) {
        return false;
    }
    FillHostData(buffers->hostBase, total);

    for (std::size_t i = 0; i < bufferCount; ++i) {
        buffers->hostSrc[i] =
            static_cast<void*>(static_cast<char*>(buffers->hostBase) + i * size);
        buffers->device[i] =
            static_cast<void*>(static_cast<char*>(buffers->deviceBase) + i * size);
    }
    return true;
}

bool FreeBuffers(CopyBuffers* buffers)
{
    bool ok = true;
    if (buffers->deviceBase != nullptr) {
        aclError ret = aclrtFree(buffers->deviceBase);
        if (ret != ACL_SUCCESS) {
            std::cerr << "aclrtFree failed, error code: " << ret << "\n";
            ok = false;
        }
        buffers->deviceBase = nullptr;
    }
    if (buffers->hostBase != nullptr) {
        aclError ret = aclrtFreeHost(buffers->hostBase);
        if (ret != ACL_SUCCESS) {
            std::cerr << "aclrtFreeHost(hostSrc) failed, error code: " << ret << "\n";
            ok = false;
        }
        buffers->hostBase = nullptr;
    }
    return ok;
}

bool RunSize(const Options& options, aclrtStream stream)
{
    CopyBuffers buffers;
    bool ok = true;
    ok = AllocateBuffers(options.ioSize, options.bufferCount, &buffers);

    // H2D: dst is device memory, destMax is device buffer size, src is host memory,
    // count is the bytes to copy, kind selects Host-to-Device, stream carries the async work.
    if (ok) {
        ok = RunOneDirection("H2D", buffers.device, buffers.hostSrc, options.ioSize,
                             ACL_MEMCPY_HOST_TO_DEVICE, stream, options.iterations);
    }

    // aclrtFree releases device memory; aclrtFreeHost releases host memory.
    ok = FreeBuffers(&buffers) && ok;

    return ok;
}

BatchCopyPlan BuildBatchCopyPlan(std::size_t size, std::size_t bufferCount, int deviceId)
{
    BatchCopyPlan plan;
    plan.destMaxs.assign(bufferCount, size);
    plan.sizes.assign(bufferCount, size);

    aclrtMemcpyBatchAttr attr;
    std::memset(&attr, 0, sizeof(attr));
    attr.srcLoc.type = ACL_MEM_LOCATION_TYPE_HOST;
    attr.dstLoc.type = ACL_MEM_LOCATION_TYPE_DEVICE;
    attr.dstLoc.id = deviceId;
    plan.attrs.push_back(attr);

    // One attribute entry applies to all batch items.
    plan.attrIndexes.push_back(0);
    return plan;
}

bool SubmitBatchCopy(const std::vector<void*>& dst, const std::vector<void*>& src,
                     BatchCopyPlan* plan, aclrtStream stream)
{
    if (plan == nullptr) {
        return false;
    }

    size_t failureIndex = static_cast<size_t>(-1);
    aclError ret = aclrtMemcpyBatchAsync(const_cast<void**>(dst.data()), plan->destMaxs.data(),
                                         const_cast<void**>(src.data()), plan->sizes.data(),
                                         src.size(), plan->attrs.data(),
                                         plan->attrIndexes.data(), plan->attrs.size(),
                                         &failureIndex, stream);
    if (ret != ACL_SUCCESS && failureIndex != static_cast<size_t>(-1)) {
        std::cerr << "aclrtMemcpyBatchAsync failed at batch index " << failureIndex << ".\n";
    }
    return CHECK_ACL(ret);
}

bool MeasureBatchCopyOnce(const std::vector<void*>& dst, const std::vector<void*>& src,
                          BatchCopyPlan* plan, aclrtStream stream, aclrtEvent start,
                          aclrtEvent end, Timing* timing)
{
    if (timing == nullptr) {
        return false;
    }
    if (!CHECK_ACL(aclrtRecordEvent(start, stream))) {
        return false;
    }

    auto submitBegin = std::chrono::steady_clock::now();
    if (!SubmitBatchCopy(dst, src, plan, stream)) {
        return false;
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

bool RunBatchDirection(const Options& options, CopyBuffers* buffers, aclrtStream stream)
{
    if (buffers == nullptr) {
        return false;
    }

    aclrtEvent start = nullptr;
    aclrtEvent end = nullptr;
    BatchCopyPlan plan = BuildBatchCopyPlan(options.ioSize, options.bufferCount, kDeviceId);

    if (!CHECK_ACL(aclrtCreateEvent(&start))) {
        return false;
    }
    if (!CHECK_ACL(aclrtCreateEvent(&end))) {
        aclrtDestroyEvent(start);
        return false;
    }

    bool ok = true;
    for (int i = 0; ok && i < kWarmupIterations; ++i) {
        Timing timing;
        ok = MeasureBatchCopyOnce(buffers->device, buffers->hostSrc, &plan, stream, start, end,
                                  &timing);
    }

    double submitUs = 0.0;
    double copyUs = 0.0;
    for (std::size_t i = 0; ok && i < options.iterations; ++i) {
        Timing timing;
        ok = MeasureBatchCopyOnce(buffers->device, buffers->hostSrc, &plan, stream, start, end,
                                  &timing);
        if (ok) {
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

    const double avgSubmitUs = submitUs / static_cast<double>(options.iterations);
    const double avgCopyUs = copyUs / static_cast<double>(options.iterations);
    const double avgWaitUs = avgCopyUs > avgSubmitUs ? avgCopyUs - avgSubmitUs : 0.0;
    PrintTableRow("H2D_BATCH", options.ioSize, options.bufferCount, avgSubmitUs, avgWaitUs,
                  avgCopyUs);
    return true;
}

bool RunSingleDeviceBatch(const Options& options)
{
    bool ok = true;
    aclrtStream stream = nullptr;
    CopyBuffers buffers;

    if (!CHECK_ACL(aclrtSetDevice(kDeviceId))) {
        std::cerr << "aclrtSetDevice(" << kDeviceId << ") failed.\n";
        return false;
    }
    if (!CHECK_ACL(aclrtCreateStream(&stream))) {
        std::cerr << "aclrtCreateStream failed.\n";
        return false;
    }

    PrintTableHeader("AscendCL aclrtMemcpyBatchAsync single-device H2D benchmark", options);
    ok = AllocateBuffers(options.ioSize, options.bufferCount, &buffers);
    if (ok) {
        ok = RunBatchDirection(options, &buffers, stream);
    }
    ok = FreeBuffers(&buffers) && ok;

    if (stream != nullptr) {
        aclError ret = aclrtDestroyStream(stream);
        if (ret != ACL_SUCCESS) {
            std::cerr << "aclrtDestroyStream failed, error code: " << ret << "\n";
            ok = false;
        }
    }
    return ok;
}

bool BuildStreamTasks(const CopyBuffers& buffers, std::size_t streamCount,
                      std::vector<StreamTask>* tasks)
{
    if (tasks == nullptr) {
        return false;
    }
    tasks->clear();
    const std::size_t bufferCount = buffers.hostSrc.size();
    const std::size_t activeStreams = std::min(streamCount, bufferCount);
    if (activeStreams == 0) {
        return false;
    }

    tasks->reserve(activeStreams);
    const std::size_t base = bufferCount / activeStreams;
    const std::size_t remainder = bufferCount % activeStreams;
    std::size_t offset = 0;
    for (std::size_t s = 0; s < activeStreams; ++s) {
        const std::size_t count = base + (s < remainder ? 1 : 0);
        StreamTask task;
        if (!CHECK_ACL(aclrtCreateStream(&task.stream))) {
            return false;
        }
        if (!CHECK_ACL(aclrtCreateEvent(&task.finish))) {
            aclrtDestroyStream(task.stream);
            return false;
        }
        task.src.reserve(count);
        task.dst.reserve(count);
        for (std::size_t i = 0; i < count; ++i) {
            task.src.push_back(buffers.hostSrc[offset + i]);
            task.dst.push_back(buffers.device[offset + i]);
        }
        offset += count;
        tasks->push_back(std::move(task));
    }
    return true;
}

bool DestroyStreamTasks(std::vector<StreamTask>* tasks)
{
    if (tasks == nullptr) {
        return false;
    }
    bool ok = true;
    for (auto& task : *tasks) {
        if (task.finish != nullptr) {
            ok = CHECK_ACL(aclrtDestroyEvent(task.finish)) && ok;
            task.finish = nullptr;
        }
        if (task.stream != nullptr) {
            ok = CHECK_ACL(aclrtDestroyStream(task.stream)) && ok;
            task.stream = nullptr;
        }
    }
    tasks->clear();
    return ok;
}

bool MeasureMultiStreamCopyOnce(std::vector<StreamTask>& tasks, std::size_t size,
                                aclrtEvent start, aclrtEvent end, Timing* timing)
{
    if (tasks.empty() || timing == nullptr) {
        return false;
    }

    if (!CHECK_ACL(aclrtRecordEvent(start, tasks[0].stream))) {
        return false;
    }

    auto submitBegin = std::chrono::steady_clock::now();
    for (auto& task : tasks) {
        for (std::size_t i = 0; i < task.src.size(); ++i) {
            if (!CHECK_ACL(aclrtMemcpyAsync(task.dst[i], size, task.src[i], size,
                                            ACL_MEMCPY_HOST_TO_DEVICE, task.stream))) {
                return false;
            }
        }
    }
    auto submitEnd = std::chrono::steady_clock::now();

    for (std::size_t i = 1; i < tasks.size(); ++i) {
        if (!CHECK_ACL(aclrtRecordEvent(tasks[i].finish, tasks[i].stream))) {
            return false;
        }
        if (!CHECK_ACL(aclrtStreamWaitEvent(tasks[0].stream, tasks[i].finish))) {
            return false;
        }
    }

    if (!CHECK_ACL(aclrtRecordEvent(end, tasks[0].stream))) {
        return false;
    }
    if (!CHECK_ACL(aclrtSynchronizeStream(tasks[0].stream))) {
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

bool RunMultiStreamDirection(const Options& options, CopyBuffers* buffers)
{
    if (buffers == nullptr) {
        return false;
    }

    aclrtEvent start = nullptr;
    aclrtEvent end = nullptr;
    std::vector<StreamTask> tasks;
    bool ok = true;

    ok = BuildStreamTasks(*buffers, kMultiStreamCount, &tasks);
    if (ok) {
        ok = CHECK_ACL(aclrtCreateEvent(&start));
    }
    if (ok) {
        ok = CHECK_ACL(aclrtCreateEvent(&end));
    }

    for (int i = 0; ok && i < kWarmupIterations; ++i) {
        Timing timing;
        ok = MeasureMultiStreamCopyOnce(tasks, options.ioSize, start, end, &timing);
    }

    double submitUs = 0.0;
    double copyUs = 0.0;
    for (std::size_t i = 0; ok && i < options.iterations; ++i) {
        Timing timing;
        ok = MeasureMultiStreamCopyOnce(tasks, options.ioSize, start, end, &timing);
        if (ok) {
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
    ok = DestroyStreamTasks(&tasks) && ok;

    if (!ok) {
        return false;
    }

    const double avgSubmitUs = submitUs / static_cast<double>(options.iterations);
    const double avgCopyUs = copyUs / static_cast<double>(options.iterations);
    const double avgWaitUs = avgCopyUs > avgSubmitUs ? avgCopyUs - avgSubmitUs : 0.0;
    PrintTableRow("H2D_MS4", options.ioSize, options.bufferCount, avgSubmitUs, avgWaitUs,
                  avgCopyUs);
    return true;
}

bool RunSingleDeviceMultiStream(const Options& options)
{
    bool ok = true;
    CopyBuffers buffers;

    if (!CHECK_ACL(aclrtSetDevice(kDeviceId))) {
        std::cerr << "aclrtSetDevice(" << kDeviceId << ") failed.\n";
        return false;
    }

    PrintTableHeader("AscendCL aclrtMemcpyAsync single-device 4-stream H2D benchmark",
                     options);
    ok = AllocateBuffers(options.ioSize, options.bufferCount, &buffers);
    if (ok) {
        ok = RunMultiStreamDirection(options, &buffers);
    }
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

    PrintTableHeader("AscendCL aclrtMemcpyAsync single-device H2D benchmark", options);
    ok = RunSize(options, stream);

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
    bool aclInitialized = false;
    bool deviceSet = false;
    aclrtStream stream = nullptr;
    aclrtEvent start = nullptr;
    aclrtEvent end = nullptr;
    CopyBuffers buffers;

    ok = InitAclReference();
    aclInitialized = ok;
    if (ok) {
        ok = CHECK_ACL(aclrtSetDevice(deviceId));
        deviceSet = ok;
    }
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

    result->submitUs.assign(options.iterations, 0.0);
    result->copyUs.assign(options.iterations, 0.0);
    for (std::size_t i = 0; i < options.iterations; ++i) {
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
    if (deviceSet) {
        ok = CHECK_ACL(aclrtResetDevice(deviceId)) && ok;
    }
    if (aclInitialized) {
        ok = CHECK_ACL(aclFinalizeReference(nullptr)) && ok;
    }
    result->ok = result->ok && ok;
}

bool RunAllDevices(const Options& options)
{
    const std::size_t deviceCount = options.devices.size();
    Barrier beginBarrier(static_cast<int>(deviceCount));
    Barrier endBarrier(static_cast<int>(deviceCount));
    std::vector<DeviceTimings> results(deviceCount);
    std::vector<std::thread> threads;
    threads.reserve(deviceCount);

    for (std::size_t index = 0; index < deviceCount; ++index) {
        const int device = options.devices[index];
        threads.emplace_back(RunAllDeviceWorker, device, std::cref(options), &beginBarrier,
                             &endBarrier, &results[index]);
    }
    for (auto& thread : threads) {
        thread.join();
    }

    bool ok = true;
    for (std::size_t index = 0; index < deviceCount; ++index) {
        if (!results[index].ok) {
            const int device = options.devices[index];
            std::cerr << "device " << device << " failed during all-device H2D test.\n";
            ok = false;
        }
    }
    if (!ok) {
        return false;
    }

    std::vector<double> maxSubmitUs(options.iterations, 0.0);
    std::vector<double> maxCopyUs(options.iterations, 0.0);
    for (std::size_t iter = 0; iter < options.iterations; ++iter) {
        for (const auto& result : results) {
            maxSubmitUs[iter] = std::max(maxSubmitUs[iter], result.submitUs[iter]);
            maxCopyUs[iter] = std::max(maxCopyUs[iter], result.copyUs[iter]);
        }
    }

    const double avgSubmitUs = Average(maxSubmitUs);
    const double avgCopyUs = Average(maxCopyUs);
    const double avgWaitUs = avgCopyUs > avgSubmitUs ? avgCopyUs - avgSubmitUs : 0.0;

    PrintTableHeader("AscendCL aclrtMemcpyAsync multi-device simultaneous H2D benchmark",
                     options);
    const std::string rowName = deviceCount == kAllDeviceCount ? "H2D_ALL8" : "H2D_ALL";
    PrintTableRow(rowName, options.ioSize, options.bufferCount * deviceCount,
                  avgSubmitUs, avgWaitUs, avgCopyUs);
    return true;
}

#ifndef _WIN32
bool ReadExact(int fd, void* data, std::size_t size)
{
    char* ptr = static_cast<char*>(data);
    std::size_t done = 0;
    while (done < size) {
        const ssize_t ret = read(fd, ptr + done, size - done);
        if (ret > 0) {
            done += static_cast<std::size_t>(ret);
            continue;
        }
        if (ret == 0) {
            return false;
        }
        if (errno == EINTR) {
            continue;
        }
        return false;
    }
    return true;
}

bool WriteExact(int fd, const void* data, std::size_t size)
{
    const char* ptr = static_cast<const char*>(data);
    std::size_t done = 0;
    while (done < size) {
        const ssize_t ret = write(fd, ptr + done, size - done);
        if (ret > 0) {
            done += static_cast<std::size_t>(ret);
            continue;
        }
        if (ret < 0 && errno == EINTR) {
            continue;
        }
        return false;
    }
    return true;
}

void CloseFd(int* fd)
{
    if (fd != nullptr && *fd >= 0) {
        close(*fd);
        *fd = -1;
    }
}

void WriteProcessResult(int fd, bool ok, const std::vector<double>& submitUs,
                        const std::vector<double>& copyUs)
{
    ProcessResultHeader header;
    header.ok = ok ? 1 : 0;
    header.iterations = static_cast<std::uint64_t>(submitUs.size());
    WriteExact(fd, &header, sizeof(header));
    if (!submitUs.empty()) {
        WriteExact(fd, submitUs.data(), submitUs.size() * sizeof(double));
        WriteExact(fd, copyUs.data(), copyUs.size() * sizeof(double));
    }
}

void RunAllDeviceProcessChild(int deviceId, const Options& options, int startReadFd,
                              int barrierWriteFd, int resultWriteFd)
{
    bool ok = true;
    bool aclInitialized = false;
    bool deviceSet = false;
    aclrtStream stream = nullptr;
    aclrtEvent start = nullptr;
    aclrtEvent end = nullptr;
    CopyBuffers buffers;
    std::vector<double> submitUs(options.iterations, 0.0);
    std::vector<double> copyUs(options.iterations, 0.0);

    ok = CHECK_ACL(aclInit(nullptr));
    aclInitialized = ok;
    if (ok) {
        ok = CHECK_ACL(aclrtSetDevice(deviceId));
        deviceSet = ok;
    }
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

    const char ready = ok ? 'R' : 'E';
    WriteExact(barrierWriteFd, &ready, sizeof(ready));

    const std::size_t totalSteps = kWarmupIterations + options.iterations;
    for (std::size_t step = 0; step < totalSteps; ++step) {
        char startSignal = 0;
        if (!ReadExact(startReadFd, &startSignal, sizeof(startSignal))) {
            ok = false;
            break;
        }

        Timing timing;
        if (ok) {
            ok = MeasureCopyOnce(buffers.device, buffers.hostSrc, options.ioSize,
                                 ACL_MEMCPY_HOST_TO_DEVICE, stream, start, end, &timing);
        }
        if (ok && step >= kWarmupIterations) {
            const std::size_t iter = step - kWarmupIterations;
            submitUs[iter] = timing.submitUs;
            copyUs[iter] = timing.copyUs;
        }

        const char done = ok ? 'D' : 'E';
        WriteExact(barrierWriteFd, &done, sizeof(done));
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
    if (deviceSet) {
        ok = CHECK_ACL(aclrtResetDevice(deviceId)) && ok;
    }
    if (aclInitialized) {
        ok = CHECK_ACL(aclFinalize()) && ok;
    }

    WriteProcessResult(resultWriteFd, ok, submitUs, copyUs);
    CloseFd(&startReadFd);
    CloseFd(&barrierWriteFd);
    CloseFd(&resultWriteFd);
    _exit(ok ? 0 : 1);
}

struct ChildProcess {
    pid_t pid = -1;
    int startWriteFd = -1;
    int barrierReadFd = -1;
    int resultReadFd = -1;
};

bool ReadChildTimings(const ChildProcess& child, std::size_t iterations, DeviceTimings* result)
{
    if (result == nullptr) {
        return false;
    }
    ProcessResultHeader header;
    if (!ReadExact(child.resultReadFd, &header, sizeof(header))) {
        return false;
    }
    if (header.iterations != iterations) {
        return false;
    }

    result->submitUs.assign(iterations, 0.0);
    result->copyUs.assign(iterations, 0.0);
    if (iterations > 0) {
        if (!ReadExact(child.resultReadFd, result->submitUs.data(),
                       iterations * sizeof(double))) {
            return false;
        }
        if (!ReadExact(child.resultReadFd, result->copyUs.data(), iterations * sizeof(double))) {
            return false;
        }
    }
    result->ok = header.ok != 0;
    return result->ok;
}

bool SpawnAllDeviceProcess(int deviceId, const Options& options, ChildProcess* child)
{
    if (child == nullptr) {
        return false;
    }

    int startPipe[2] = {-1, -1};
    int barrierPipe[2] = {-1, -1};
    int resultPipe[2] = {-1, -1};
    if (pipe(startPipe) != 0 || pipe(barrierPipe) != 0 || pipe(resultPipe) != 0) {
        std::cerr << "pipe failed for device " << deviceId << ".\n";
        CloseFd(&startPipe[0]);
        CloseFd(&startPipe[1]);
        CloseFd(&barrierPipe[0]);
        CloseFd(&barrierPipe[1]);
        CloseFd(&resultPipe[0]);
        CloseFd(&resultPipe[1]);
        return false;
    }

    const pid_t pid = fork();
    if (pid < 0) {
        std::cerr << "fork failed for device " << deviceId << ".\n";
        CloseFd(&startPipe[0]);
        CloseFd(&startPipe[1]);
        CloseFd(&barrierPipe[0]);
        CloseFd(&barrierPipe[1]);
        CloseFd(&resultPipe[0]);
        CloseFd(&resultPipe[1]);
        return false;
    }

    if (pid == 0) {
        CloseFd(&startPipe[1]);
        CloseFd(&barrierPipe[0]);
        CloseFd(&resultPipe[0]);
        RunAllDeviceProcessChild(deviceId, options, startPipe[0], barrierPipe[1],
                                 resultPipe[1]);
    }

    CloseFd(&startPipe[0]);
    CloseFd(&barrierPipe[1]);
    CloseFd(&resultPipe[1]);
    child->pid = pid;
    child->startWriteFd = startPipe[1];
    child->barrierReadFd = barrierPipe[0];
    child->resultReadFd = resultPipe[0];
    return true;
}

void CloseChildPipes(ChildProcess* child)
{
    if (child == nullptr) {
        return;
    }
    CloseFd(&child->startWriteFd);
    CloseFd(&child->barrierReadFd);
    CloseFd(&child->resultReadFd);
}

bool RunAllDeviceProcesses(const Options& options)
{
    std::signal(SIGPIPE, SIG_IGN);

    const std::size_t deviceCount = options.devices.size();
    std::vector<ChildProcess> children(deviceCount);
    bool ok = true;
    for (std::size_t index = 0; index < deviceCount; ++index) {
        ok = SpawnAllDeviceProcess(options.devices[index], options, &children[index]) && ok;
    }
    if (!ok) {
        for (auto& child : children) {
            CloseChildPipes(&child);
        }
        for (const auto& child : children) {
            if (child.pid > 0) {
                waitpid(child.pid, nullptr, 0);
            }
        }
        return false;
    }

    for (std::size_t index = 0; index < deviceCount; ++index) {
        char ready = 0;
        if (!ReadExact(children[index].barrierReadFd, &ready, sizeof(ready)) ||
            ready != 'R') {
            const int device = options.devices[index];
            std::cerr << "device " << device << " process failed during setup.\n";
            ok = false;
        }
    }

    const std::size_t totalSteps = kWarmupIterations + options.iterations;
    for (std::size_t step = 0; ok && step < totalSteps; ++step) {
        const char start = 'S';
        for (auto& child : children) {
            if (!WriteExact(child.startWriteFd, &start, sizeof(start))) {
                ok = false;
            }
        }
        for (std::size_t index = 0; index < deviceCount; ++index) {
            char done = 0;
            if (!ReadExact(children[index].barrierReadFd, &done, sizeof(done)) ||
                done != 'D') {
                const int device = options.devices[index];
                std::cerr << "device " << device << " process failed during copy step.\n";
                ok = false;
            }
        }
    }

    for (auto& child : children) {
        CloseFd(&child.startWriteFd);
        CloseFd(&child.barrierReadFd);
    }

    std::vector<DeviceTimings> results(deviceCount);
    for (std::size_t index = 0; index < deviceCount; ++index) {
        if (!ReadChildTimings(children[index], options.iterations, &results[index])) {
            const int device = options.devices[index];
            std::cerr << "device " << device << " process failed to return timings.\n";
            ok = false;
        }
        CloseFd(&children[index].resultReadFd);
    }

    for (std::size_t index = 0; index < deviceCount; ++index) {
        int status = 0;
        if (waitpid(children[index].pid, &status, 0) < 0 || !WIFEXITED(status) ||
            WEXITSTATUS(status) != 0) {
            const int device = options.devices[index];
            std::cerr << "device " << device << " process exited abnormally.\n";
            ok = false;
        }
    }

    if (!ok) {
        return false;
    }

    std::vector<double> maxSubmitUs(options.iterations, 0.0);
    std::vector<double> maxCopyUs(options.iterations, 0.0);
    for (std::size_t iter = 0; iter < options.iterations; ++iter) {
        for (const auto& result : results) {
            maxSubmitUs[iter] = std::max(maxSubmitUs[iter], result.submitUs[iter]);
            maxCopyUs[iter] = std::max(maxCopyUs[iter], result.copyUs[iter]);
        }
    }

    const double avgSubmitUs = Average(maxSubmitUs);
    const double avgCopyUs = Average(maxCopyUs);
    const double avgWaitUs = avgCopyUs > avgSubmitUs ? avgCopyUs - avgSubmitUs : 0.0;

    PrintTableHeader("AscendCL aclrtMemcpyAsync multi-process simultaneous H2D benchmark",
                     options);
    const std::string rowName = deviceCount == kAllDeviceCount ? "H2D_ALL8P" : "H2D_ALLP";
    PrintTableRow(rowName, options.ioSize, options.bufferCount * deviceCount,
                  avgSubmitUs, avgWaitUs, avgCopyUs);
    return true;
}
#else
bool RunAllDeviceProcesses(const Options&)
{
    std::cerr << "all8_process is only supported on Linux/POSIX platforms.\n";
    return false;
}
#endif

struct TestCase {
    TestType type;
    const char* name;
    bool (*run)(const Options&);
};

const std::vector<TestCase>& TestCases()
{
    static const std::vector<TestCase> cases = {
        {TestType::SingleStream, "single_stream", RunSingleDevice},
        {TestType::Batch, "batch", RunSingleDeviceBatch},
        {TestType::MultiStream, "multi_stream", RunSingleDeviceMultiStream},
    };
    return cases;
}

bool RunSelectedTests(const Options& options)
{
    bool ok = true;
    for (const auto& testCase : TestCases()) {
        if (options.testType == TestType::All || options.testType == testCase.type) {
            if (!testCase.run(options)) {
                ok = false;
                break;
            }
        }
    }
    return ok;
}

int DeviceResetCount(TestType type)
{
    (void)type;
    return 1;
}

}  // namespace

int main(int argc, char const* argv[])
{
    Options options;
    if (!ParseArgs(argc, argv, &options)) {
        return options.showHelp ? 0 : 1;
    }

    if (options.testType == TestType::All8Process) {
        return RunAllDeviceProcesses(options) ? 0 : 1;
    }
    if (options.testType == TestType::All8SingleStream) {
        return RunAllDevices(options) ? 0 : 1;
    }

    // aclInit initializes AscendCL runtime state for this process.
    if (!CHECK_ACL(aclInit(nullptr))) {
        std::cerr << "aclInit failed. Check that Ascend runtime is installed and configured.\n";
        return 1;
    }

    bool ok = true;

    if (ok) {
        ok = RunSelectedTests(options);
    }

    for (int device = 0; device < DeviceResetCount(options.testType); ++device) {
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

    if (ok && options.testType == TestType::All) {
        ok = RunAllDevices(options);
    }

    return ok ? 0 : 1;
}
