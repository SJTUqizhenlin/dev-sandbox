# H2D FFTS Lane Sweep Experiment Plan

## 背景

当前 `run_h2d_ffts_experiments.py` 已经覆盖两组 H2D 实验：

- `size_sweep`：固定 buffer num，扫描 IO size。
- `num_sweep`：固定 IO size，扫描 buffer num。

第三组实验需要固定：

- IO size：`37K`
- buffer num：`1024`
- iterations：沿用脚本全局 `--iterations`，默认 `128`
- device count：沿用脚本全局 `--device-count`，默认 `1`
- lane：`1,2,4,8,16,32`

目标是观察 FFTS split 阶段中不同 ready lane 数对 `ascend_h2d_ffts_split` 的影响。

相关文件：

`@C:/Users/z00943858/Desktop/lzx-sandbox/dev-sandbox/module/copy/ascend/run_h2d_ffts_experiments.py`

`@C:/Users/z00943858/Desktop/lzx-sandbox/dev-sandbox/module/copy/ascend/ffts_d2d_dispatcher_ascend.h`

`@C:/Users/z00943858/Desktop/lzx-sandbox/dev-sandbox/module/copy/ascend/copy_instance_ffts_pipeline_ascend.h`

## 现状分析

当前 FFTS lane 数不是运行参数，而是在 dispatcher 中由固定常量控制：

```text
laneCount = min(copy descriptor count, kFftsMaxReadyLanes)
```

现有默认值是：

```text
kFftsMaxReadyLanes = 8
```

对于 `ascend_h2d_ffts_split -s 37K -n 1024`：

```text
copy descriptor count = 1024
laneCount = min(1024, 8) = 8
```

因此如果只改 Python 脚本，无法真正测试 `1,2,4,8,16,32` 这些 lane 条件。必须先给 C++ dispatcher 增加一个最小范围的运行时 lane knob。

## 总体方案

保持 copy CLI 参数不变，不新增 `copy` 二进制的命令行选项。新增一个环境变量控制 FFTS lane：

```text
FFTS_MAX_READY_LANES
```

这样 Python 实验脚本可以在每次 subprocess 调用时设置不同 env，C++ 侧按 env 改变 `BuildCopies` 里的 lane 上限。

默认行为保持不变：

```text
未设置 FFTS_MAX_READY_LANES 时，默认使用 8。
```

## C++ 改动方案

### Dispatcher Lane Knob

在 dispatcher 中把固定常量语义调整为默认值：

```text
kDefaultFftsMaxReadyLanes = 8
```

新增一个小函数读取环境变量：

```text
MaxReadyLanes()
```

行为：

- 环境变量不存在或为空：返回默认值 `8`。
- 环境变量不是正整数：返回默认值 `8`。
- 环境变量为 `0`：返回默认值 `8`。
- 环境变量大于 `uint16_t` 上限：截断或回落到安全上限。

`BuildCopies` 中改为：

```text
laneCount = min(copy descriptor count, MaxReadyLanes())
```

这样 `-n 1024` 时，lane 可以真实变成：

```text
1, 2, 4, 8, 16, 32
```

### Runtime 字段关系

对于每个 lane 配置：

```text
totalContextNum = 1024
readyContextNum = laneCount
preloadContextNum = min(readyContextNum, kFftsContextMaxNum)
```

由于 lane 最大只扫到 `32`，而 `kFftsContextMaxNum` 是 `128`，所以第三组实验中：

```text
preloadContextNum = readyContextNum
```

这次实验主要观察的是 `readyContextNum` 和依赖图宽度变化，而不是 preload 上限变化。

## Python 脚本改动方案

### 新增参数

给 `run_h2d_ffts_experiments.py` 增加三组参数：

```text
--lane-sweep-size
```

默认：

```text
37K
```

```text
--lane-sweep-buffer-num
```

默认：

```text
1024
```

```text
--lane-sweep-lanes
```

默认：

```text
1,2,4,8,16,32
```

### 新增实验类型

新增 experiment 名称：

```text
lane_sweep
```

其 x 轴含义：

```text
x_value = lane count
```

每个 lane 运行：

```text
copy -t ascend_h2d_ffts_split -s 37K -n 1024 -i 128 -d 1
```

同时设置环境变量：

```text
FFTS_MAX_READY_LANES=<lane>
```

日志文件名需要包含 lane，例如：

```text
lane_sweep_lane_8_h2d_ffts_r1.log
```

### Baseline 处理

第三组的核心变量只对 FFTS 生效，CE 和 CE multi-stream 没有 lane 概念。建议处理方式：

- 主图只画 `H2D+FFTS` 随 lane 的变化。
- 可选地在图中添加 CE 和 CE multi-stream 的固定 baseline 横线。
- baseline 使用同样条件跑一次：

```text
io_size = 37K
buffer_num = 1024
iterations = 128
device_count = 1
```

这样图上可以同时回答两个问题：

- FFTS lane 增加是否提升 H2D+FFTS 带宽。
- 最优 lane 下，H2D+FFTS 是否超过 CE 和 CE multi-stream baseline。

### 数据字段扩展

`RAW_FIELDS` 增加：

```text
ffts_lanes
```

`AGG_FIELDS` 增加：

```text
ffts_lanes
```

对于非 lane 实验：

```text
ffts_lanes = ""
```

对于 lane sweep：

```text
ffts_lanes = 当前 lane
```

`command` 字段建议记录环境变量前缀：

```text
FFTS_MAX_READY_LANES=8 copy -t ascend_h2d_ffts_split ...
```

这样后续看 CSV 或日志时能确认每条数据对应的 lane。

## 图表方案

新增两张图：

```text
lane_sweep_bandwidth.png
```

```text
lane_sweep_copy_time.png
```

横轴：

```text
lane count
```

横轴刻度：

```text
1, 2, 4, 8, 16, 32
```

纵轴：

- bandwidth 图：`bw_gbs`
- copy time 图：`copy_avg_us`

建议额外输出一张 submit 图：

```text
lane_sweep_submit_time.png
```

原因是 lane 增大会改变 FFTS context 构造和 launch 的提交开销，可能出现 bandwidth 提升但 submit 时间上升的情况。

## 分析文档输出

`analysis.md` 中增加 `lane_sweep` 小节。

建议输出以下结论项：

- 最优 bandwidth 的 lane。
- 最低 copy time 的 lane。
- 最低 submit time 的 lane。
- lane 从 `1` 到 `32` 的 bandwidth 提升倍数。
- lane 从 `8` 到 `16/32` 是否继续提升。
- 最优 lane 下相对 CE baseline 的带宽比。
- 最优 lane 下相对 CE multi-stream baseline 的带宽比。

分析口径：

```text
bandwidth speedup = lane_bw / baseline_bw
```

```text
lane scaling = bw_at_lane_N / bw_at_lane_1
```

## 预期执行命令

默认执行三组实验：

```bash
python3 module/copy/ascend/run_h2d_ffts_experiments.py
```

只修改 lane sweep 默认值时：

```bash
python3 module/copy/ascend/run_h2d_ffts_experiments.py \
  --lane-sweep-size 37K \
  --lane-sweep-buffer-num 1024 \
  --lane-sweep-lanes 1,2,4,8,16,32 \
  --iterations 128 \
  --device-count 1
```

## 验证方案

### 功能验证

先跑一个小规模 lane sweep：

```bash
python3 module/copy/ascend/run_h2d_ffts_experiments.py \
  --lane-sweep-lanes 1,2 \
  --iterations 2 \
  --repeats 1
```

检查：

- 日志中每个 lane 的命令都带 `FFTS_MAX_READY_LANES`。
- `summary.csv` 中有 `lane_sweep` 行。
- `analysis.md` 中有 lane sweep 小节。
- `plots/` 下有 lane sweep 图。

### 结果验证

正式运行后检查：

- `lane = 8` 的结果应与旧默认行为同口径。
- `lane = 1` 通常应该更接近串行依赖图。
- `lane = 16/32` 是否提升取决于 FFTS runtime 和硬件 SDMA 调度能力，不能只凭 ready lane 数假设一定线性提升。

## 风险与边界

- `FFTS_MAX_READY_LANES` 控制的是 FFTS dependency graph 的 ready lane 宽度，不等于硬件实际并发 SDMA 数。
- lane 增大可能增加 runtime 调度开销，submit time 可能上升。
- 如果底层硬件并发资源不足，`16/32` 可能相对 `8` 没有收益，甚至退化。
- 第三组只对 `ascend_h2d_ffts_split` 有直接意义，CE 和 CE multi-stream 只能作为固定 baseline。

## 实现步骤

1. 在 `ffts_d2d_dispatcher_ascend.h` 中新增环境变量读取逻辑，保持默认 lane 为 `8`。
2. 在 `BuildCopies` 中使用运行时 lane 上限。
3. 在 `run_h2d_ffts_experiments.py` 中新增 lane sweep 参数。
4. 扩展 `run_one` 支持额外 env 和 `ffts_lanes` 字段。
5. 新增 `lane_sweep` 执行循环。
6. 扩展 aggregate、terminal summary、plot 和 analysis。
7. 用小规模参数验证脚本输出结构。
8. 在 Ascend 环境中执行正式实验并检查曲线。
