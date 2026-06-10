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
#include "copy_buffer_ascend.h"
#include "copy_case.h"
#include "copy_instance_ascend.h"

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <linux/mempolicy.h>
#include <memory>
#include <sched.h>
#include <sstream>
#include <stdexcept>
#include <string>
#include <sys/syscall.h>
#include <thread>
#include <unistd.h>

namespace {

std::vector<std::string> SplitString(const std::string& text, char delimiter)
{
    std::vector<std::string> values;
    std::stringstream stream{text};
    std::string value;
    while (std::getline(stream, value, delimiter)) { values.push_back(value); }
    return values;
}

std::string DeviceConfigValue(const char* envName, size_t device, char delimiter)
{
    const char* env = std::getenv(envName);
    if (env == nullptr || env[0] == '\0') { return {}; }
    const auto values = SplitString(env, delimiter);
    if (device >= values.size()) { return {}; }
    return values[device];
}

void BindCurrentThreadToCpus(const std::string& cpuList)
{
    cpu_set_t cpuSet;
    CPU_ZERO(&cpuSet);
    bool hasCpu = false;
    for (const auto& item : SplitString(cpuList, ',')) {
        if (item.empty()) { continue; }
        const auto dash = item.find('-');
        const auto begin = std::stoul(item.substr(0, dash));
        const auto end = dash == std::string::npos ? begin : std::stoul(item.substr(dash + 1));
        for (size_t cpu = begin; cpu <= end; cpu++) {
            CPU_SET(cpu, &cpuSet);
            hasCpu = true;
        }
    }
    if (!hasCpu) { return; }
    if (sched_setaffinity(0, sizeof(cpuSet), &cpuSet) != 0) {
        throw std::runtime_error("sched_setaffinity failed: " + std::string(std::strerror(errno)));
    }
}

void BindCurrentThreadMemoryToNode(const std::string& nodeText)
{
    if (nodeText.empty()) { return; }
    const auto node = std::stoul(nodeText);
    if (node >= sizeof(unsigned long) * 8) {
        throw std::runtime_error("NUMA node is too large for the local nodemask");
    }
    unsigned long nodeMask = 1ul << node;
    if (syscall(SYS_set_mempolicy, MPOL_BIND, &nodeMask, sizeof(nodeMask) * 8) != 0) {
        throw std::runtime_error("set_mempolicy failed: " + std::string(std::strerror(errno)));
    }
}

void ApplyHugeShmThreadBinding(size_t device)
{
    const auto cpuList = DeviceConfigValue("COPY_HUGE_SHM_DEVICE_CPUS", device, ';');
    if (!cpuList.empty()) { BindCurrentThreadToCpus(cpuList); }

    const auto numaNode = DeviceConfigValue("COPY_HUGE_SHM_DEVICE_NUMA", device, ',');
    if (!numaNode.empty()) { BindCurrentThreadMemoryToNode(numaNode); }
}

}  // namespace

DEFINE_COPY_CASE(Host2DeviceCECase, "host_to_device_ce",
                 "memcpy from host to device with ce one by one", ctx)
{
    CopyResult result;
    for (size_t device = 0; device < ctx.nDevice; device++) {
        HostCopyBuffer srcBuffer{device, ctx.size, ctx.num};
        DeviceCopyBuffer dstBuffer{device, ctx.size, ctx.num};
        H2DCECopyInstance instance{ctx.iter, false};
        result.Push(instance.DoCopy(&srcBuffer, &dstBuffer));
    }
    result.Show("[[ " + Key() + " ]] " + Brief());
}

DEFINE_COPY_CASE(Host2DeviceBatchCECase, "host_to_device_batch_ce",
                 "memcpy from host to device with batch ce one by one", ctx)
{
    CopyResult result;
    for (size_t device = 0; device < ctx.nDevice; device++) {
        HostCopyBuffer srcBuffer{device, ctx.size, ctx.num};
        DeviceCopyBuffer dstBuffer{device, ctx.size, ctx.num};
        H2DBatchCECopyInstance instance{ctx.iter, false, device};
        result.Push(instance.DoCopy(&srcBuffer, &dstBuffer));
    }
    result.Show("[[ " + Key() + " ]] " + Brief());
}

DEFINE_COPY_CASE(OneHost2AllDeviceCECase, "one_host_to_all_device_ce",
                 "memcpy from one host to all device with ce", ctx)
{
    CopyResult result;
    HostCopyBuffer srcBuffer{0, ctx.size, ctx.num};
    for (size_t device = 0; device < ctx.nDevice; device++) {
        DeviceCopyBuffer dstBuffer{device, ctx.size, ctx.num};
        H2DCECopyInstance instance{ctx.iter, false};
        result.Push(instance.DoCopy(&srcBuffer, &dstBuffer));
    }
    result.Show("[[ " + Key() + " ]] " + Brief());
}

DEFINE_COPY_CASE(HugeShm2DeviceCECase, "huge_shm_to_device_ce",
                 "memcpy from HugeTLB shared host memory to device with ce one by one", ctx)
{
    if (!ctx.hugeShmParallel) {
        CopyResult result;
        for (size_t device = 0; device < ctx.nDevice; device++) {
            HugeSharedCopyBuffer srcBuffer{device, ctx.size, ctx.num};
            DeviceCopyBuffer dstBuffer{device, ctx.size, ctx.num};
            H2DCECopyInstance instance{ctx.iter, false};
            result.Push(instance.DoCopy(&srcBuffer, &dstBuffer));
        }
        result.Show("[[ " + Key() + " ]] " + Brief());
        return;
    }

    std::vector<std::unique_ptr<CopyResult::Result>> results(ctx.nDevice);
    std::vector<std::exception_ptr> errors(ctx.nDevice);
    std::vector<std::thread> threads;
    threads.reserve(ctx.nDevice);
    for (size_t device = 0; device < ctx.nDevice; device++) {
        threads.emplace_back([&, device]() {
            try {
                ApplyHugeShmThreadBinding(device);
                HugeSharedCopyBuffer srcBuffer{device, ctx.size, ctx.num};
                DeviceCopyBuffer dstBuffer{device, ctx.size, ctx.num};
                H2DCECopyInstance instance{ctx.iter, false};
                results[device] = std::make_unique<CopyResult::Result>(
                    instance.DoCopy(&srcBuffer, &dstBuffer));
            } catch (...) {
                errors[device] = std::current_exception();
            }
        });
    }
    for (auto& thread : threads) { thread.join(); }
    for (const auto& error : errors) {
        if (error) { std::rethrow_exception(error); }
    }

    CopyResult result;
    for (size_t device = 0; device < ctx.nDevice; device++) {
        result.Push(std::move(*results[device]));
    }
    result.Show("[[ " + Key() + " ]] " + Brief());
}

DEFINE_COPY_CASE(OneHugeShm2AllDeviceCECase, "one_huge_shm_to_all_device_ce",
                 "memcpy from one HugeTLB shared host memory to all device with ce", ctx)
{
    CopyResult result;
    HugeSharedCopyBuffer srcBuffer{0, ctx.size, ctx.num};
    for (size_t device = 0; device < ctx.nDevice; device++) {
        DeviceCopyBuffer dstBuffer{device, ctx.size, ctx.num};
        H2DCECopyInstance instance{ctx.iter, false};
        result.Push(instance.DoCopy(&srcBuffer, &dstBuffer));
    }
    result.Show("[[ " + Key() + " ]] " + Brief());
}

DEFINE_COPY_CASE(AllHost2AllDeviceCECase, "all_host_to_all_device_ce",
                 "memcpy from all host to all device with ce at one time", ctx)
{
    std::vector<const CopyBuffer*> srcBuffers(ctx.nDevice);
    std::vector<const CopyBuffer*> dstBuffers(ctx.nDevice);
    for (size_t device = 0; device < ctx.nDevice; device++) {
        srcBuffers[device] = new HostCopyBuffer{device, ctx.size, ctx.num};
        dstBuffers[device] = new DeviceCopyBuffer{device, ctx.size, ctx.num};
    }
    H2DCECopyInstance instance{ctx.iter, false};
    CopyResult result;
    result.Push(instance.DoCopyBatch(srcBuffers, dstBuffers));
    for (size_t device = 0; device < ctx.nDevice; device++) {
        delete srcBuffers[device];
        delete dstBuffers[device];
    }
    result.Show("[[ " + Key() + " ]] " + Brief());
}

DEFINE_COPY_CASE(AllHost2AllDeviceCEMultiThreadCase, "all_host_to_all_device_ce_multi_thread",
                 "memcpy from all host to all device with ce using one submit thread per device",
                 ctx)
{
    std::vector<const CopyBuffer*> srcBuffers(ctx.nDevice);
    std::vector<const CopyBuffer*> dstBuffers(ctx.nDevice);
    for (size_t device = 0; device < ctx.nDevice; device++) {
        srcBuffers[device] = new HostCopyBuffer{device, ctx.size, ctx.num};
        dstBuffers[device] = new DeviceCopyBuffer{device, ctx.size, ctx.num};
    }
    H2DCEParallelSubmitCopyInstance instance{ctx.iter, false};
    CopyResult result;
    result.Push(instance.DoCopyBatch(srcBuffers, dstBuffers));
    for (size_t device = 0; device < ctx.nDevice; device++) {
        delete srcBuffers[device];
        delete dstBuffers[device];
    }
    result.Show("[[ " + Key() + " ]] " + Brief());
}

DEFINE_COPY_CASE(Device2DeviceCECase, "device_to_device_ce",
                 "memcpy from device to device with ce one by one", ctx)
{
    CopyResult result;
    for (size_t device = 0; device < ctx.nDevice; device++) {
        DeviceCopyBuffer srcBuffer{device, ctx.size, ctx.num};
        DeviceCopyBuffer dstBuffer{device, ctx.size, ctx.num};
        D2DCECopyInstance instance{ctx.iter, false};
        result.Push(instance.DoCopy(&srcBuffer, &dstBuffer));
    }
    result.Show("[[ " + Key() + " ]] " + Brief());
}

DEFINE_COPY_CASE(OneDevice2AllDeviceCECase, "one_device_to_all_device_ce",
                 "memcpy from one device to all device with ce", ctx)
{
    CopyResult result;
    DeviceCopyBuffer srcBuffer{0, ctx.size, ctx.num};
    for (size_t device = 0; device < ctx.nDevice; device++) {
        DeviceCopyBuffer dstBuffer{device, ctx.size, ctx.num};
        D2DCECopyInstance instance{ctx.iter, false};
        result.Push(instance.DoCopy(&srcBuffer, &dstBuffer));
    }
    result.Show("[[ " + Key() + " ]] " + Brief());
}

DEFINE_COPY_CASE(Anonymous2DeviceCECase, "anonymous_to_device_ce",
                 "memcpy from anonymous to device one by one", ctx)
{
    CopyResult result;
    for (size_t device = 0; device < ctx.nDevice; device++) {
        AnonymousCopyBuffer srcBuffer{device, ctx.size, ctx.num};
        DeviceCopyBuffer dstBuffer{device, ctx.size, ctx.num};
        H2DCECopyInstance instance{ctx.iter, false};
        result.Push(instance.DoCopy(&srcBuffer, &dstBuffer));
    }
    result.Show("[[ " + Key() + " ]] " + Brief());
}

DEFINE_COPY_CASE(Host2DeviceCEMultiStreamCase, "host_to_device_ce_multi_stream",
                 "memcpy from host to device with ce using multi stream one by one", ctx)
{
    constexpr auto streamCount = 48;
    CopyResult result;
    for (size_t device = 0; device < ctx.nDevice; device++) {
        HostCopyBuffer srcBuffer{device, ctx.size, ctx.num};
        DeviceCopyBuffer dstBuffer{device, ctx.size, ctx.num};
        H2DCEMultiStreamCopyInstance instance{ctx.iter, false, streamCount};
        result.Push(instance.DoCopy(&srcBuffer, &dstBuffer));
    }
    result.Show("[[ " + Key() + " ]] " + Brief());
}
