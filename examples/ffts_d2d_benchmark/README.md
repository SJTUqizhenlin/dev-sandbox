# FFTS Plus SDMA vs ACL D2D Merge/Split Benchmark

This benchmark compares two D2D (Device-to-Device) copy paths on Ascend NPU:

1. **ACL path**: Loop `aclrtMemcpyAsync` N times on one stream.
2. **FFTS path**: Build N SDMA context descriptors, launch one `rtFftsPlusTaskLaunchWithFlag`.

Two directions are tested:

- **Merge**: N separate source buffers -> 1 contiguous transfer buffer.
- **Split**: 1 contiguous transfer buffer -> N separate destination buffers.

The program is a standalone example that does not depend on the main project CMake build.

## What It Shows

The benchmark follows this flow:

1. `aclInit` initializes AscendCL for the current process.
2. `aclrtSetDevice` selects the Ascend device.
3. `aclrtCreateStream` creates a stream for asynchronous work.
4. `aclrtMalloc` allocates device buffers for source, destination, and transfer regions.
5. `aclrtMemcpy` (synchronous H2D) initializes source buffers with deterministic patterns.
6. For each measured iteration:
   - **ACL path**: Submit N `aclrtMemcpyAsync` calls to the stream.
   - **FFTS path**: Build N `rtFftsPlusSdmaCtx_t` context descriptors with up to 8
     ready lanes and lane dependency chains, then launch one FFTS Plus task.
7. `aclrtRecordEvent` / `aclrtEventElapsedTime` measures the stream-side copy time.
8. `aclrtSynchronizeStream` waits for all copy work to complete.
9. After measurement, D2H read-back and `memcmp` validates correctness.

### FFTS Task Structure

One FFTS Plus task consists of:

- **SQE** (`rtFftsPlusSqe_t`, 64 bytes): Scheduling header with `totalContextNum`, `readyContextNum`,
  and `contextAddressBase` pointing to the context descriptor buffer.
- **descBuf**: Continuous array of N context descriptors (each 128 bytes). Each descriptor
  is a `rtFftsPlusSdmaCtx_t` with source/destination address, data length, and successor list.

For N > 8, only the first 8 contexts are marked as ready (8 lanes). The remaining contexts
are linked through lane dependency chains (`successorList`) so that each lane's next context
activates when the previous one on the same lane finishes.

```
rtFftsPlusTaskInfo_t
  |-- fftsPlusSqe -> rtFftsPlusSqe_t [64B]
  |     totalContextNum = N
  |     readyContextNum = min(N, 8)
  |     contextAddressBase -> descBuf physical address
  |
  |-- descBuf -> [N x 128B]
  |     ctx[0]: rtFftsPlusSdmaCtx_t (lane 0, ready)
  |     ctx[1]: rtFftsPlusSdmaCtx_t (lane 1, ready)
  |     ...     ctx[7]: rtFftsPlusSdmaCtx_t (lane 7, ready)
  |     ctx[8]: rtFftsPlusSdmaCtx_t (lane 0, successor of ctx[0])
  |     ctx[9]: rtFftsPlusSdmaCtx_t (lane 1, successor of ctx[1])
  |     ...
```

### Timing Method

The timing follows the same methodology as the `h2d_d2h_async_memcpy` reference benchmark
in this repository:

1. **Warmup**: Run `kWarmupIterations` (5) iterations without recording results.
2. **Measurement**: Run `iterations` iterations, accumulate `buildUs`, `submitUs`, `copyUs`.
3. **Average**: Divide each accumulated total by `iterations` to get `avgBuildUs`, `avgSubmitUs`,
   `avgCopyUs`.

```text
build timer start
  build SDMA contexts + lane dependencies
build timer stop  -> buildUs

record start event on stream
submit timer start
  submit ACL loop OR FFTS launch
submit timer stop  -> submitUs
record stop event on stream
synchronize stream

event elapsed  -> copyUs
```

- `buildUs`: Host-side time to construct FFTS context descriptors. Zero for ACL path.
- `submitUs`: Host-side time to submit the copy work (API call time only).
- `copyUs`: Stream-side time measured by `aclrtRecordEvent` + `aclrtEventElapsedTime`.
- Per-IO times: `buildUs / count`, `submitUs / count`, `copyUs / count`.
- Bandwidth: `totalBytes / copyUs` converted to MB/s.

## Timing Columns

```text
Dir      Path   Size   Count   Build(us)   Submit(us)   Copy(us)   Build/IO(us)   Submit/IO(us)   Copy/IO(us)   BW(MB/s)   Pass
```

- `Dir`: `merge` or `split`.
- `Path`: `acl` or `ffts`.
- `Size`: bytes per buffer, printed in a human-readable unit.
- `Count`: number of buffers copied per measurement iteration.
- `Build(us)`: average CPU-side time to build FFTS context descriptors (0 for ACL).
- `Submit(us)`: average CPU-side time to submit the copy work.
- `Copy(us)`: average stream-side copy time measured by event elapsed time.
- `Build/IO(us)`: `Build(us) / Count`.
- `Submit/IO(us)`: `Submit(us) / Count`.
- `Copy/IO(us)`: `Copy(us) / Count`.
- `BW(MB/s)`: effective bandwidth from `Size * Count / Copy(us)`.
- `Pass`: correctness validation result (`yes` or `no`).

## Build

Run on a Linux machine with CANN installed. The build script auto-detects `ASCEND_ROOT`
from environment variables or common install paths. If CANN does not expose FFTS Plus
headers, the build falls back to `ffts_plus_minimal_runtime.h` (bundled local definitions).

```bash
bash build.sh
```

To override the CANN root:

```bash
ASCEND_ROOT=/usr/local/Ascend/cann-9.0.0 bash build.sh
```

## Run

```bash
bash run.sh
```

The run script auto-detects library paths and passes environment variables as CLI arguments.

Environment variables:

```text
DEVICE_ID     Device ID. Default: 0
TEST_TYPE     all, merge, split. Default: all
COPY_PATH     acl, ffts, both. Default: both
IO_SIZE       Bytes per buffer (K/M/G suffixes). Default: 64K
BUFFER_COUNT  Number of buffers per iteration. Default: 1024
ITERATIONS    Number of measured iterations. Default: 128
```

Examples:

```bash
COPY_PATH=ffts bash run.sh
COPY_PATH=acl bash run.sh
TEST_TYPE=merge bash run.sh
IO_SIZE=2K BUFFER_COUNT=64 ITERATIONS=50 bash run.sh
```

Direct invocation:

```bash
./build/ffts_vs_acl_d2d_benchmark -t merge -p ffts -s 64K -n 1024 -i 128
./build/ffts_vs_acl_d2d_benchmark -t split -p acl -s 2K -n 64 -i 50
./build/ffts_vs_acl_d2d_benchmark -t all -p both -s 8K -n 16 -i 128
```

Full CLI options:

```text
-t <test_type>   Test type: all, merge, split. Default: all
-p <copy_path>   Copy path: acl, ffts, both. Default: both
-s <io_size>     Bytes per buffer (K/M/G suffixes). Default: 64K
-n <buffer_count> Number of buffers per iteration. Default: 1024
-i <iterations>  Number of measured iterations. Default: 128
-d <device_id>   Device ID. Default: 0
-h               Show help message
```

## Example Output

```text
FFTS vs ACL D2D benchmark (device=0, warmup=5, iterations=128)

Dir      Path   Size   Count   Build(us)   Submit(us)   Copy(us)   Build/IO(us)   Submit/IO(us)   Copy/IO(us)   BW(MB/s)   Pass
----------------------------------------------------------------------------------------------------------------------------------------
merge     acl   64 KB   1024         0.000      160.200      2000.000           0.000           0.156         1.953        32000.00   yes
merge    ffts   64 KB   1024        19.800       12.400       960.000           0.019           0.012         0.938        66666.67   yes
split     acl   64 KB   1024         0.000      158.900      2000.000           0.000           0.155         1.953        32000.00   yes
split    ffts   64 KB   1024        20.100       12.500       980.000           0.020           0.012         0.961        65306.12   yes
```

## Validation

After each measurement run, the benchmark reads back the destination buffer(s) via D2H
and compares against the expected pattern. Validation is not included in the timing.

- **Merge**: Each source buffer has a unique pattern `f(i, j)`. After merge, the transfer
  buffer is read back and each offset range is compared against its source pattern.
- **Split**: The transfer buffer contains concatenated patterns. After split, each destination
  buffer is read back and compared against the corresponding offset range of the transfer pattern.

## Relationship to h2d_d2h_async_memcpy

This benchmark uses the same timing methodology as the `acl_memcpy_minimal` example
in this repository:

- Warmup + average accumulation (not p50/median).
- `aclrtSynchronizeStream` (not `aclrtSynchronizeEvent`).
- `aclrtRecordEvent` + `aclrtEventElapsedTime` for stream-side measurement.
- Per-IO normalized times and MB/s bandwidth.
- `<iomanip>` formatted table output.

The key difference is the data direction: `h2d_d2h_async_memcpy` measures H2D (Host-to-Device)
and D2H (Device-to-Host) copies, while this benchmark measures D2D (Device-to-Device) merge
and split patterns, comparing ACL's per-copy async API against FFTS Plus SDMA's single-launch
approach.