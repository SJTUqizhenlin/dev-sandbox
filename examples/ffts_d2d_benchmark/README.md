# FFTS Plus SDMA 与 ACL D2D 聚合/拆分对比 benchmark

本 benchmark 在 Ascend NPU 上比较两条 D2D（Device-to-Device）拷贝路径：

1. **ACL 路径**：在同一个 stream 上循环调用 N 次 `aclrtMemcpyAsync`。
2. **FFTS 路径**：构造 N 个 SDMA context 描述符，通过一次 `rtFftsPlusTaskLaunchWithFlag` 提交。

两个方向：

- **merge（聚合）**：N 个离散源 buffer 拷贝到 1 个连续 transfer buffer。
- **split（拆分）**：1 个连续 transfer buffer 拷贝到 N 个离散目的 buffer。

本程序是独立示例，不依赖主项目的 CMake 构建。

## 程序流程

1. `aclInit` 初始化 AscendCL。
2. `aclrtSetDevice` 选择设备。
3. `aclrtCreateStream` 创建异步 stream。
4. `aclrtMalloc` 分配设备内存（源、目的、transfer buffer）。
5. `aclrtMemcpy`（同步 H2D）用确定性 pattern 初始化源 buffer。
6. 每次测量迭代：
   - **ACL 路径**：提交 N 次 `aclrtMemcpyAsync`。
   - **FFTS 路径**：构造 N 个 `rtFftsPlusSdmaCtx_t`，用最多 8 条 lane 组织依赖链，然后一次 launch。
7. `aclrtRecordEvent` + `aclrtEventElapsedTime` 测量 stream 侧拷贝时间。
8. `aclrtSynchronizeStream` 等待完成。
9. 测量结束后 D2H 回读 + `memcmp` 校验正确性。

## FFTS Task 结构

一个 FFTS Plus task 由两部分组成：

- **SQE**（`rtFftsPlusSqe_t`，64字节）：调度头，包含 `totalContextNum`、`readyContextNum`、`contextAddressBase`（指向 descBuf 物理地址）。
- **descBuf**：N 个 context 描述符的连续数组（每个 128字节），每个描述符是 `rtFftsPlusSdmaCtx_t`，包含源/目的地址、数据长度和 successor 链。

当 N > 8 时，只有前 8 个 context 作为 ready ctx（8 条 lane），剩余 context 通过 lane 依赖链（`successorList`）串联，同一 lane 上前一个完成后激活下一个。

```
rtFftsPlusTaskInfo_t
  |-- fftsPlusSqe -> rtFftsPlusSqe_t [64B]
  |     totalContextNum = N
  |     readyContextNum = min(N, 8)
  |     contextAddressBase -> descBuf 物理地址
  |
  |-- descBuf -> [N x 128B]
  |     ctx[0]: rtFftsPlusSdmaCtx_t (lane 0, ready)
  |     ctx[1]: rtFftsPlusSdmaCtx_t (lane 1, ready)
  |     ...     ctx[7]: rtFftsPlusSdmaCtx_t (lane 7, ready)
  |     ctx[8]: rtFftsPlusSdmaCtx_t (lane 0, ctx[0] 的后继)
  |     ctx[9]: rtFftsPlusSdmaCtx_t (lane 1, ctx[1] 的后继)
  |     ...
```

## 计时方法

计时方法论与本仓库 `acl_memcpy_minimal` 示例一致：

1. **Warmup**：先跑 5 次不计结果。
2. **测量**：跑 `iterations` 次，累加 `buildUs`、`submitUs`、`copyUs`。
3. **平均**：累加值除以迭代次数得到 `avgBuildUs`、`avgSubmitUs`、`avgCopyUs`。

```text
build 计时开始
  构造 SDMA context + lane 依赖链
build 计时结束  -> buildUs

在 stream 上 record start event
submit 计时开始
  提交 ACL 循环 或 FFTS launch
submit 计时结束  -> submitUs
在 stream 上 record stop event
synchronize stream

event elapsed  -> copyUs
```

- `buildUs`：Host 侧构造 FFTS context 的耗时，ACL 路径为 0。
- `submitUs`：Host 侧提交拷贝工作的耗时（仅 API 调用时间）。
- `copyUs`：Stream 侧实际拷贝时间，由 `aclrtRecordEvent` + `aclrtEventElapsedTime` 测量。
- 单次 IO 时间：`buildUs / count`、`submitUs / count`、`copyUs / count`。
- 带宽：`totalBytes / copyUs` 转换为 MB/s。

## 输出列说明

```text
Dir      Path   Size   Count   Build(us)   Submit(us)   Copy(us)   Build/IO(us)   Submit/IO(us)   Copy/IO(us)   BW(MB/s)   Pass
```

| 列 | 说明 |
|---|---|
| `Dir` | `merge` 或 `split` |
| `Path` | `acl` 或 `ffts` |
| `Size` | 单个 buffer 字节数，带 K/M/G 单位 |
| `Count` | 每次迭代拷贝的 buffer 数量 |
| `Build(us)` | FFTS context 构造平均耗时（ACL 路径为 0） |
| `Submit(us)` | API 提交平均耗时 |
| `Copy(us)` | Stream 侧拷贝平均耗时 |
| `Build/IO(us)` | `Build(us) / Count` |
| `Submit/IO(us)` | `Submit(us) / Count` |
| `Copy/IO(us)` | `Copy(us) / Count` |
| `BW(MB/s)` | 有效带宽 |
| `Pass` | 正确性校验结果 |

## 编译

在安装了 CANN 的 Linux 机器上，进入本目录执行：

```bash
bash build.sh
```

脚本会自动从环境变量或常见安装路径检测 `ASCEND_ROOT`。如果 CANN 没有暴露 FFTS Plus 头文件，会回退到本目录自带的 `ffts_plus_minimal_runtime.h`。

手动指定 CANN 路径：

```bash
ASCEND_ROOT=/usr/local/Ascend/cann-9.0.0 bash build.sh
```

编译产物：`build/ffts_vs_acl_d2d_benchmark`

## 运行

```bash
bash run.sh
```

脚本会自动检测库路径，并将环境变量转为 CLI 参数。

环境变量：

| 变量 | 默认值 | 说明 |
|---|---|---|
| `DEVICE_ID` | `0` | 设备 ID |
| `TEST_TYPE` | `all` | 测试类型：`all`、`merge`、`split` |
| `COPY_PATH` | `both` | 拷贝路径：`acl`、`ffts`、`both` |
| `IO_SIZE` | `64K` | 单 buffer 字节数，支持 K/M/G 后缀 |
| `BUFFER_COUNT` | `1024` | 每次迭代的 buffer 数量 |
| `ITERATIONS` | `128` | 测量迭代次数 |

示例：

```bash
# 只跑 FFTS 路径
COPY_PATH=ffts bash run.sh

# 只跑 ACL 路径
COPY_PATH=acl bash run.sh

# 只跑聚合方向
TEST_TYPE=merge bash run.sh

# 自定义参数
IO_SIZE=2K BUFFER_COUNT=64 ITERATIONS=50 bash run.sh
```

直接调用：

```bash
./build/ffts_vs_acl_d2d_benchmark -t merge -p ffts -s 64K -n 1024 -i 128
./build/ffts_vs_acl_d2d_benchmark -t split -p acl -s 2K -n 64 -i 50
./build/ffts_vs_acl_d2d_benchmark -t all -p both -s 8K -n 16 -i 128 -d 0
```

CLI 参数：

```text
-t <test_type>   测试类型：all, merge, split。默认: all
-p <copy_path>   拷贝路径：acl, ffts, both。默认: both
-s <io_size>     单 buffer 字节数（K/M/G 后缀）。默认: 64K
-n <buffer_count> 每次迭代 buffer 数量。默认: 1024
-i <iterations>  测量迭代次数。默认: 128
-d <device_id>   设备 ID。默认: 0
-h               显示帮助信息
```

## 输出示例

```text
FFTS vs ACL D2D benchmark (device=0, warmup=5, iterations=128)

Dir      Path   Size   Count   Build(us)   Submit(us)   Copy(us)   Build/IO(us)   Submit/IO(us)   Copy/IO(us)   BW(MB/s)   Pass
----------------------------------------------------------------------------------------------------------------------------------------
merge     acl   64 KB   1024         0.000      160.200      2000.000           0.000           0.156         1.953        32000.00   yes
merge    ffts   64 KB   1024        19.800       12.400       960.000           0.019           0.012         0.938        66666.67   yes
split     acl   64 KB   1024         0.000      158.900      2000.000           0.000           0.155         1.953        32000.00   yes
split    ffts   64 KB   1024        20.100       12.500       980.000           0.020           0.012         0.961        65306.12   yes
```

## 正确性校验

每次测量结束后，benchmark 通过 D2H 回读目的 buffer 并与期望 pattern 比对。校验不计入计时。

- **merge 校验**：每个源 buffer 使用不同 pattern `f(i, j)`。聚合后 D2H 回读 transfer buffer，比较每个偏移区间与对应的源 pattern。
- **split 校验**：transfer buffer 使用连续 pattern。拆分后 D2H 回读每个目的 buffer，比较与 transfer 对应偏移区间。

## 与 h2d_d2h_async_memcpy 的关系

本 benchmark 使用与本仓库 `acl_memcpy_minimal` 示例相同的计时方法论：

- warmup + 累加求平均（不是 p50/中位数）
- `aclrtSynchronizeStream`（不是 `aclrtSynchronizeEvent`）
- `aclrtRecordEvent` + `aclrtEventElapsedTime` 测量 stream 侧时间
- 单次 IO 归一化时间 + MB/s 带宽
- `<iomanip>` 格式化表格输出

关键差异在于数据方向：`h2d_d2h_async_memcpy` 测量 H2D（Host-to-Device）和 D2H（Device-to-Host）拷贝，而本 benchmark 测量 D2D（Device-to-Device）的 merge 和 split 模式，比较 ACL 的逐拷贝异步 API 与 FFTS Plus SDMA 的单次 launch 方式。