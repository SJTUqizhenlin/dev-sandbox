# dev-sandbox 代码结构走读指南

这份指南面向“不熟 C++，但想快速读懂这个项目”的读者。目标不是把 C++ 一次学完，而是先看懂这个项目的组织方式、程序从哪里开始、数据怎么流动、Ascend/CUDA/模拟后端怎么切换。

## 1. 先建立整体地图

这个项目是一个 C++17 + CMake 的性能实验项目，主要生成几个可执行程序：

- `logger`：日志工具和示例。
- `aio`：异步文件 I/O 性能测试。
- `copy`：内存拷贝性能测试，覆盖 host/device/anonymous/device-to-device 等 case。
- `trans`：H2D/D2H 数据传输性能测试，按方向、host buffer 类型、device buffer 类型、传输方法组合过滤。

顶层构建入口：

`@dev-sandbox/CMakeLists.txt`

模块入口：

`@dev-sandbox/module/CMakeLists.txt`

后端检测逻辑：

`@dev-sandbox/cmake/DetectRuntime.cmake`

三种后端目录命名很规律：

- `cuda/`：CUDA 后端。
- `ascend/`：Ascend 后端。
- `simu/`：CPU 模拟后端。

读代码时可以先只看 `ascend/`，因为你的主要问题集中在 Ascend 上。等理解一条路径后，再对比 `cuda/` 和 `simu/`。

## 2. 构建系统怎么决定用哪个后端

入口是顶层 CMake：

`@dev-sandbox/CMakeLists.txt`

它会 include 运行时检测模块：

`@dev-sandbox/cmake/DetectRuntime.cmake`

检测顺序大致是：

1. 先找 CUDA。
2. 再找 Ascend。
3. 都没有就走 Simulation。

然后各模块的 `CMakeLists.txt` 会根据检测结果添加不同源文件。以 `trans` 为例：

`@dev-sandbox/module/trans/CMakeLists.txt`

如果 `ASCEND_FOUND`，就编译：

`@dev-sandbox/module/trans/ascend/trans_runtime_ascend.cc`

`@dev-sandbox/module/trans/ascend/trans_case_ascend.cc`

这意味着你读 Ascend 行为时，不需要同时脑补 CUDA 文件。编译期只会选中一套后端实现。

## 3. 建议阅读顺序

### 第一轮：只看入口和参数

先看可执行程序入口，也就是 `main`：

`@dev-sandbox/module/copy/copy_main.cc`

`@dev-sandbox/module/trans/trans_main.cc`

重点看这些问题：

- 命令行参数有哪些？
- 参数被放进哪个 `Context` 结构？
- 最后调用了哪个 `Run`、`DoCopy` 或 `TransOne`？

暂时不用纠结模板、继承、宏。第一轮只要知道“用户参数最后变成上下文对象，然后交给 case 跑”。

### 第二轮：看 case 注册和过滤

`copy` 的 case 定义：

`@dev-sandbox/module/copy/copy_case.h`

Ascend copy case：

`@dev-sandbox/module/copy/ascend/copy_case_ascend.cc`

`trans` 的 case 定义：

`@dev-sandbox/module/trans/trans_case.h`

Ascend trans case：

`@dev-sandbox/module/trans/ascend/trans_case_ascend.cc`

这里有两个 C++ 初学者容易卡住的点。

第一，`DEFINE_COPY_CASE`、`DEFINE_TRANS_H2D_CASE` 这些是宏。可以把它们理解成“帮你生成一个 case 类，并自动注册到工厂里”。不需要一开始完全展开宏，只要知道它让字符串 case 名和真正的 `Run` 函数绑在了一起。

第二，`Factory` 是工厂。它保存所有注册过的 case，运行时根据命令行参数过滤出要跑的 case。

### 第三轮：看 buffer 怎么分配

`copy` 的通用 buffer 抽象：

`@dev-sandbox/module/copy/copy_buffer.h`

Ascend copy buffer：

`@dev-sandbox/module/copy/ascend/copy_buffer_ascend.h`

`trans` 的通用 buffer 抽象：

`@dev-sandbox/module/trans/trans_buffer.h`

Ascend trans host buffer：

`@dev-sandbox/module/trans/ascend/trans_host_buffer_ascend.h`

Ascend trans device buffer：

`@dev-sandbox/module/trans/ascend/trans_device_buffer_ascend.h`

这个项目里的核心模式是：

```text
构造 buffer 时一次分配 size * number
operator[](i) 返回 addr + i * size
拷贝时循环提交第 i 片
析构 buffer 时释放整块内存
```

也就是说，不是每次 memcpy 都 malloc。分配和释放在 buffer 生命周期里，真正测量的循环里主要是提交异步拷贝和同步等待。

### 第四轮：看拷贝模板或实例

`copy` 的通用拷贝实例：

`@dev-sandbox/module/copy/copy_instance.h`

Ascend copy 实现：

`@dev-sandbox/module/copy/ascend/copy_instance_ascend.h`

`trans` 的通用传输模板：

`@dev-sandbox/module/trans/trans_template.h`

Ascend trans 实现：

`@dev-sandbox/module/trans/ascend/trans_template_ascend.h`

你可以抓住三个阶段：

```text
Prepare / OnTransPre
  建 stream/event
  收集 src[i] 和 dst[i] 地址

DoCopyOnce / OnTrans
  记录 start event
  提交 aclrtMemcpyAsync 或 aclrtMemcpyBatchAsync
  记录 end event
  synchronize
  计算耗时

Cleanup / OnTransPost
  销毁 stream/event
```

## 4. copy 模块怎么读

`copy` 更像“按 case 名跑一个具体拷贝实验”。

入口：

`@dev-sandbox/module/copy/copy_main.cc`

核心抽象：

- `CopyCase`：一个实验 case。
- `CopyBuffer`：一块源或目标内存。
- `CopyInstance`：一次拷贝方法，比如 CE、Batch CE、多 stream。
- `CopyResult`：统计结果和带宽输出。
- `CopyRuntime`：初始化和释放运行时。

Ascend H2D 的一条典型路径：

```text
copy_main.cc
  -> CopyCaseFactory::Filter
  -> Host2DeviceCECase::Run
  -> HostCopyBuffer
      aclrtMallocHost(size * num)
  -> DeviceCopyBuffer
      aclrtMalloc(size * num)
  -> H2DCECopyInstance::DoCopy
  -> AscendCopyInstanceBase::Prepare
      src[i] = host_base + i * size
      dst[i] = device_base + i * size
  -> warmup 3 次
  -> iter 次 DoCopyOnce
      aclrtMemcpyAsync(dst[i], size, src[i], size, ACL_MEMCPY_HOST_TO_DEVICE, stream)
  -> Cleanup
```

对应文件：

`@dev-sandbox/module/copy/ascend/copy_case_ascend.cc`

`@dev-sandbox/module/copy/ascend/copy_buffer_ascend.h`

`@dev-sandbox/module/copy/ascend/copy_instance_ascend.h`

## 5. trans 模块怎么读

`trans` 更像“专门比较 H2D/D2H 传输组合的工具”。

入口：

`@dev-sandbox/module/trans/trans_main.cc`

核心抽象：

- `TransCase`：一个传输 case。
- `TransBuffer`：host 或 device buffer。
- `TransTemplate`：传输方法模板，比如 CE、Batch CE、多 stream。
- `TransResult`：统计结果和带宽输出。
- `TransRuntime`：初始化和释放运行时。

命令行参数和含义：

- `-t H2D` 或 `-t D2H`：方向。
- `-H normal/anonymous/registered`：host buffer 类型。
- `-D normal`：device buffer 类型。
- `-M ce/batch_ce/ms_48`：传输方法。
- `-s`：每个数据块大小，单位是字节。
- `-n`：数据块数量。
- `-d`：设备数量。
- `-i`：测量迭代次数。

Ascend H2D 的一条典型路径：

```text
trans_main.cc
  -> TransCaseFactory::Filter
  -> H2D case Run
  -> TransHostNormalBuffer
      aclrtMallocHost(size * number)
  -> TransDeviceNormalBuffer
      aclrtMalloc(size * number)
  -> TransH2DCETemplate::TransOne
  -> TransStreamTemplate::OnTransPre
      src[i] = host_base + i * size
      dst[i] = device_base + i * size
  -> warmup 3 次
  -> iteration 次 OnTrans
      aclrtMemcpyAsync(dst[i], size, src[i], size, ACL_MEMCPY_HOST_TO_DEVICE, stream)
  -> OnTransPost
```

对应文件：

`@dev-sandbox/module/trans/ascend/trans_case_ascend.cc`

`@dev-sandbox/module/trans/ascend/trans_host_buffer_ascend.h`

`@dev-sandbox/module/trans/ascend/trans_device_buffer_ascend.h`

`@dev-sandbox/module/trans/ascend/trans_template_ascend.h`

## 6. copy 和 trans 的区别

`copy` 的关注点是“一个命名 case”。比如：

```bash
./copy -t host_to_device_ce -s 512M -n 8 -i 128 -d 8
```

它适合你已经知道要跑哪个 case，直接按 case 名执行。

`trans` 的关注点是“组合过滤”。比如：

```bash
./trans -t H2D -H normal -D normal -M ce -s 32768 -n 1024 -d 8 -i 1024
```

它适合系统比较：

```text
方向 H2D/D2H
  x host buffer 类型
  x device buffer 类型
  x 方法 ce/batch_ce/ms_48
```

两者底层思想很像：先分配 buffer，再切片，再循环提交拷贝，再统计带宽。

## 7. AIO 模块怎么读

如果你现在主要看 Ascend 拷贝，可以把 `aio` 放到后面。它是文件 I/O 性能测试，依赖 Linux native AIO、epoll、eventfd、O_DIRECT 等接口。

入口：

`@dev-sandbox/module/aio/aio_main.cc`

核心文件：

`@dev-sandbox/module/aio/aio_engine.h`

`@dev-sandbox/module/aio/aio_impl.h`

`@dev-sandbox/module/aio/aio_impl.cc`

`@dev-sandbox/module/aio/space_layout.h`

`@dev-sandbox/module/aio/host_buffer.h`

阅读顺序建议：

1. 先看 `aio_main.cc` 的参数。
2. 再看 `AioEngine` 怎么提交读写。
3. 最后再看 `AioImpl` 封装的 Linux syscall。

这部分 Linux 系统调用比较多，不建议作为第一个学习入口。

## 8. logger 模块怎么读

日志模块比较独立，适合当 C++ 练手材料。

核心文件：

`@dev-sandbox/module/logger/logger.h`

`@dev-sandbox/module/logger/logger.cc`

`@dev-sandbox/module/logger/logger_example.cc`

可以重点看：

- 单例模式：全局只有一个 logger。
- 后台线程：异步 flush 日志。
- 宏：`LOG_INFO`、`LOG_ERROR` 这类调用如何自动带上文件名、行号、函数名。

## 9. 不会 C++ 时，先学这些就够读本项目

不用先完整刷一本 C++。为了读这个项目，建议按下面顺序补概念。

### 必须先懂

- 函数和参数：知道 `main(argc, argv)` 是入口。
- `struct` / `class`：知道它们在这里主要用来组织数据和行为。
- 构造函数 / 析构函数：构造时分配资源，析构时释放资源。
- 继承和虚函数：父类定义接口，后端子类实现细节。
- 指针和 `void*`：内存地址，Ascend/CUDA API 大量使用。
- `std::vector`：动态数组，这里常用来保存 src/dst 地址列表。
- `std::size_t`：无符号整数，常用来表示大小和数量。

### 读这个项目很常见

- RAII：对象创建时拿资源，对象销毁时释放资源。比如 buffer 构造时 malloc，析构时 free。
- Factory：工厂保存所有 case，根据名字找到要跑的 case。
- Macro：宏帮忙生成重复代码，比如自动注册 case。
- Template 命名：本项目里的 `TransTemplate` 不是 C++ 模板语法，而是“传输流程模板”的普通类名。

### 可以晚点学

- 完整模板元编程。
- STL 复杂算法。
- 移动语义细节。
- CMake 高级写法。
- CUDA kernel 细节。

## 10. 一条高效学习路线

建议用 5 天节奏读：

### 第 1 天：会跑命令，知道产物在哪

看：

`@dev-sandbox/CMakeLists.txt`

`@dev-sandbox/module/CMakeLists.txt`

`@dev-sandbox/module/copy/copy_main.cc`

`@dev-sandbox/module/trans/trans_main.cc`

目标：

- 知道项目生成哪些程序。
- 知道 `copy` 和 `trans` 参数含义。
- 知道 `-d` 是设备数量，不是某个单独 device id。

### 第 2 天：只追 H2D CE

看：

`@dev-sandbox/module/trans/ascend/trans_case_ascend.cc`

`@dev-sandbox/module/trans/ascend/trans_host_buffer_ascend.h`

`@dev-sandbox/module/trans/ascend/trans_device_buffer_ascend.h`

`@dev-sandbox/module/trans/ascend/trans_template_ascend.h`

目标：

- 画出 `aclrtMallocHost`、`aclrtMalloc`、`aclrtMemcpyAsync` 的关系。
- 看懂一次分配整块，按 `size` 切片，多次提交 memcpy。

### 第 3 天：对比 Batch CE 和多 stream

继续看：

`@dev-sandbox/module/trans/ascend/trans_template_ascend.h`

目标：

- `ce`：循环多次 `aclrtMemcpyAsync`。
- `batch_ce`：一次 `aclrtMemcpyBatchAsync` 提交地址数组。
- `ms_48`：把 `number` 个数据块分给 48 个 stream。

### 第 4 天：看 copy 的同构设计

看：

`@dev-sandbox/module/copy/ascend/copy_case_ascend.cc`

`@dev-sandbox/module/copy/ascend/copy_buffer_ascend.h`

`@dev-sandbox/module/copy/ascend/copy_instance_ascend.h`

目标：

- 理解 `copy` 和 `trans` 的结构相似点。
- 理解为什么 `copy` 更像 case 列表，`trans` 更像组合矩阵。

### 第 5 天：再看 CMake 和 runtime

看：

`@dev-sandbox/cmake/DetectRuntime.cmake`

`@dev-sandbox/module/copy/ascend/copy_runtime_ascend.cc`

`@dev-sandbox/module/trans/ascend/trans_runtime_ascend.cc`

目标：

- 知道后端怎么被选中。
- 知道 Ascend runtime 初始化和 finalize 在哪里。

## 11. 读代码时可以反复问自己的问题

遇到一个函数，先问：

- 它是在分配资源，还是提交任务，还是统计结果？
- 它跑在构造阶段、prepare 阶段、iteration 阶段，还是 cleanup 阶段？
- 它处理的是整块 buffer，还是其中一个 `i` 切片？
- 它是通用抽象，还是 Ascend/CUDA/simu 的具体实现？

遇到一个类，先问：

- 这个类是不是一个接口？
- 谁继承它？
- 谁创建它？
- 它的析构函数会释放什么资源？

遇到一个地址，先问：

- 这个地址来自 host 还是 device？
- 它是 base address，还是 `base + i * size` 的切片地址？
- 它最终传给了 `src` 还是 `dst`？

## 12. 推荐从这里开始做笔记

你可以先画一张 H2D 图：

```text
Host buffer base
  aclrtMallocHost(size * number)
  |
  +-- src[0] = base + 0 * size
  +-- src[1] = base + 1 * size
  +-- ...

Device buffer base
  aclrtMalloc(size * number)
  |
  +-- dst[0] = base + 0 * size
  +-- dst[1] = base + 1 * size
  +-- ...

Copy loop
  aclrtMemcpyAsync(dst[i], size, src[i], size, HOST_TO_DEVICE, stream)
```

这张图理解了，`copy` 和 `trans` 里大部分 Ascend H2D 代码就都能顺着读下去了。

