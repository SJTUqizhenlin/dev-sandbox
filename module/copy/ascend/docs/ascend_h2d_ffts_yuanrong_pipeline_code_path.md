# ascend_h2d_ffts_yuanrong_pipeline 代码路径精讲

这份文档只讲 `ascend_h2d_ffts_yuanrong_pipeline` 这个 copy benchmark case 的当前代码路径。它的核心目标是复刻 yuanrong 里的 H2D 双缓冲流水线模型：先把一个 logical object 做成一笔连续 H2D，写入 device staging slot；再用 FFTS Plus SDMA 把 staging slot 拆到多个 fragmented device buffer；两个 staging slot 轮转，让相邻 object 的 H2D 阶段和 FFTS split 阶段可以重叠。

先把最容易误解的点说清楚：这个 case 里的 FFTS 不直接从 Host 读数据。真正的 Host-to-Device 仍然是 `aclrtMemcpyAsync`。FFTS 负责的是 device staging slot 到最终 fragmented device buffers 的 D2D split。

## 一句话总览

命令行选择 `ascend_h2d_ffts_yuanrong_pipeline` 后，路径大致是：

```text
copy_main
-> CopyCaseFactory
-> AscendH2DFFTSYuanrongPipelineCase
-> HostCopyBuffer + FragmentedDeviceCopyBuffer
-> H2DFFTSYuanrongPipelineCopyInstance
-> Prepare
-> DoCopyOnce
-> SubmitObject
-> BuildObjectCopies
-> FftsD2DDispatcher::BuildCopies
-> FftsD2DDispatcher::Launch
```

对应源码文件：

`@dev-sandbox/module/copy/copy_main.cc`

`@dev-sandbox/module/copy/copy_case.h`

`@dev-sandbox/module/copy/ascend/copy_case_ffts_d2d_ascend.cc`

`@dev-sandbox/module/copy/ascend/copy_buffer_ascend.h`

`@dev-sandbox/module/copy/ascend/copy_instance_ffts_pipeline_ascend.h`

`@dev-sandbox/module/copy/ascend/ffts_d2d_dispatcher_ascend.h`

## 编译和注册入口

这个 case 不是任何 Ascend 后端都会编进 binary。CMake 里先按 `RUNTIME_BACKEND=Ascend` 收集 Ascend copy 文件，但会先把 FFTS case 文件移出；只有 `HAVE_ASCEND_FFTS_RUNTIME` 成立时，才把 `copy_case_ffts_d2d_ascend.cc` 加回来，并把 FFTS runtime include 和 runtime library 链进去。

所以如果运行时 `-t ascend_h2d_ffts_yuanrong_pipeline` 找不到 case，第一层要先看是不是编译时没有找到 FFTS header 或 `libruntime`，而不是先怀疑 case 注册函数。

`@dev-sandbox/module/copy/CMakeLists.txt`

程序启动后，`copy_main` 只负责解析 `-t/-s/-n/-i/-d`。它不直接认识某个 Ascend case，而是把 `-t` 得到的名字交给 `CopyCaseFactory::Filter`。真正注册发生在 `DEFINE_COPY_CASE` 宏里：宏生成 case 类，并创建一个全局静态 registrar；进程启动时 registrar 把 case 对象放进 `CopyCaseFactory`。

`@dev-sandbox/module/copy/copy_main.cc`

`@dev-sandbox/module/copy/copy_case.h`

## case 层做了什么

`AscendH2DFFTSYuanrongPipelineCase` 是命令行 case 的第一段真实业务代码。它做的事情很克制：

1. 读取 `COPY_FFTS_PIPELINE_OBJECT_FRAGS`，解析失败或没设置时使用默认值 `8`。
2. 对每个 device 创建一个连续 Host 源 buffer。
3. 创建一个 fragmented Device 目标 buffer。
4. 在 Host 源 buffer 写入可校验 pattern。
5. 把 Device 目标 buffer 清零。
6. 创建 `H2DFFTSYuanrongPipelineCopyInstance`。
7. 调用 `DoCopy` 进入 benchmark 通用计时流程。
8. 如果 `COPY_FFTS_VALIDATE=1`，把目标 device fragments 拷回 Host 验证 pattern。
9. 输出结果时附带实际生效的 `object_frags`。

`object_frags` 的含义不是 FFTS lane 数，而是一个 logical object 包含多少个 fragment。例如 `-s 37K -n 1024` 且 `COPY_FFTS_PIPELINE_OBJECT_FRAGS=8` 时，一轮 iteration 会被切成 `128` 个 logical object；每个 object 先做一笔 `37K * 8` 的 H2D，然后 FFTS split 到 8 个目标 fragments。

`@dev-sandbox/module/copy/ascend/copy_case_ffts_d2d_ascend.cc`

## buffer 形态

这个 case 的三种 buffer 角色必须分清：

`HostCopyBuffer` 是源。它用 `aclrtMallocHost` 申请一整块连续 Host pinned memory。`src[i]` 只是这块连续内存中第 `i` 个 fragment 的偏移。

`FragmentedDeviceCopyBuffer` 是最终目的地。它为每个 fragment 单独 `aclrtMalloc`，所以 `dst[i]` 不是连续大 buffer 的偏移，而是一个独立 device 指针。

`DeviceCopyBuffer` 在这个 case 里不作为最终目的地，而是作为 device staging slot。每个 slot 是一块连续 device buffer，容量是 `size * object_frags`，用于承接一笔 object 级别的大 H2D。

这三者对应的路径是：

```text
HostCopyBuffer 连续 Host 区
-> DeviceCopyBuffer staging slot 连续 Device 区
-> FragmentedDeviceCopyBuffer 离散 Device fragments
```

`@dev-sandbox/module/copy/ascend/copy_buffer_ascend.h`

## logical object 是什么

`H2DFFTSYuanrongPipelineCopyInstance` 里用 `PipelineObjectRange` 描述一个 logical object：

```text
firstFragment: 这个 object 从第几个 fragment 开始
fragmentCount: 这个 object 包含几个 fragment
bytes: fragmentCount * size
```

它是 benchmark 为模拟 yuanrong object/blob 关系切出来的调度单位，不是用户侧真实对象系统里的 object。对于这个 case：

- 一轮 iteration 的总 payload 仍然是 `size * number`。
- `number` 个目标 fragments 会按 `object_frags` 分组。
- 每组就是一个 logical object。
- 最后一组不够 `object_frags` 时，按剩余 fragment 数提交。

这个 object 边界很关键。没有它，代码只能表达“一个大 H2D 后一次 FFTS split”，也就是 `ascend_h2d_ffts_split` 的语义；有了它，才能在一轮 iteration 内形成多个 object，让 slot 0 和 slot 1 轮转起来。

`@dev-sandbox/module/copy/ascend/copy_instance_ffts_pipeline_ascend.h`

## instance 的资源布局

`H2DFFTSYuanrongPipelineCopyInstance` 直接继承 `CopyInstance`，没有复用 `FftsPipelineCopyInstanceBase`。原因是老 base 假设只有一条 stream 和一个 transfer buffer，而 yuanrong-style pipeline 必须同时管理两条 stream 和两个 staging slot。

核心成员可以按四组理解。

第一组是输入和 object 参数：

```text
src_
dst_
configuredObjectFrags_
objectFrags_
size_
number_
maxObjectBytes_
objects_
```

第二组是 stream 和总计时 event：

```text
h2dStream_
fftsStream_
totalStart_
totalEnd_
```

第三组是 slot 生命周期 event：

```text
slotReady_
slotFree_
```

`slotReady_` 的语义是“这个 slot 的 H2D 已完成，FFTS 可以读”。它对应 yuanrong 里的 `toPinDone` 语义。

`slotFree_` 的语义是“这个 slot 的 FFTS split 已完成，H2D 可以复用”。它对应 yuanrong 里的 `toDestDone` 语义。

第四组是 staging 和 FFTS 提交资源：

```text
transferBuffers_
objectCopies_
objectDispatchers_
```

`transferBuffers_` 固定两个，形成 pipeline depth 为 2 的双缓冲。`objectCopies_` 是当前 object 的 D2D split specs。`objectDispatchers_` 是每个 object 一个 dispatcher，避免不同 object 的 FFTS context 混在一起。

`@dev-sandbox/module/copy/ascend/copy_instance_ffts_pipeline_ascend.h`

## Prepare 路径

`Prepare` 是资源初始化和 object 切分的入口。它的执行顺序可以这样读：

1. 校验只有一个 src buffer 和一个 dst buffer。
2. 校验源和目的的 fragment 数量、单 fragment 大小一致。
3. 根据 affinity 选择当前 device。
4. 保存 `size_` 和 `number_`。
5. 把配置的 `object_frags` 裁剪到不超过 `number_`。
6. 计算单个 staging slot 的最大字节数 `maxObjectBytes_`。
7. 调用 `BuildObjectRanges` 把 `number_` 个 fragment 切成多个 logical object。
8. 创建 H2D stream 和 FFTS stream。
9. 创建总计时 event。
10. 对两个 slot 分别创建 `slotReady_` 和 `slotFree_`。
11. 对两个 slot 分别分配连续 device staging buffer。
12. 先在 H2D stream 上 record 两个 `slotFree_`，表示两个 slot 初始可用。

最后一步很重要。后续每个 object 的 H2D 都会先 wait 对应 slot 的 `slotFree_`。如果初始不 record，第一轮 H2D 会等待一个没有有效状态的 event。

`@dev-sandbox/module/copy/ascend/copy_instance_ffts_pipeline_ascend.h`

## DoCopyOnce 路径

`DoCopyOnce` 是一次 benchmark iteration 的真实提交路径。它用 H2D stream 做总计时协调流：

1. 在 H2D stream 上记录 `totalStart_`。
2. 让 FFTS stream 等待 `totalStart_`，保证 FFTS 工作不会跑到计时窗口之前。
3. 在 Host 侧开始记录 submit 时间。
4. 遍历所有 logical object，逐个调用 `SubmitObject`。
5. Host submit 计时结束，得到 `Submit(us)`。
6. H2D stream 等待两个 slot 的 `slotFree_`。
7. 在 H2D stream 上记录 `totalEnd_`。
8. 同步 H2D stream。
9. 用 `aclrtEventElapsedTime` 得到 `Copy(us)`。

第 6 步是为了把 FFTS stream 上的尾部工作 join 回 H2D stream。因为最后一个 object 的 FFTS split 可能还在 FFTS stream 上运行，只有等两个 slot 都再次 free，才能说明整条 pipeline 已经完成。

`Submit(us)` 统计的是 Host 侧把整轮 object 的 H2D、event wait、FFTS launch 都提交出去的耗时。

`Copy(us)` 统计的是从第一笔 H2D 开始，到最后一个 FFTS split 释放 slot 为止的 device 侧端到端时间。

`@dev-sandbox/module/copy/ascend/copy_instance_ffts_pipeline_ascend.h`

## DoCopyOnce 事件同步和计时细节

`DoCopyOnce` 本身不是同步执行每个 object 的函数。它的主要工作是在两条 stream 上把一整轮 iteration 的异步任务排好队，再用 event 把 FFTS stream 的尾部工作汇合回 H2D stream，最后统计这一整轮 pipeline 的时间。

可以先把它看成三段：

```text
打开计时窗口
-> 提交所有 logical object
-> 等待两个 slot 都释放并记录结束时间
```

第一段是打开计时窗口：

```text
h2dStream_ record totalStart_
fftsStream_ wait totalStart_
```

`totalStart_` 记录在 `h2dStream_` 上。随后 `fftsStream_` 等待这个 event，保证后续 FFTS split 不会跑到计时窗口之前。因为这个 case 有两条 stream，如果不让 FFTS stream 等 `totalStart_`，理论上 FFTS stream 上的任务可能和计时窗口边界错位。

第二段是提交所有 object：

```text
submitStart = host now
for each object:
    SubmitObject(objectIndex, object)
submitCost = host now - submitStart
```

这里得到的是 `Submit(us)`。它是 Host 侧提交开销，表示 CPU 把这一轮 iteration 内所有 object 的 H2D、event wait、FFTS launch 都 enqueue 出去花了多久。它不是 device 真正搬完数据的时间。

`SubmitObject` 内部才是真正串起流水线的地方：

```text
slot = objectIndex % 2

h2dStream_ wait slotFree_[slot]
h2dStream_ memcpy object -> transfer slot
h2dStream_ record slotReady_[slot]

fftsStream_ wait slotReady_[slot]
BuildObjectCopies(slot, object)
dispatcher BuildCopies
dispatcher Launch on fftsStream_
fftsStream_ record slotFree_[slot]
```

这里有两个 slot event：

```text
slotReady_[slot]: H2D 已经把 object 写进 staging slot，FFTS 可以读
slotFree_[slot]: FFTS 已经读完 staging slot 并完成 split，H2D 可以复用
```

单个 slot 的生命周期是：

```text
slotFree
-> H2D stream 写 staging slot
-> slotReady
-> FFTS stream 读 staging slot 并 split 到目标 fragments
-> slotFree
```

因此，每个 object 不是完整同步完再提交下一个 object。Host 侧只是把依赖关系提交到两个 stream。真实执行时，H2D stream 和 FFTS stream 可以在不同 slot 上重叠：

```text
time ---------------------------------------------------->

H2D stream   wait free0  H2D obj0 -> slot0  wait free1  H2D obj1 -> slot1  wait free0  H2D obj2 -> slot0
FFTS stream                            FFTS obj0 <- slot0           FFTS obj1 <- slot1           FFTS obj2 <- slot0
```

这段重叠成立的前提是两个 stream 操作的是不同 slot。例如 H2D stream 写 `slot1` 时，FFTS stream 可以读 `slot0`。但 H2D stream 想重新写 `slot0` 时，必须等待 `slot0` 上一次 FFTS split 记录 `slotFree_`，否则会覆盖 FFTS 还没读完的 staging 数据。

第三段是收尾统计：

```text
for each slot:
    h2dStream_ wait slotFree_[slot]
h2dStream_ record totalEnd_
h2dStream_ synchronize
event elapsed time: totalStart_ -> totalEnd_
```

这里为什么要等两个 `slotFree_`？因为最后的 FFTS split 是在 `fftsStream_` 上执行的，而 `totalEnd_` 要记录在 `h2dStream_` 上。如果 `h2dStream_` 不等待 `slotFree_`，`totalEnd_` 可能只表示 H2D stream 自己的任务排完了，不表示 FFTS stream 上最后一个 split 已经完成。

所以 `slotFree_` 在收尾阶段还有一个作用：把 FFTS stream 的完成状态 join 回 H2D stream。等两个 slot 都 free，就说明所有可能还在使用 staging slot 的 FFTS split 都完成了。此时再 record `totalEnd_`，`aclrtEventElapsedTime(totalStart_, totalEnd_)` 才能代表整轮 pipeline 的端到端 device 时间。

完整依赖链可以画成这样：

```text
totalStart_ on h2dStream_
    |
    +-- h2dStream_ submits object H2D copies
    |
    +-- fftsStream_ waits totalStart_
          |
          +-- waits each slotReady_
          +-- launches FFTS split
          +-- records each slotFree_
                    |
                    v
h2dStream_ waits all slotFree_
    |
totalEnd_ on h2dStream_
```

最后 `aclrtSynchronizeStream(h2dStream_)` 等的是 H2D stream，但因为 H2D stream 已经等待了两个 `slotFree_`，而 `slotFree_` 是 FFTS stream 在 split 后记录的，所以这个同步也间接保证 FFTS stream 的尾部 split 已经完成。

一句话总结：`DoCopyOnce` 用 `totalStart_` 和 `totalEnd_` 统计整轮端到端 device 时间，用 `slotReady_` 表示 H2D 阶段完成、FFTS 可以读，用 `slotFree_` 表示 FFTS 阶段完成、H2D 可以复用 slot；真正的流水线重叠来自 `h2dStream_` 和 `fftsStream_` 在不同 staging slot 上被 event 串起来。

`@dev-sandbox/module/copy/ascend/copy_instance_ffts_pipeline_ascend.h`

## SubmitObject 是核心

`SubmitObject` 是这条 case 最值得精读的函数。它把一个 logical object 串成“等 slot、H2D、通知 FFTS、提交 split、释放 slot”。

单个 object 的顺序是：

1. 用 `objectIndex % 2` 选择 slot。
2. 在 H2D stream 上等待这个 slot 的 `slotFree_`。
3. 把 `src[firstFragment]` 开始的连续 Host object 数据拷到当前 staging slot 首地址。
4. H2D stream 记录 `slotReady_`。
5. FFTS stream 等待 `slotReady_`。
6. 调用 `BuildObjectCopies`，为当前 object 的每个 fragment 生成一条 D2D copy spec。
7. 当前 object 的 dispatcher 调用 `BuildCopies`。
8. dispatcher 在 FFTS stream 上 `Launch`。
9. FFTS stream 记录 `slotFree_`，释放这个 slot。

这里的 H2D 是一笔 object 级别的大 H2D。目的地址是当前 staging slot 的首地址，源地址是 Host 连续 buffer 中当前 object 的起点，真实 copy 大小是 `object.bytes`。传给 `aclrtMemcpyAsync` 的目的最大长度是 `maxObjectBytes_`，用于表达这个 staging slot 的容量；真实搬运长度仍然是当前 object 的字节数。

`@dev-sandbox/module/copy/ascend/copy_instance_ffts_pipeline_ascend.h`

## BuildObjectCopies 如何映射指针

`BuildObjectCopies` 把一个 logical object 转成 FFTS D2D split 列表。对于当前 object 内第 `i` 个 fragment：

```text
src = 当前 staging slot 的第 i 个 fragment 偏移
dst = 最终 fragmented device buffer 的 firstFragment + i
size = 单 fragment 大小
```

所以 FFTS 看到的不是 Host 指针，而是：

```text
device staging slot -> fragmented device buffer
```

这就是这个 case 名字里 `H2D + FFTS` 的准确含义：H2D 只负责把 object 搬到连续 device staging，FFTS 只负责 device 内部 split。

`@dev-sandbox/module/copy/ascend/copy_instance_ffts_pipeline_ascend.h`

## FFTS dispatcher 做了什么

`FftsD2DDispatcher` 不理解 object，也不理解 H2D。它只把一组 `AscendD2DCopySpec` 转成 FFTS Plus SDMA contexts，再 launch 到 runtime。

`BuildCopies` 的主要逻辑是：

1. 清空上一次 contexts。
2. 根据 copy spec 数量和 `FFTS_MAX_READY_LANES` 决定 ready lane 数。
3. 每个 fragment copy 生成一个 SDMA context。
4. 按 lane 做 round-robin。
5. 同一个 lane 上，前一个 context 依赖后一个 context，形成链。
6. 返回初始 ready context 数。

`Launch` 的主要逻辑是：

1. 构造 FFTS Plus SQE。
2. 设置 total context 数和 ready context 数。
3. 把 context buffer 地址填进 task info。
4. 调用 `rtFftsPlusTaskLaunchWithFlag`，把 task 提交到传入的 ACL stream。

这里的 `FFTS_MAX_READY_LANES` 和 `COPY_FFTS_PIPELINE_OBJECT_FRAGS` 是两个不同旋钮：

- `COPY_FFTS_PIPELINE_OBJECT_FRAGS` 决定一个 object 里有多少条 fragment split。
- `FFTS_MAX_READY_LANES` 决定这些 split context 里最多有多少条可以初始 ready。

`@dev-sandbox/module/copy/ascend/ffts_d2d_dispatcher_ascend.h`

## 双缓冲如何重叠

假设 `object_frags=8`，一轮 iteration 有多个 object，slot 选择如下：

```text
object 0 -> slot 0
object 1 -> slot 1
object 2 -> slot 0
object 3 -> slot 1
```

slot 生命周期是：

```text
slotFree -> H2D writes slot -> slotReady -> FFTS reads slot -> slotFree
```

因此 H2D stream 可以在 slot 1 上提交 object 1 的 H2D，同时 FFTS stream 在 slot 0 上处理 object 0 的 split。等 object 2 要重新使用 slot 0 时，H2D stream 必须先等 object 0 的 FFTS split 记录 `slotFree_`。

可以把它画成一条横向时间轴。下面的图里，`H2D(objN -> slotM)` 表示把第 N 个 object 做大 H2D 写进第 M 个 staging slot；`FFTS(objN <- slotM)` 表示 FFTS 从第 M 个 staging slot 读出 object N，并 split 到最终 fragmented device buffers。

```text
time  ------------------------------------------------------------>

H2D stream   wait slot0 free   H2D(obj0 -> slot0)   wait slot1 free   H2D(obj1 -> slot1)   wait slot0 free   H2D(obj2 -> slot0)   wait slot1 free   H2D(obj3 -> slot1)
                                  | slot0 ready                          | slot1 ready                          | slot0 ready                          | slot1 ready
                                  v                                      v                                      v                                      v
FFTS stream                        FFTS(obj0 <- slot0)                   FFTS(obj1 <- slot1)                   FFTS(obj2 <- slot0)                   FFTS(obj3 <- slot1)
                                       | slot0 free                            | slot1 free                            | slot0 free                            | slot1 free
```

前两个 `wait slot0 free` 和 `wait slot1 free` 通常不会形成实际等待，因为 `Prepare` 阶段已经先把两个 `slotFree_` record 到 `h2dStream_`，表示两个 slot 初始都是空闲的。后面的 `wait slot0 free`、`wait slot1 free` 就是实际复用保护：`obj2` 复用 slot0 前必须等 `obj0` 的 FFTS 完成，`obj3` 复用 slot1 前必须等 `obj1` 的 FFTS 完成。

上面真正发生重叠的是这种关系：

```text
time  ------------------------------------------------------------>

H2D stream   [ H2D obj0 on slot0 ][ H2D obj1 on slot1 ][ wait slot0 ][ H2D obj2 on slot0 ][ wait slot1 ][ H2D obj3 on slot1 ]
FFTS stream                      [ FFTS obj0 on slot0 ][ FFTS obj1 on slot1 ][ FFTS obj2 on slot0 ][ FFTS obj3 on slot1 ]
                                ^^^^^^^^^^^^^^^^^^^^^
                                这里就是重叠：H2D 在写 slot1，FFTS 在读 slot0
```

如果不画 wait，只看两条 stream 上的工作段，重叠关系更直观：

```text
time ------------------------------------------------------------>

H2D stream    H2D obj0(slot0)   H2D obj1(slot1)   H2D obj2(slot0)   H2D obj3(slot1)
FFTS stream                    FFTS obj0(slot0)  FFTS obj1(slot1)  FFTS obj2(slot0)  FFTS obj3(slot1)
                              <--- overlap ---> <--- overlap ---> <--- overlap --->
```

这里三个 overlap 分别表示：

- `H2D obj1(slot1)` 可以和 `FFTS obj0(slot0)` 重叠。
- `H2D obj2(slot0)` 可以和 `FFTS obj1(slot1)` 重叠。
- `H2D obj3(slot1)` 可以和 `FFTS obj2(slot0)` 重叠。

也就是说，`slot0` 的 H2D 和 `slot1` 的 FFTS 可以重叠，`slot1` 的 H2D 和 `slot0` 的 FFTS 也可以重叠。不能重叠的是同一个 slot 的写和读：例如 `H2D obj2(slot0)` 必须等 `FFTS obj0(slot0)` 释放 `slot0`。

换句话说，双缓冲不是让同一个 slot 同时读写。它是让两个 slot 交替承担不同阶段：

```text
同一时刻：
slot0 可能正在被 FFTS 读
slot1 可能正在被 H2D 写

下一段时间：
slot0 被 FFTS 释放后，H2D 才能重新写 slot0
slot1 写完后，FFTS 才能读取 slot1
```

这就是它和 `ascend_h2d_ffts_split` 的核心区别：

- `ascend_h2d_ffts_split` 是一轮 iteration 里一次大 H2D，然后一次 FFTS split。
- `ascend_h2d_ffts_yuanrong_pipeline` 是一轮 iteration 里多个 logical object，每个 object 一次 H2D 加一次 FFTS split，两个 slot 轮转。

当 `object_frags` 接近 `number` 时，新 case 会退化得更像单 object 两段式路径；当 `object_frags` 太小时，H2D 和 FFTS launch 次数会明显增多，提交开销会被放大。中间值才是观察 pipeline overlap 的主要区间。

`@dev-sandbox/module/copy/ascend/copy_instance_ffts_pipeline_ascend.h`

## 和 yuanrong 原路径的对应关系

这个 benchmark case 借的是 yuanrong 的 device 侧调度思想，不是把 DataSystem 整条对象缓存路径搬过来。

yuanrong 原模型可以概括为：

```text
host object 连续数据
-> device 中转 buffer
-> 多个 device blob
```

本 case 的模型是：

```text
HostCopyBuffer 中的一段 logical object
-> DeviceCopyBuffer staging slot
-> FragmentedDeviceCopyBuffer 中的多个 fragments
```

对应关系如下：

```text
yuanrong host object       <-> benchmark logical object
yuanrong device blob list  <-> FragmentedDeviceCopyBuffer 的一组 fragments
yuanrong toPinDone         <-> slotReady_
yuanrong toDestDone        <-> slotFree_
yuanrong secondaryStream   <-> h2dStream_
yuanrong primaryStream     <-> fftsStream_
```

本 case 没有实现 yuanrong 的 host staging 线程，也没有 DataSystem 的 object metadata、worker fetch、buffer composer 等逻辑。原因是 copy benchmark 的 `HostCopyBuffer` 已经是连续 Host pinned memory，可以直接表达“一个 object 的连续 Host 数据”。这个 case 主要验证的是 device staging slot + FFTS split 的双缓冲调度。

`@yuanrong-datasystem/src/datasystem/common/device/ascend/acl_resource_manager.h`

`@yuanrong-datasystem/src/datasystem/common/device/ascend/acl_resource_manager.cpp`

## 结果口径

结果表里的 `Method` 会显示为 `H2D+YPipe`。标题里会附带实际生效的 `object_frags`。

`Count` 仍然是总 fragment 数，不是 object 数。带宽计算仍然按 `size * count / Copy(us)`，所以它和 `ascend_h2d_ffts_split` 可以在总 payload 口径上对比。

但解释结果时要注意：

- `Submit(us)` 包含多个 object 的 H2D、event wait 和 FFTS launch 提交。
- `Copy(us)` 是整轮 pipeline 的端到端 device 时间。
- `object_frags` 会同时影响 object 数、H2D 粒度、FFTS launch 次数和可重叠程度。
- `FFTS_MAX_READY_LANES` 只影响单个 object 内部 FFTS split 的 ready lane 宽度。

`@dev-sandbox/module/copy/copy_result.h`

## 推荐读代码顺序

第一步读 case 是否会被编进 binary：

`@dev-sandbox/module/copy/CMakeLists.txt`

第二步读命令行如何找到 case：

`@dev-sandbox/module/copy/copy_main.cc`

`@dev-sandbox/module/copy/copy_case.h`

第三步读 case 体，确认输入 buffer 和 instance：

`@dev-sandbox/module/copy/ascend/copy_case_ffts_d2d_ascend.cc`

第四步读三类 buffer 的内存形态：

`@dev-sandbox/module/copy/ascend/copy_buffer_ascend.h`

第五步读 instance 成员和 `Prepare`：

`@dev-sandbox/module/copy/ascend/copy_instance_ffts_pipeline_ascend.h`

第六步读 `DoCopyOnce` 和 `SubmitObject`：

`@dev-sandbox/module/copy/ascend/copy_instance_ffts_pipeline_ascend.h`

第七步读 FFTS context 构造和 launch：

`@dev-sandbox/module/copy/ascend/ffts_d2d_dispatcher_ascend.h`

## 抓手

读这段代码时始终问四个问题：

1. 当前处理的是 fragment、logical object，还是整轮 iteration？
2. 当前指针在 Host、device staging slot，还是最终 fragmented device buffer？
3. 当前 stream 是 H2D stream，还是 FFTS stream？
4. 当前 event 表示 slot ready，还是 slot free？

这四个问题能答清楚，`ascend_h2d_ffts_yuanrong_pipeline` 就不再是一团 stream/event/FFTS context 交织的代码，而是一条很清楚的两段式双缓冲流水线。
