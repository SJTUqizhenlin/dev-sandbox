# Minimal AscendCL Async H2D Memcpy Demo

This demo is a small, standalone example for learning the basic AscendCL
`aclrtMemcpyAsync` flow. It does not depend on the main project and is not wired
into the repository CMake build.

The program measures asynchronous Host-to-Device (H2D) copies for one requested
buffer size and buffer count. It first runs a single-device test on device 0,
then runs an 8-device simultaneous test where each device reads from its own host
buffer into its own device buffer.

For each measurement iteration, every device submits a batch of async copies to
one stream and then synchronizes once. The 8-device test uses one host thread per
device and a CPU barrier to align each iteration, avoiding cross-device event
dependencies in the benchmark code.

## What It Shows

The program follows this basic runtime flow:

1. `aclInit` initializes AscendCL for the current process.
2. `aclrtSetDevice` selects the Ascend device to use.
3. `aclrtCreateStream` creates a stream for asynchronous work.
4. `aclrtMallocHost` allocates one large pinned host memory region.
5. `aclrtMalloc` allocates one large device memory region.
6. The large regions are sliced into `Count` fixed-size copy entries.
7. `aclrtMemcpyAsync` submits several asynchronous memory copies to the stream.
8. `aclrtSynchronizeStream` waits until the submitted copy has finished.
9. `aclrtFree`, `aclrtFreeHost`, `aclrtDestroyStream`, `aclrtResetDevice`, and
   `aclFinalize` release resources.

The source pointer is host memory and the destination pointer is device memory.

`aclrtMemcpyAsync` has this important argument order:

```cpp
aclrtMemcpyAsync(dst, destMax, src, count, kind, stream)
```

- `dst`: destination address.
- `destMax`: destination buffer capacity in bytes.
- `src`: source address.
- `count`: number of bytes to copy.
- `kind`: copy direction, such as `ACL_MEMCPY_HOST_TO_DEVICE` or
  `ACL_MEMCPY_DEVICE_TO_HOST`.
- `stream`: stream that receives the asynchronous copy work.

The memory returned by `aclrtMallocHost` and `aclrtMalloc` is generally aligned
for AscendCL transfer requirements. This demo uses those returned pointers
directly and does not create unaligned offsets.

## Timing Columns

The output columns are:

```text
Dir          Size   Count    Submit(us)      Wait(us)      Copy(us)        BW(MB/s)
```

- `Size`: bytes per buffer, printed in a human-readable unit.
- `Count`: number of buffers copied per measurement iteration.
- `Submit(us)`: average CPU-side time spent submitting the batch of
  `aclrtMemcpyAsync` calls.
- `Copy(us)`: average stream-side copy time measured by `aclrtRecordEvent` and
  `aclrtEventElapsedTime`, matching the style used by the main benchmark.
- `Wait(us)`: `Copy(us) - Submit(us)`, clamped at zero for display.
- `BW(MB/s)`: effective bandwidth computed from `Size * Count / Copy(us)`.

## Build

Run this on a Linux machine with AscendCL installed. Adjust the Ascend toolkit
path if your installation is not under `/usr/local/Ascend/ascend-toolkit/latest`.

```bash
g++ h2d_d2h_async_memcpy.cpp -o h2d_d2h_async_memcpy \
  -I/usr/local/Ascend/ascend-toolkit/latest/include \
  -L/usr/local/Ascend/ascend-toolkit/latest/lib64 \
  -lascendcl
```

If the runtime library is not found at run time, set `LD_LIBRARY_PATH`:

```bash
export LD_LIBRARY_PATH=/usr/local/Ascend/ascend-toolkit/latest/lib64:${LD_LIBRARY_PATH}
```

## Run

```bash
./h2d_d2h_async_memcpy -s 64K -n 1024 -i 128
```

Options:

```text
-s <io_size>       Bytes per buffer. Suffixes K/M/G are supported.
-n <buffer_count>  Number of buffers copied per measurement iteration.
-i <iterations>    Number of measured iterations. Default: 128.
```

The single-device test is fixed to device 0.
The 8-device test uses devices 0 through 7.

Example output:

```text
AscendCL aclrtMemcpyAsync single-device H2D benchmark
warmup=5, iterations=128, buffers_per_iteration=1024

Dir             Size   Count    Submit(us)      Wait(us)      Copy(us)        BW(MB/s)
--------------------------------------------------------------------------------------
H2D            64 KB    1024       160.200      1839.800      2000.000        32000.00

AscendCL aclrtMemcpyAsync 8-device simultaneous H2D benchmark
warmup=5, iterations=128, buffers_per_iteration=1024

Dir             Size   Count    Submit(us)      Wait(us)      Copy(us)        BW(MB/s)
--------------------------------------------------------------------------------------
H2D_ALL8       64 KB    8192      1200.000      2800.000      4000.000       128000.00
```
