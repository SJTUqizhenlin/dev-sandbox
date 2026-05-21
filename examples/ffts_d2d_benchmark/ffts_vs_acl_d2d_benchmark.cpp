#include "acl/acl.h"

#if __has_include("runtime/rt_ffts_plus.h")
#include "runtime/rt_ffts_plus.h"
#elif __has_include("rt_external_ffts.h")
#include "rt_external_ffts.h"
#else
#include "ffts_plus_minimal_runtime.h"
#endif

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

namespace {

constexpr uint32_t kSdmaFp32AtomicMoveSqe = 0x1E70;
constexpr uint16_t kContextMaxNum = 128;
constexpr uint16_t kMaxReadyLanes = 8;
constexpr uint8_t kFftsCommunicationTask = 0x5A;
constexpr int kWarmupIterations = 5;
constexpr int kDefaultMeasureIterations = 128;
constexpr std::size_t kDefaultIoSize = 64 * 1024;
constexpr std::size_t kDefaultBufferCount = 1024;

static_assert(sizeof(rtFftsPlusComCtx_t) == 128, "rtFftsPlusComCtx_t must be 128 bytes");
static_assert(sizeof(rtFftsPlusSdmaCtx_t) == 128, "rtFftsPlusSdmaCtx_t must be 128 bytes");

enum class TestType {
    All,
    Merge,
    Split,
};

enum class CopyPath {
    Acl,
    Ffts,
    Both,
};

struct Options {
    uint32_t deviceId = 0;
    TestType testType = TestType::All;
    CopyPath copyPath = CopyPath::Both;
    std::size_t ioSize = kDefaultIoSize;
    std::size_t bufferCount = kDefaultBufferCount;
    std::size_t iterations = kDefaultMeasureIterations;
};

struct CopySpec {
    void *dst = nullptr;
    const void *src = nullptr;
    size_t size = 0;
};

struct Timing {
    double buildUs = 0.0;
    double submitUs = 0.0;
    double copyUs = 0.0;
};

struct Measurements {
    std::vector<double> buildUs;
    std::vector<double> submitUs;
    std::vector<double> copyUs;
    bool ok = true;
};

bool CheckAcl(aclError ret, const char *expr, const char *file, int line)
{
    if (ret == ACL_SUCCESS) {
        return true;
    }
    std::cerr << "ACL call failed: " << expr << "\n"
              << "  at " << file << ":" << line << "\n"
              << "  error code: " << ret << "\n";
    const char *msg = aclGetRecentErrMsg();
    if (msg != nullptr) {
        std::cerr << "  message: " << msg << "\n";
    }
    return false;
}

#define CHECK_ACL(expr) CheckAcl((expr), #expr, __FILE__, __LINE__)

bool CheckRt(rtError_t ret, const char *expr, const char *file, int line)
{
    if (ret == RT_ERROR_NONE) {
        return true;
    }
    std::cerr << "RT call failed: " << expr << "\n"
              << "  at " << file << ":" << line << "\n"
              << "  error code: " << ret << "\n";
    return false;
}

#define CHECK_RT(expr) CheckRt((expr), #expr, __FILE__, __LINE__)

double ElapsedUs(std::chrono::steady_clock::time_point begin,
                 std::chrono::steady_clock::time_point end)
{
    return std::chrono::duration<double, std::micro>(end - begin).count();
}

double BandwidthMBps(std::size_t bytes, double avgCopyUs)
{
    const double seconds = avgCopyUs / 1000000.0;
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
    for (char &ch : text) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        if (ch == '-') {
            ch = '_';
        }
    }
    return text;
}

const char *TestTypeName(TestType type)
{
    switch (type) {
        case TestType::All:
            return "all";
        case TestType::Merge:
            return "merge";
        case TestType::Split:
            return "split";
    }
    return "unknown";
}

const char *CopyPathName(CopyPath path)
{
    switch (path) {
        case CopyPath::Acl:
            return "acl";
        case CopyPath::Ffts:
            return "ffts";
        case CopyPath::Both:
            return "both";
    }
    return "unknown";
}

bool ParseTestType(const std::string &text, TestType *type)
{
    if (type == nullptr) {
        return false;
    }
    const std::string name = NormalizeName(text);
    if (name == "all") {
        *type = TestType::All;
        return true;
    }
    if (name == "merge") {
        *type = TestType::Merge;
        return true;
    }
    if (name == "split") {
        *type = TestType::Split;
        return true;
    }
    return false;
}

bool ParseCopyPath(const std::string &text, CopyPath *path)
{
    if (path == nullptr) {
        return false;
    }
    const std::string name = NormalizeName(text);
    if (name == "acl") {
        *path = CopyPath::Acl;
        return true;
    }
    if (name == "ffts") {
        *path = CopyPath::Ffts;
        return true;
    }
    if (name == "both") {
        *path = CopyPath::Both;
        return true;
    }
    return false;
}

bool ParseDeviceId(const std::string &text, uint32_t *deviceId)
{
    if (text.empty() || deviceId == nullptr) {
        return false;
    }
    try {
        std::size_t pos = 0;
        const unsigned long long parsed = std::stoull(text, &pos, 10);
        if (pos != text.size() || parsed > std::numeric_limits<uint32_t>::max()) {
            return false;
        }
        *deviceId = static_cast<uint32_t>(parsed);
        return true;
    } catch (...) {
        return false;
    }
}

bool ParseSize(const std::string &text, std::size_t *value)
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
    try {
        std::size_t pos = 0;
        const std::size_t parsed = std::stoull(number, &pos, 10);
        if (pos != number.size() || parsed == 0) {
            return false;
        }
        *value = parsed * multiplier;
        return true;
    } catch (...) {
        return false;
    }
}

void PrintUsage(const char *prog)
{
    std::cout << "Usage: " << (prog != nullptr ? prog : "ffts_vs_acl_d2d_benchmark")
              << " [-t <test_type>] [-p <copy_path>] [-s <io_size>] [-n <buffer_count>]"
              << " [-i <iterations>] [-d <device_id>] [-h]\n"
              << "\n"
              << "Options:\n"
              << "  -t <test_type>   Test type: all, merge, split. Default: "
              << TestTypeName(TestType::All) << "\n"
              << "  -p <copy_path>   Copy path: acl, ffts, both. Default: "
              << CopyPathName(CopyPath::Both) << "\n"
              << "  -s <io_size>     Bytes per buffer (suffixes K/M/G). Default: "
              << SizeLabel(kDefaultIoSize) << "\n"
              << "  -n <buffer_count> Number of buffers per iteration. Default: "
              << kDefaultBufferCount << "\n"
              << "  -i <iterations>  Number of measured iterations. Default: "
              << kDefaultMeasureIterations << "\n"
              << "  -d <device_id>   Device ID. Default: 0\n"
              << "  -h               Show this help message.\n";
}

bool ParseArgs(int argc, char const *argv[], Options *options)
{
    if (options == nullptr) {
        return false;
    }
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "-h" || arg == "--help") {
            PrintUsage(argv[0]);
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
        if (arg == "-p") {
            if (i + 1 >= argc || !ParseCopyPath(argv[++i], &options->copyPath)) {
                std::cerr << "Invalid value for -p.\n";
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
            if (i + 1 >= argc || !ParseDeviceId(argv[++i], &options->deviceId)) {
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

static uint64_t PtrToU64(const void *ptr)
{
    return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(ptr));
}

static void *ByteOffset(void *ptr, size_t offset)
{
    return reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(ptr) + offset);
}

static const void *ByteOffset(const void *ptr, size_t offset)
{
    return reinterpret_cast<const void *>(reinterpret_cast<uintptr_t>(ptr) + offset);
}

static std::vector<uint8_t> MakePattern(size_t fragmentIndex, size_t size)
{
    std::vector<uint8_t> data(size);
    for (size_t i = 0; i < size; ++i) {
        data[i] = static_cast<uint8_t>((fragmentIndex * 17 + i * 31 + (i >> 8)) & 0xFF);
    }
    return data;
}

static bool CopyH2D(void *dstDevice, const std::vector<uint8_t> &srcHost)
{
    if (!CHECK_ACL(aclrtMemcpy(dstDevice, srcHost.size(), srcHost.data(), srcHost.size(),
                                ACL_MEMCPY_HOST_TO_DEVICE))) {
        return false;
    }
    return true;
}

static bool CopyD2H(std::vector<uint8_t> *dstHost, const void *srcDevice)
{
    if (!CHECK_ACL(aclrtMemcpy(dstHost->data(), dstHost->size(), srcDevice, dstHost->size(),
                                ACL_MEMCPY_DEVICE_TO_HOST))) {
        return false;
    }
    return true;
}

class DeviceBuffer {
public:
    DeviceBuffer() = default;
    DeviceBuffer(const DeviceBuffer &) = delete;
    DeviceBuffer &operator=(const DeviceBuffer &) = delete;

    DeviceBuffer(DeviceBuffer &&other) noexcept
    {
        ptr_ = other.ptr_;
        size_ = other.size_;
        other.ptr_ = nullptr;
        other.size_ = 0;
    }

    DeviceBuffer &operator=(DeviceBuffer &&other) noexcept
    {
        if (this != &other) {
            Free();
            ptr_ = other.ptr_;
            size_ = other.size_;
            other.ptr_ = nullptr;
            other.size_ = 0;
        }
        return *this;
    }

    ~DeviceBuffer()
    {
        Free();
    }

    bool Alloc(size_t size)
    {
        Free();
        if (!CHECK_ACL(aclrtMalloc(&ptr_, size, ACL_MEM_MALLOC_HUGE_FIRST))) {
            return false;
        }
        size_ = size;
        return true;
    }

    bool MemsetZero()
    {
        if (ptr_ == nullptr || size_ == 0) {
            return false;
        }
        if (!CHECK_ACL(aclrtMemset(ptr_, size_, 0, size_))) {
            return false;
        }
        return true;
    }

    void *Ptr() const
    {
        return ptr_;
    }

    size_t Size() const
    {
        return size_;
    }

private:
    void Free()
    {
        if (ptr_ != nullptr) {
            aclrtFree(ptr_);
            ptr_ = nullptr;
            size_ = 0;
        }
    }

    void *ptr_ = nullptr;
    size_t size_ = 0;
};

class MiniFftsD2DDispatcher {
public:
    void Reserve(size_t count)
    {
        contexts_.reserve(count);
    }

    void Reset()
    {
        contexts_.clear();
        completed_ = false;
    }

    bool AddMemcpy(void *dst, const void *src, size_t size, uint32_t *taskId)
    {
        if (completed_) {
            std::cerr << "Cannot add memcpy after launch. Call Reset first.\n";
            return false;
        }
        if (dst == nullptr || src == nullptr || taskId == nullptr) {
            std::cerr << "Invalid null pointer in AddMemcpy.\n";
            return false;
        }
        if (size == 0 || size > std::numeric_limits<uint32_t>::max()) {
            std::cerr << "Invalid memcpy size: " << size << "\n";
            return false;
        }
        if (contexts_.size() >= std::numeric_limits<uint16_t>::max()) {
            std::cerr << "Too many FFTS contexts: " << contexts_.size() << "\n";
            return false;
        }

        rtFftsPlusComCtx_t comCtx{};
        auto *sdmaCtx = reinterpret_cast<rtFftsPlusSdmaCtx_t *>(&comCtx);
        BuildSdmaCtx(dst, src, size, sdmaCtx);
        contexts_.push_back(comCtx);
        *taskId = static_cast<uint32_t>(contexts_.size() - 1);
        return true;
    }

    bool AddDependency(uint32_t predecessorId, uint32_t successorId)
    {
        if (predecessorId >= contexts_.size() || successorId >= contexts_.size()) {
            std::cerr << "Invalid dependency: " << predecessorId << " -> " << successorId
                      << ", ctxCount=" << contexts_.size() << "\n";
            return false;
        }

        auto &predecessor = contexts_[predecessorId];
        auto &successor = contexts_[successorId];
        if (predecessor.successorNum >= RT_CTX_SUCCESSOR_NUM) {
            std::cerr << "Too many successors for ctx " << predecessorId << "\n";
            return false;
        }
        if (successor.predCntInit == std::numeric_limits<uint8_t>::max()) {
            std::cerr << "Too many predecessors for ctx " << successorId << "\n";
            return false;
        }

        predecessor.successorList[predecessor.successorNum] = static_cast<uint16_t>(successorId);
        predecessor.successorNum++;
        successor.predCntInit++;
        successor.predCnt++;
        return true;
    }

    bool Launch(aclrtStream stream, uint16_t readyContextNum)
    {
        if (contexts_.empty()) {
            std::cerr << "No FFTS contexts to launch.\n";
            return false;
        }
        if (readyContextNum == 0 || readyContextNum > contexts_.size()) {
            std::cerr << "Invalid readyContextNum=" << readyContextNum
                      << ", ctxCount=" << contexts_.size() << "\n";
            return false;
        }

        rtFftsPlusSqe_t sqe{};
        sqe.fftsType = RT_FFTS_PLUS_TYPE;
        sqe.totalContextNum = static_cast<uint16_t>(contexts_.size());
        sqe.readyContextNum = readyContextNum;
        sqe.preloadContextNum = std::min<uint16_t>(readyContextNum, kContextMaxNum);
        sqe.timeout = 0;
        sqe.subType = kFftsCommunicationTask;

        rtFftsPlusTaskInfo_t task{};
        task.fftsPlusSqe = &sqe;
        task.descBuf = contexts_.data();
        task.descBufLen = sizeof(rtFftsPlusComCtx_t) * contexts_.size();
        task.descAddrType = RT_FFTS_PLUS_CTX_DESC_ADDR_TYPE_HOST;
        task.argsHandleInfoNum = 0;
        task.argsHandleInfoPtr = nullptr;

        completed_ = true;
        if (!CHECK_RT(rtFftsPlusTaskLaunchWithFlag(&task, reinterpret_cast<rtStream_t>(stream), 0))) {
            return false;
        }
        return true;
    }

private:
    static void BuildSdmaCtx(void *dst, const void *src, size_t size, rtFftsPlusSdmaCtx_t *ctx)
    {
        constexpr uint32_t kShift = 32;
        constexpr uint64_t kLowMask = 0xFFFFFFFFULL;

        const uint64_t srcAddr = PtrToU64(src);
        const uint64_t dstAddr = PtrToU64(dst);

        ctx->contextType = RT_CTX_TYPE_SDMA;
        ctx->threadDim = 1;
        ctx->sdmaSqeHeader = kSdmaFp32AtomicMoveSqe;
        ctx->sourceAddressBaseL = static_cast<uint32_t>(srcAddr & kLowMask);
        ctx->sourceAddressBaseH = static_cast<uint32_t>(srcAddr >> kShift);
        ctx->sourceAddressOffset = 0;
        ctx->destinationAddressBaseL = static_cast<uint32_t>(dstAddr & kLowMask);
        ctx->destinationAddressBaseH = static_cast<uint32_t>(dstAddr >> kShift);
        ctx->destinationAddressOffset = 0;
        ctx->nonTailDataLength = static_cast<uint32_t>(size);
        ctx->tailDataLength = static_cast<uint32_t>(size);
    }

    std::vector<rtFftsPlusComCtx_t> contexts_;
    bool completed_ = false;
};

static uint16_t BuildFftsCopies(MiniFftsD2DDispatcher *dispatcher, const std::vector<CopySpec> &copies)
{
    if (dispatcher == nullptr || copies.empty()) {
        return 0;
    }

    const uint16_t laneCount = static_cast<uint16_t>(std::min<size_t>(copies.size(), kMaxReadyLanes));
    std::vector<int32_t> lastTaskId(laneCount, -1);

    for (size_t i = 0; i < copies.size(); ++i) {
        uint32_t taskId = 0;
        if (!dispatcher->AddMemcpy(copies[i].dst, copies[i].src, copies[i].size, &taskId)) {
            return 0;
        }

        const size_t lane = i % laneCount;
        if (lastTaskId[lane] >= 0) {
            if (!dispatcher->AddDependency(static_cast<uint32_t>(lastTaskId[lane]), taskId)) {
                return 0;
            }
        }
        lastTaskId[lane] = static_cast<int32_t>(taskId);
    }

    return laneCount;
}

class MergeCase {
public:
    bool Init(size_t fragmentCount, size_t fragmentBytes)
    {
        fragmentCount_ = fragmentCount;
        fragmentBytes_ = fragmentBytes;
        totalBytes_ = fragmentCount * fragmentBytes;
        srcBuffers_.resize(fragmentCount);
        hostFragments_.reserve(fragmentCount);
        copies_.reserve(fragmentCount);

        if (!transferBuffer_.Alloc(totalBytes_)) {
            return false;
        }

        for (size_t i = 0; i < fragmentCount; ++i) {
            hostFragments_.push_back(MakePattern(i, fragmentBytes));
            if (!srcBuffers_[i].Alloc(fragmentBytes)) {
                return false;
            }
            if (!CopyH2D(srcBuffers_[i].Ptr(), hostFragments_[i])) {
                return false;
            }
            copies_.push_back({ByteOffset(transferBuffer_.Ptr(), i * fragmentBytes), srcBuffers_[i].Ptr(), fragmentBytes});
        }
        return true;
    }

    bool ResetOutput()
    {
        return transferBuffer_.MemsetZero();
    }

    bool Validate()
    {
        std::vector<uint8_t> merged(totalBytes_);
        if (!CopyD2H(&merged, transferBuffer_.Ptr())) {
            return false;
        }
        for (size_t i = 0; i < fragmentCount_; ++i) {
            const uint8_t *actual = merged.data() + i * fragmentBytes_;
            if (std::memcmp(actual, hostFragments_[i].data(), fragmentBytes_) != 0) {
                std::cerr << "Merge validation failed at fragment " << i << "\n";
                return false;
            }
        }
        return true;
    }

    const std::vector<CopySpec> &Copies() const
    {
        return copies_;
    }

    size_t TotalBytes() const
    {
        return totalBytes_;
    }

private:
    size_t fragmentCount_ = 0;
    size_t fragmentBytes_ = 0;
    size_t totalBytes_ = 0;
    DeviceBuffer transferBuffer_;
    std::vector<DeviceBuffer> srcBuffers_;
    std::vector<std::vector<uint8_t>> hostFragments_;
    std::vector<CopySpec> copies_;
};

class SplitCase {
public:
    bool Init(size_t fragmentCount, size_t fragmentBytes)
    {
        fragmentCount_ = fragmentCount;
        fragmentBytes_ = fragmentBytes;
        totalBytes_ = fragmentCount * fragmentBytes;
        dstBuffers_.resize(fragmentCount);
        hostFragments_.reserve(fragmentCount);
        copies_.reserve(fragmentCount);

        std::vector<uint8_t> transferHost(totalBytes_);
        for (size_t i = 0; i < fragmentCount; ++i) {
            hostFragments_.push_back(MakePattern(i, fragmentBytes));
            std::memcpy(transferHost.data() + i * fragmentBytes, hostFragments_[i].data(), fragmentBytes);
        }

        if (!transferBuffer_.Alloc(totalBytes_)) {
            return false;
        }
        if (!CopyH2D(transferBuffer_.Ptr(), transferHost)) {
            return false;
        }

        for (size_t i = 0; i < fragmentCount; ++i) {
            if (!dstBuffers_[i].Alloc(fragmentBytes)) {
                return false;
            }
            copies_.push_back({dstBuffers_[i].Ptr(), ByteOffset(transferBuffer_.Ptr(), i * fragmentBytes), fragmentBytes});
        }
        return true;
    }

    bool ResetOutput()
    {
        for (auto &buffer : dstBuffers_) {
            if (!buffer.MemsetZero()) {
                return false;
            }
        }
        return true;
    }

    bool Validate()
    {
        for (size_t i = 0; i < fragmentCount_; ++i) {
            std::vector<uint8_t> actual(fragmentBytes_);
            if (!CopyD2H(&actual, dstBuffers_[i].Ptr())) {
                return false;
            }
            if (actual != hostFragments_[i]) {
                std::cerr << "Split validation failed at fragment " << i << "\n";
                return false;
            }
        }
        return true;
    }

    const std::vector<CopySpec> &Copies() const
    {
        return copies_;
    }

    size_t TotalBytes() const
    {
        return totalBytes_;
    }

private:
    size_t fragmentCount_ = 0;
    size_t fragmentBytes_ = 0;
    size_t totalBytes_ = 0;
    DeviceBuffer transferBuffer_;
    std::vector<DeviceBuffer> dstBuffers_;
    std::vector<std::vector<uint8_t>> hostFragments_;
    std::vector<CopySpec> copies_;
};

static bool SubmitAclCopies(const std::vector<CopySpec> &copies, aclrtStream stream)
{
    for (const auto &copy : copies) {
        if (!CHECK_ACL(aclrtMemcpyAsync(copy.dst, copy.size, copy.src, copy.size,
                                         ACL_MEMCPY_DEVICE_TO_DEVICE, stream))) {
            return false;
        }
    }
    return true;
}

bool MeasureCopyOnce(const std::vector<CopySpec> &copies, const std::string &path,
                     aclrtStream stream, aclrtEvent startEvent, aclrtEvent stopEvent,
                     Timing *timing)
{
    if (timing == nullptr) {
        return false;
    }

    double buildUs = 0.0;
    uint16_t readyCount = 0;
    MiniFftsD2DDispatcher dispatcher;

    if (path == "ffts") {
        dispatcher.Reserve(copies.size());
        auto buildBegin = std::chrono::steady_clock::now();
        readyCount = BuildFftsCopies(&dispatcher, copies);
        auto buildEnd = std::chrono::steady_clock::now();
        buildUs = ElapsedUs(buildBegin, buildEnd);
        if (readyCount == 0) {
            return false;
        }
    }

    if (!CHECK_ACL(aclrtRecordEvent(startEvent, stream))) {
        return false;
    }

    auto submitBegin = std::chrono::steady_clock::now();
    if (path == "acl") {
        if (!SubmitAclCopies(copies, stream)) {
            return false;
        }
    } else {
        if (!dispatcher.Launch(stream, readyCount)) {
            return false;
        }
    }
    auto submitEnd = std::chrono::steady_clock::now();

    if (!CHECK_ACL(aclrtRecordEvent(stopEvent, stream))) {
        return false;
    }
    if (!CHECK_ACL(aclrtSynchronizeStream(stream))) {
        return false;
    }

    float elapsedMs = 0.0f;
    if (!CHECK_ACL(aclrtEventElapsedTime(&elapsedMs, startEvent, stopEvent))) {
        return false;
    }

    timing->buildUs = buildUs;
    timing->submitUs = ElapsedUs(submitBegin, submitEnd);
    timing->copyUs = static_cast<double>(elapsedMs) * 1000.0;
    return true;
}

bool RunMeasuredPath(const std::vector<CopySpec> &copies, const std::string &path,
                    aclrtStream stream, size_t iterations, Measurements *out)
{
    if (out == nullptr || copies.empty()) {
        return false;
    }

    aclrtEvent startEvent = nullptr;
    aclrtEvent stopEvent = nullptr;
    if (!CHECK_ACL(aclrtCreateEvent(&startEvent))) {
        return false;
    }
    if (!CHECK_ACL(aclrtCreateEvent(&stopEvent))) {
        aclrtDestroyEvent(startEvent);
        return false;
    }

    bool ok = true;
    for (int i = 0; i < kWarmupIterations; ++i) {
        Timing timing;
        if (!MeasureCopyOnce(copies, path, stream, startEvent, stopEvent, &timing)) {
            ok = false;
            break;
        }
    }

    double totalBuildUs = 0.0;
    double totalSubmitUs = 0.0;
    double totalCopyUs = 0.0;
    if (ok) {
        for (size_t i = 0; i < iterations; ++i) {
            Timing timing;
            if (!MeasureCopyOnce(copies, path, stream, startEvent, stopEvent, &timing)) {
                ok = false;
                break;
            }
            totalBuildUs += timing.buildUs;
            totalSubmitUs += timing.submitUs;
            totalCopyUs += timing.copyUs;
        }
    }

    if (startEvent != nullptr) {
        ok = CHECK_ACL(aclrtDestroyEvent(startEvent)) && ok;
    }
    if (stopEvent != nullptr) {
        ok = CHECK_ACL(aclrtDestroyEvent(stopEvent)) && ok;
    }

    if (!ok) {
        return false;
    }

    const double avgBuildUs = totalBuildUs / static_cast<double>(iterations);
    const double avgSubmitUs = totalSubmitUs / static_cast<double>(iterations);
    const double avgCopyUs = totalCopyUs / static_cast<double>(iterations);

    out->buildUs.push_back(avgBuildUs);
    out->submitUs.push_back(avgSubmitUs);
    out->copyUs.push_back(avgCopyUs);
    out->ok = true;
    return true;
}

void PrintTableHeader(const std::string &title)
{
    std::cout << title << "\n\n"
              << std::left << std::setw(10) << "Dir" << std::setw(6) << "Path"
              << std::right << std::setw(12) << "Size" << std::setw(8) << "Count"
              << std::setw(14) << "Build(us)" << std::setw(14) << "Submit(us)"
              << std::setw(14) << "Copy(us)"
              << std::setw(16) << "Build/IO(us)" << std::setw(16) << "Submit/IO(us)"
              << std::setw(14) << "Copy/IO(us)" << std::setw(16) << "BW(MB/s)"
              << std::setw(6) << "Pass" << "\n";
    std::cout << std::string(136, '-') << "\n";
}

void PrintTableRow(const std::string &direction, const std::string &path,
                   std::size_t ioSize, std::size_t count, std::size_t totalBytes,
                   double avgBuildUs, double avgSubmitUs, double avgCopyUs, bool pass)
{
    const auto countAsDouble = static_cast<double>(count);
    const double buildPerIoUs = avgBuildUs / countAsDouble;
    const double submitPerIoUs = avgSubmitUs / countAsDouble;
    const double copyPerIoUs = avgCopyUs / countAsDouble;

    std::cout << std::left << std::setw(10) << direction << std::setw(6) << path
              << std::right << std::setw(12) << SizeLabel(ioSize) << std::setw(8) << count
              << std::fixed << std::setprecision(3)
              << std::setw(14) << avgBuildUs << std::setw(14) << avgSubmitUs
              << std::setw(14) << avgCopyUs
              << std::setw(16) << buildPerIoUs << std::setw(16) << submitPerIoUs
              << std::setw(14) << copyPerIoUs << std::setprecision(2)
              << std::setw(16) << BandwidthMBps(totalBytes, avgCopyUs)
              << std::setw(6) << (pass ? "yes" : "no") << "\n";
}

template <typename CaseT>
bool RunOnePath(const std::string &direction, const std::string &path,
                CaseT *testCase, aclrtStream stream, const Options &opt)
{
    if (!testCase->ResetOutput()) {
        return false;
    }

    Measurements measurements;
    bool runOk = RunMeasuredPath(testCase->Copies(), path, stream, opt.iterations, &measurements);
    bool validateOk = false;
    if (runOk) {
        validateOk = testCase->Validate();
    }

    PrintTableRow(direction, path, opt.ioSize, opt.bufferCount, testCase->TotalBytes(),
                  measurements.buildUs.empty() ? 0.0 : measurements.buildUs[0],
                  measurements.submitUs.empty() ? 0.0 : measurements.submitUs[0],
                  measurements.copyUs.empty() ? 0.0 : measurements.copyUs[0],
                  runOk && validateOk);

    return runOk && validateOk;
}

bool ShouldRun(CopyPath selected, CopyPath value)
{
    return selected == CopyPath::Both || selected == value;
}

bool ShouldRun(TestType selected, TestType value)
{
    return selected == TestType::All || selected == value;
}

bool RunMergeCase(aclrtStream stream, const Options &opt)
{
    MergeCase testCase;
    if (!testCase.Init(opt.bufferCount, opt.ioSize)) {
        return false;
    }
    bool ok = true;
    if (ShouldRun(opt.copyPath, CopyPath::Acl)) {
        ok = RunOnePath("merge", "acl", &testCase, stream, opt) && ok;
    }
    if (ShouldRun(opt.copyPath, CopyPath::Ffts)) {
        ok = RunOnePath("merge", "ffts", &testCase, stream, opt) && ok;
    }
    return ok;
}

bool RunSplitCase(aclrtStream stream, const Options &opt)
{
    SplitCase testCase;
    if (!testCase.Init(opt.bufferCount, opt.ioSize)) {
        return false;
    }
    bool ok = true;
    if (ShouldRun(opt.copyPath, CopyPath::Acl)) {
        ok = RunOnePath("split", "acl", &testCase, stream, opt) && ok;
    }
    if (ShouldRun(opt.copyPath, CopyPath::Ffts)) {
        ok = RunOnePath("split", "ffts", &testCase, stream, opt) && ok;
    }
    return ok;
}

} // namespace

int main(int argc, char const *argv[])
{
    Options options;
    if (!ParseArgs(argc, argv, &options)) {
        return 1;
    }

    if (!CHECK_ACL(aclInit(nullptr))) {
        std::cerr << "aclInit failed. Check that Ascend runtime is installed and configured.\n";
        return 1;
    }

    bool ok = true;
    bool deviceSet = false;
    aclrtStream stream = nullptr;

    if (!CHECK_ACL(aclrtSetDevice(options.deviceId))) {
        aclFinalize();
        return 1;
    }
    deviceSet = true;

    if (!CHECK_ACL(aclrtCreateStream(&stream))) {
        ok = false;
    }

    if (ok) {
        std::string title = "FFTS vs ACL D2D benchmark (device=" +
                            std::to_string(options.deviceId) + ", warmup=" +
                            std::to_string(kWarmupIterations) + ", iterations=" +
                            std::to_string(options.iterations) + ")";
        PrintTableHeader(title);

        if (ShouldRun(options.testType, TestType::Merge)) {
            ok = RunMergeCase(stream, options) && ok;
        }
        if (ShouldRun(options.testType, TestType::Split)) {
            ok = RunSplitCase(stream, options) && ok;
        }
    }

    if (stream != nullptr) {
        ok = CHECK_ACL(aclrtDestroyStream(stream)) && ok;
    }
    if (deviceSet) {
        ok = CHECK_ACL(aclrtResetDevice(options.deviceId)) && ok;
    }
    ok = CHECK_ACL(aclFinalize()) && ok;

    return ok ? 0 : 1;
}