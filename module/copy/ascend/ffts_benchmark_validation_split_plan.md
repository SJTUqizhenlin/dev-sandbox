# FFTS Bandwidth Benchmark and Validation Split Plan

## 目标

把 FFTS copy 的带宽测试和正确性校验拆开：

- 带宽测试脚本只负责性能采样、日志、汇总、画图和分析，不再承担 validation。
- validation 单独由一个脚本触发，专门验证数据正确性。
- `copy` CLI 不新增命令行参数，继续保持最小侵入。
- C++ case 中保留 validation 能力，但默认不执行，避免污染带宽测试流程。

相关文件：

`@C:/Users/z00943858/Desktop/lzx-sandbox/dev-sandbox/module/copy/ascend/run_h2d_ffts_experiments.py`

`@C:/Users/z00943858/Desktop/lzx-sandbox/dev-sandbox/module/copy/ascend/copy_case_ffts_d2d_ascend.cc`

`@C:/Users/z00943858/Desktop/lzx-sandbox/dev-sandbox/module/copy/ascend/copy_instance_ffts_pipeline_ascend.h`

`@C:/Users/z00943858/Desktop/lzx-sandbox/dev-sandbox/module/copy/ascend/ffts_d2d_dispatcher_ascend.h`

## 当前问题

当前 Python 带宽脚本本身没有直接做 validation，但它调用的 FFTS C++ case 会在 `DoCopy` 之后执行 `ASSERT(Validate...)`。

也就是说，现在实际路径是：

```text
Python benchmark script
-> copy binary
-> FFTS case
-> DoCopy benchmark
-> Validate result
-> print result
```

其中 `Validate` 不在 `Copy(us)` event 计时范围内，但它仍然会：

- 增加每条 benchmark 命令的端到端 wall time。
- 引入额外 D2H 回读或 host 检查。
- 对大规模 sweep 产生大量非性能路径开销。
- 让带宽测试脚本同时承担性能测试和正确性验证两个职责。

因此需要把职责拆开。

## 拆分原则

### 带宽测试路径

带宽测试只运行真实待测 copy pipeline：

```text
H2D big copy + FFTS split
FFTS merge + D2H big copy
```

带宽测试中不做：

- device result 回读校验。
- host result pattern 比较。
- 每轮 case 结束后的 validation assert。

带宽脚本仍然可以保留：

- benchmark 命令日志。
- raw result。
- summary。
- plot。
- analysis。

### Validation 路径

validation 单独跑，目标是证明数据路径正确，不追求稳定带宽。

validation 可以做：

- 初始化 deterministic pattern。
- 执行 copy pipeline。
- 回读结果。
- 比较 pattern。
- 输出 PASS/FAIL。

validation 不用于性能结论。

## C++ 方案

### 增加环境变量开关

新增一个环境变量：

```text
COPY_FFTS_VALIDATE
```

默认行为：

```text
COPY_FFTS_VALIDATE 未设置或为 0 时，不执行 validation。
```

开启行为：

```text
COPY_FFTS_VALIDATE=1 时，执行现有 ValidatePatternedBuffer 或 ValidateHostPatternedBuffer。
```

这样不需要改 `copy` CLI，也不需要扩展 `CopyCase::Context`。

### 保留现有 case 名

继续使用现有 case：

```text
ascend_h2d_ffts_split
ascend_ffts_merge_d2h
ascend_d2d_split_ffts
ascend_d2d_merge_ffts
```

这些 case 在 benchmark 默认路径下只跑性能；在 validation 脚本设置环境变量后才执行校验。

### 封装校验开关

在 FFTS case 文件中增加小函数：

```text
FftsValidationEnabled()
```

行为：

- 读取 `COPY_FFTS_VALIDATE`。
- `1`、`true`、`TRUE`、`on`、`ON` 视为开启。
- 其他值视为关闭。

然后把现有：

```text
ASSERT(ValidatePatternedBuffer(...))
```

改成：

```text
if (FftsValidationEnabled()) {
    ASSERT(ValidatePatternedBuffer(...))
}
```

host 校验同理。

## 带宽测试脚本方案

带宽脚本仍使用：

```text
run_h2d_ffts_experiments.py
```

它负责三类实验：

- `size_sweep`
- `num_sweep`
- `lane_sweep`

脚本中需要保证 benchmark-only：

- subprocess env 中显式设置 `COPY_FFTS_VALIDATE=0`。
- 不解析 validation 输出。
- 不因为 validation 失败或 PASS/FAIL 影响带宽结果。

建议在脚本配置输出中打印：

```text
validation=disabled
```

这样实验日志可以明确说明该轮是纯带宽测试。

## Validation 脚本方案

新增脚本：

```text
run_h2d_ffts_validation.py
```

脚本职责：

- 专门触发 correctness validation。
- 设置 `COPY_FFTS_VALIDATE=1`。
- 使用较小 iterations，默认 `1`。
- 可扫代表性 size、buffer num、lane。
- 输出终端 PASS/FAIL 表。
- 每个失败 case 保留日志。

### 默认 validation 矩阵

建议默认矩阵不要太大，覆盖代表性边界即可：

```text
sizes = 2K,8K,37K,64K
buffer_nums = 1,10,64,1024
lanes = 1,8,32
iterations = 1
device_count = 1
```

### 默认 validation cases

建议覆盖四条 FFTS 路径：

```text
ascend_h2d_ffts_split
ascend_ffts_merge_d2h
ascend_d2d_split_ffts
ascend_d2d_merge_ffts
```

如果只想验证 H2D 相关实验，可以支持参数缩小到：

```text
ascend_h2d_ffts_split
```

### 命令示例

默认运行：

```bash
python3 module/copy/ascend/run_h2d_ffts_validation.py
```

只验证 H2D split：

```bash
python3 module/copy/ascend/run_h2d_ffts_validation.py \
  --cases ascend_h2d_ffts_split \
  --sizes 37K \
  --buffer-nums 1024 \
  --lanes 1,2,4,8,16,32 \
  --iterations 1 \
  --device-count 1
```

每条命令实际设置：

```text
COPY_FFTS_VALIDATE=1
```

如果同时接入 lane sweep，还应设置：

```text
FFTS_MAX_READY_LANES=<lane>
```

## 推荐运行顺序

正式做性能实验前：

```text
先跑 validation 脚本，确认数据正确。
```

validation 通过后：

```text
再跑 bandwidth benchmark 脚本，收集性能数据。
```

推荐流程：

```text
1. build copy binary
2. run_h2d_ffts_validation.py
3. run_h2d_ffts_experiments.py
4. 查看 summary、plots、analysis
```

## 输出格式

### Validation 输出

validation 脚本输出终端表格即可：

```text
case                      size    buffers   lane   result   log
ascend_h2d_ffts_split     37K     1024      8      PASS     logs/...
ascend_h2d_ffts_split     37K     1024      32     PASS     logs/...
```

如果失败：

```text
result = FAIL
```

并保留对应 log。

### Bandwidth 输出

带宽脚本继续输出性能 summary：

```text
experiment    x        case             submit_us    copy_us    bw_gbs
```

这里不出现 validation PASS/FAIL 字段。

## 和 Lane Sweep 的关系

第三组 lane sweep 的性能测试中：

```text
COPY_FFTS_VALIDATE=0
FFTS_MAX_READY_LANES=<lane>
```

第三组 lane validation 中：

```text
COPY_FFTS_VALIDATE=1
FFTS_MAX_READY_LANES=<lane>
```

这样可以分别回答两个问题：

- correctness：不同 lane 下结果是否正确。
- performance：不同 lane 下带宽和 copy time 如何变化。

二者不要混在一次 benchmark 里。

## 实现步骤

1. 在 `copy_case_ffts_d2d_ascend.cc` 中增加 `FftsValidationEnabled()`。
2. 将现有 `ASSERT(Validate...)` 包到 validation 开关里。
3. 在 `run_h2d_ffts_experiments.py` 的 subprocess env 中显式设置 `COPY_FFTS_VALIDATE=0`。
4. 新增 `run_h2d_ffts_validation.py`。
5. validation 脚本设置 `COPY_FFTS_VALIDATE=1`，并按 size、buffer num、lane、case 运行矩阵。
6. validation 脚本只判断 return code 和日志，不参与 bandwidth aggregation。
7. 正式性能实验前先跑 validation，性能实验时只跑 benchmark-only 脚本。

## 风险与注意点

- 关闭 benchmark path 的 validation 后，性能结果更干净，但必须养成先跑 validation 的流程。
- validation 不应该用高 iterations，避免把 correctness 检查变成长时间压力测试。
- validation 的失败不应被 benchmark 脚本吞掉；两个脚本职责要清晰。
- lane sweep 中 `COPY_FFTS_VALIDATE` 和 `FFTS_MAX_READY_LANES` 是两个正交开关，不要混用。
- `Copy(us)` 本来没有包含 validation，但 validation 会影响脚本端到端耗时和实验流程，因此仍然需要拆开。
