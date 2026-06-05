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
#ifndef COPY_BUFFER_ASCEND_H
#define COPY_BUFFER_ASCEND_H

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <vector>
#include "copy_buffer.h"
#include "error_handle_ascend.h"

#ifndef MFD_CLOEXEC
#define MFD_CLOEXEC 0x0001U
#endif

#ifndef MFD_HUGETLB
#define MFD_HUGETLB 0x0004U
#endif

#ifndef MFD_HUGE_SHIFT
#define MFD_HUGE_SHIFT 26
#endif

#ifndef MFD_HUGE_2MB
#define MFD_HUGE_2MB (21U << MFD_HUGE_SHIFT)
#endif

inline size_t CheckedTotalBytes(size_t size, size_t number)
{
    ASSERT(number == 0 || size <= std::numeric_limits<size_t>::max() / number);
    const auto total = size * number;
    ASSERT(total > 0);
    return total;
}

inline size_t RoundUpToHugePageSize(size_t bytes)
{
    constexpr size_t kHugePageSize = 2ull * 1024ull * 1024ull;
    const auto remainder = bytes % kHugePageSize;
    if (remainder == 0) { return bytes; }
    const auto padding = kHugePageSize - remainder;
    ASSERT(bytes <= std::numeric_limits<size_t>::max() - padding);
    return bytes + padding;
}

inline int CreateHugeTlbMemfd(const std::string& name)
{
#ifdef SYS_memfd_create
    return static_cast<int>(
        syscall(SYS_memfd_create, name.c_str(), MFD_CLOEXEC | MFD_HUGETLB | MFD_HUGE_2MB));
#else
    errno = ENOSYS;
    return -1;
#endif
}

class HostCopyBuffer : public CopyBuffer {
public:
    HostCopyBuffer(size_t device, size_t size, size_t number) : CopyBuffer{device, size, number}
    {
        const auto total = size * number;
        ASCEND_ASSERT(aclrtSetDevice(device_));
        ASCEND_ASSERT(aclrtMallocHost(&addr_, total));
        std::memset(addr_, 'h', total);
    }
    ~HostCopyBuffer() override
    {
        if (addr_) {
            ASCEND_ASSERT(aclrtSetDevice(device_));
            ASCEND_ASSERT(aclrtFreeHost(addr_));
        }
    }
    std::string Name() const override { return "acl::host::" + std::to_string(device_); }
};

class AnonymousCopyBuffer : public CopyBuffer {
public:
    AnonymousCopyBuffer(size_t device, size_t size, size_t number)
        : CopyBuffer{device, size, number}
    {
        const auto total = size * number;
        ASCEND_ASSERT(aclrtSetDevice(device_));
        constexpr auto prot = PROT_READ | PROT_WRITE;
        constexpr auto flags = MAP_ANONYMOUS | MAP_PRIVATE | MAP_POPULATE;
        addr_ = mmap(nullptr, total, prot, flags, -1, 0);
        ASSERT(addr_ != MAP_FAILED);
        std::memset(addr_, 'a', total);
        ASCEND_ASSERT(aclrtHostRegisterV2(addr_, total, ACL_HOST_REG_MAPPED | ACL_HOST_REG_PINNED));
    }
    ~AnonymousCopyBuffer() override
    {
        if (addr_) {
            ASCEND_ASSERT(aclrtSetDevice(device_));
            ASCEND_ASSERT(aclrtHostUnregister(addr_));
            munmap(addr_, size_ * number_);
        }
    }
    std::string Name() const override { return "acl::anon::" + std::to_string(device_); }
};

class HugeSharedCopyBuffer : public CopyBuffer {
public:
    HugeSharedCopyBuffer(size_t device, size_t size, size_t number)
        : CopyBuffer{device, size, number}
    {
        const auto total = CheckedTotalBytes(size, number);
        mappedBytes_ = RoundUpToHugePageSize(total);
        const auto name = "copy_ascend_" + std::to_string(getpid()) + "_" +
                          std::to_string(reinterpret_cast<std::uintptr_t>(this));
        fd_ = CreateHugeTlbMemfd(name);
        ASSERT(fd_ != -1);
        ASSERT(ftruncate(fd_, mappedBytes_) == 0);
        constexpr auto prot = PROT_READ | PROT_WRITE;
        constexpr auto flags = MAP_SHARED | MAP_POPULATE;
        addr_ = mmap(nullptr, mappedBytes_, prot, flags, fd_, 0);
        ASSERT(addr_ != MAP_FAILED);
        std::memset(addr_, 'g', total);
        ASCEND_ASSERT(aclrtSetDevice(device_));
        ASCEND_ASSERT(
            aclrtHostRegisterV2(addr_, mappedBytes_, ACL_HOST_REG_MAPPED | ACL_HOST_REG_PINNED));
    }

    ~HugeSharedCopyBuffer() override
    {
        if (addr_) {
            ASCEND_ASSERT(aclrtSetDevice(device_));
            ASCEND_ASSERT(aclrtHostUnregister(addr_));
            munmap(addr_, mappedBytes_);
            addr_ = nullptr;
        }
        if (fd_ != -1) {
            close(fd_);
            fd_ = -1;
        }
    }

    void* HostAt(size_t i) const { return At(i); }

    std::string Name() const override
    {
        return "acl::huge_shm::" + std::to_string(device_);
    }

private:
    int fd_ = -1;
    size_t mappedBytes_ = 0;
};

class MallocHostRegisterCopyBuffer : public CopyBuffer {
public:
    MallocHostRegisterCopyBuffer(size_t device, size_t size, size_t number)
        : CopyBuffer{device, size, number}
    {
        const auto total = size * number;
        ASCEND_ASSERT(aclrtSetDevice(device_));
        ASCEND_ASSERT(aclrtMallocHost(&hostAddr_, total));
        std::memset(hostAddr_, 'r', total);
        ASCEND_ASSERT(aclrtHostRegister(hostAddr_, total, ACL_HOST_REGISTER_MAPPED,
                                        &deviceAddr_));
        addr_ = deviceAddr_;
    }

    ~MallocHostRegisterCopyBuffer() override
    {
        if (hostAddr_) {
            ASCEND_ASSERT(aclrtSetDevice(device_));
            ASCEND_ASSERT(aclrtHostUnregister(hostAddr_));
            ASCEND_ASSERT(aclrtFreeHost(hostAddr_));
            hostAddr_ = nullptr;
            deviceAddr_ = nullptr;
            addr_ = nullptr;
        }
    }

    void* At(size_t i) const override
    {
        return static_cast<void*>(static_cast<char*>(deviceAddr_) + i * size_);
    }

    void* HostAt(size_t i) const
    {
        return static_cast<void*>(static_cast<char*>(hostAddr_) + i * size_);
    }

    std::string Name() const override { return "acl::host_reg::" + std::to_string(device_); }

private:
    void* hostAddr_ = nullptr;
    void* deviceAddr_ = nullptr;
};

class MallocHostRegisterV2CopyBuffer : public CopyBuffer {
public:
    MallocHostRegisterV2CopyBuffer(size_t device, size_t size, size_t number)
        : CopyBuffer{device, size, number}
    {
        const auto total = size * number;
        ASCEND_ASSERT(aclrtSetDevice(device_));
        ASCEND_ASSERT(aclrtMallocHost(&hostAddr_, total));
        std::memset(hostAddr_, 'v', total);
        ASCEND_ASSERT(aclrtHostRegisterV2(hostAddr_, total,
                                          ACL_HOST_REG_MAPPED | ACL_HOST_REG_PINNED));
        ASCEND_ASSERT(aclrtHostGetDevicePointer(hostAddr_, &deviceAddr_, 0));
        addr_ = deviceAddr_;
    }

    ~MallocHostRegisterV2CopyBuffer() override
    {
        if (hostAddr_) {
            ASCEND_ASSERT(aclrtSetDevice(device_));
            ASCEND_ASSERT(aclrtHostUnregister(hostAddr_));
            ASCEND_ASSERT(aclrtFreeHost(hostAddr_));
            hostAddr_ = nullptr;
            deviceAddr_ = nullptr;
            addr_ = nullptr;
        }
    }

    void* At(size_t i) const override
    {
        return static_cast<void*>(static_cast<char*>(deviceAddr_) + i * size_);
    }

    void* HostAt(size_t i) const
    {
        return static_cast<void*>(static_cast<char*>(hostAddr_) + i * size_);
    }

    std::string Name() const override { return "acl::host_regv2::" + std::to_string(device_); }

private:
    void* hostAddr_ = nullptr;
    void* deviceAddr_ = nullptr;
};

class DeviceCopyBuffer : public CopyBuffer {
public:
    DeviceCopyBuffer(size_t device, size_t size, size_t number) : CopyBuffer{device, size, number}
    {
        const auto total = size * number;
        ASCEND_ASSERT(aclrtSetDevice(device_));
        ASCEND_ASSERT(aclrtMalloc(&addr_, total, ACL_MEM_MALLOC_HUGE_FIRST));
        ASCEND_ASSERT(aclrtMemset(addr_, total, 'd', total));
    }
    ~DeviceCopyBuffer() override
    {
        if (addr_) {
            ASCEND_ASSERT(aclrtSetDevice(device_));
            ASCEND_ASSERT(aclrtFree(addr_));
        }
    }
    std::string Name() const override { return "acl::device::" + std::to_string(device_); }
};

class FragmentedDeviceCopyBuffer : public CopyBuffer {
public:
    FragmentedDeviceCopyBuffer(size_t device, size_t size, size_t number)
        : CopyBuffer{device, size, number}
    {
        fragments_.resize(number_);
        ASCEND_ASSERT(aclrtSetDevice(device_));
        for (auto& fragment : fragments_) {
            ASCEND_ASSERT(aclrtMalloc(&fragment, size_, ACL_MEM_MALLOC_HUGE_FIRST));
            ASCEND_ASSERT(aclrtMemset(fragment, size_, 'd', size_));
        }
        if (!fragments_.empty()) { addr_ = fragments_[0]; }
    }

    ~FragmentedDeviceCopyBuffer() override
    {
        ASCEND_ASSERT(aclrtSetDevice(device_));
        for (auto& fragment : fragments_) {
            if (fragment != nullptr) { ASCEND_ASSERT(aclrtFree(fragment)); }
        }
    }

    void* At(size_t i) const override
    {
        ASSERT(i < fragments_.size());
        return fragments_[i];
    }

    std::string Name() const override
    {
        return "acl::device_frag::" + std::to_string(device_);
    }

private:
    std::vector<void*> fragments_;
};

#endif  // COPY_BUFFER_ASCEND_H
