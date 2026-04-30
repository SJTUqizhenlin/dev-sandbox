# Minimal AscendCL Async Memcpy Demo

This demo is a small, standalone example for learning the basic AscendCL
`aclrtMemcpyAsync` flow. It does not depend on the main project and is not wired
into the repository CMake build.

The program measures asynchronous Host-to-Device (H2D) and Device-to-Host (D2H)
copies for several buffer sizes. It prints an aligned terminal table for quick
reading.

## What It Shows

The program follows this basic runtime flow:

1. `aclInit` initializes AscendCL for the current process.
2. `aclrtSetDevice` selects the Ascend device to use.
3. `aclrtCreateStream` creates a stream for asynchronous work.
4. `aclrtMallocHost` allocates pinned host memory.
5. `aclrtMalloc` allocates device memory.
6. `aclrtMemcpyAsync` submits an asynchronous memory copy to the stream.
7. `aclrtSynchronizeStream` waits until the submitted copy has finished.
8. `aclrtFree`, `aclrtFreeHost`, `aclrtDestroyStream`, `aclrtResetDevice`, and
   `aclFinalize` release resources.

For H2D, the source pointer is host memory and the destination pointer is device
memory. For D2H, the source pointer is device memory and the destination pointer
is host memory.

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
Dir          Size    Submit(us)      Wait(us)     Total(us)        BW(MB/s)
```

- `avg_submit_us`: average time spent inside the `aclrtMemcpyAsync` call. This
  measures submission overhead, not full copy completion.
- `avg_total_us`: average time from just before `aclrtMemcpyAsync` until
  `aclrtSynchronizeStream` returns.
- `avg_wait_us`: `avg_total_us - avg_submit_us`, roughly the time spent waiting
  for the asynchronous copy to complete after submission.
- `bandwidth_MBps`: effective bandwidth computed from `avg_total_us`.

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
./h2d_d2h_async_memcpy
```

Example output:

```text
AscendCL aclrtMemcpyAsync H2D/D2H benchmark
warmup=5, iterations=50, device=0

Dir             Size    Submit(us)      Wait(us)     Total(us)        BW(MB/s)
------------------------------------------------------------------------------
H2D             4 KB         3.200        14.500        17.700          220.69
D2H             4 KB         3.100        15.200        18.300          213.48
```
