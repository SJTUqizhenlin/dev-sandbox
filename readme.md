# dev-sandbox

C++17 性能测试项目，使用 CMake 构建，支持 CUDA、Ascend 和 CPU 模拟后端。

## 构建

```bash
cmake -B build
cmake --build build -j
```

构建完成后，`copy` 可执行文件通常位于：

```bash
./build/module/copy/copy
```

## copy 公共参数

所有 `copy` case 都通过 `-t` 指定 case 名，其他参数控制数据规模、迭代次数和设备数。

```text
-t <name>   case 名称，可重复指定多个 -t
-s <size>   单个数据块大小，例如 16K、1M，默认 512M
-n <count>  每个 buffer 内的数据块数量，默认 8
-i <count>  迭代次数，默认 128
-d <count>  设备数量，默认 8
```

查看当前后端可用 case：

```bash
./build/module/copy/copy -t unknown
```

## copy case 总览

### CUDA / Ascend CE

`CE` 表示使用设备 copy engine 做拷贝。CUDA 和 Ascend 后端会根据当前构建环境注册各自支持的
case。

| case | 后端 | 传输方向 | 说明 |
| --- | --- | --- | --- |
| `host_to_device_ce` | CUDA / Ascend | host -> device | 逐设备 H2D 拷贝 |
| `host_to_device_batch_ce` | CUDA / Ascend | host -> device | 使用 batch CE 提交 H2D 拷贝 |
| `one_host_to_all_device_ce` | CUDA / Ascend | host0 -> all devices | 同一份 host buffer 依次拷贝到所有 device |
| `huge_shm_to_device_ce` | Ascend | HugeTLB shared host -> device | 每个 device 使用一份 HugeTLB shared host buffer 做 H2D 拷贝 |
| `one_huge_shm_to_all_device_ce` | Ascend | HugeTLB shared host0 -> all devices | 同一份 HugeTLB shared host buffer 依次拷贝到所有 device |
| `all_host_to_all_device_ce` | CUDA / Ascend | host[i] -> device[i] | 多个 host/device buffer 一次批量提交 |
| `device_to_device_ce` | CUDA / Ascend | device -> device | 单设备内 D2D 拷贝 |
| `one_device_to_all_device_ce` | CUDA / Ascend | device0 -> all devices | 同一份 device buffer 依次拷贝到所有 device |
| `anonymous_to_device_ce` | CUDA / Ascend | anonymous host -> device | 从匿名 host 内存拷贝到 device |

### CUDA 专属

| case | 传输方向 | 说明 |
| --- | --- | --- |
| `device_to_host_ce` | device -> host | 逐设备 D2H 拷贝 |
| `device_to_host_batch_ce` | device -> host | 使用 batch CE 提交 D2H 拷贝 |
| `host_to_device_sm` | host -> device | 使用 CUDA SM kernel 做 H2D 拷贝 |
| `device_to_host_sm` | device -> host | 使用 CUDA SM kernel 做 D2H 拷贝 |
| `one_host_to_all_device_sm` | host0 -> all devices | 同一份 host buffer 通过 SM 拷贝到所有 device |
| `device_to_anonymous_ce` | device -> anonymous host | 从 device 拷贝到匿名 host 内存 |
| `anonymous_to_device_sm` | anonymous host -> device | 使用 SM 从匿名 host 内存拷贝到 device |
| `device_to_anonymous_sm` | device -> anonymous host | 使用 SM 从 device 拷贝到匿名 host 内存 |

### Ascend 专属

| case | 传输方向 | 说明 |
| --- | --- | --- |
| `host_to_device_ce_multi_stream` | host -> device | 使用多 stream 提交 H2D 拷贝 |
| `ascend_h2d_ffts_yuanrong_pipeline` | host -> fragmented device | Yuanrong 风格两段式 H2D，先 H2D 到 device staging，再用 FFTS 拆到 fragmented device buffer |
| `ascend_huge_shm_h2d_ffts_yuanrong_pipeline` | HugeTLB shared host -> fragmented device | 使用 HugeTLB shared host buffer 作为 Yuanrong pipeline 的 H2D 源 |

#### 运行 Yuanrong pipeline H2D

数据一致性校验：

```bash
COPY_FFTS_VALIDATE=1 COPY_FFTS_PIPELINE_OBJECT_FRAGS=8 \
./build/module/copy/copy -t ascend_h2d_ffts_yuanrong_pipeline -s 37K -n 1024 -i 4 -d 1
```

性能测试：

```bash
COPY_FFTS_VALIDATE=0 COPY_FFTS_PIPELINE_OBJECT_FRAGS=8 \
./build/module/copy/copy -t ascend_h2d_ffts_yuanrong_pipeline -s 37K -n 1024 -i 128 -d 1
```

HugeTLB shared host buffer 对比测试：

```bash
./build/module/copy/copy -t one_huge_shm_to_all_device_ce -s 2M -n 8 -i 128 -d 8
```

```bash
COPY_FFTS_VALIDATE=1 COPY_FFTS_PIPELINE_OBJECT_FRAGS=8 \
./build/module/copy/copy -t ascend_huge_shm_h2d_ffts_yuanrong_pipeline -s 37K -n 1024 -i 4 -d 1
```

HugeTLB shared host buffer 使用 `memfd_create` 申请 2MB HugeTLB 页，不需要提前挂载 hugetlbfs。
运行时会消耗系统预留的 HugeTLB 池，可以在宿主机观察 `HugePages_Free` 的变化：

```bash
watch -n 0.2 'grep -i Huge /proc/meminfo'
```

申请页数约为 `ceil(size * count / 2MB)`。进程启动创建 buffer 后 `HugePages_Free` 会下降，进程退出释放 buffer 后会回升。
`ShmemHugePages` 通常不会变化，它不是这个 HugeTLB memfd 路径的主要观测口径。

扫描 `COPY_FFTS_PIPELINE_OBJECT_FRAGS` 并和 `ascend_h2d_ffts_split` 比较：

```bash
python3 module/copy/ascend/scripts/run_h2d_ffts_object_frags_sweep.py \
  --copy-bin ./build/module/copy/copy \
  --io-size 37K \
  --buffer-num 1024 \
  --object-frags 1,2,4,8,16,32,64,128 \
  --iterations 128 \
  --device-count 1
```

### GDR

GDR case 注册在 `copy` 主程序中。CUDA 后端可用且系统检测到 `libibverbs` 头文件和库时，
才会编译 GDR case。

| case | 传输方向 | 说明 |
| --- | --- | --- |
| `host_to_device_gdr` | host -> device | 通过 RDMA write 逐设备写入对应 GPU |
| `one_host_to_all_device_gdr` | host0 -> all devices | 同一块 host buffer 向所有 GPU 并发提交 RDMA write |
| `all_host_to_all_device_gdr` | host[i] -> device[i] | 每张卡对应独立 host/device buffer，并发提交 RDMA write |

### 模拟后端

| case | 传输方向 | 说明 |
| --- | --- | --- |
| `host_to_anonymous_memcpy` | host -> anonymous host | CPU `memcpy` 模拟 host 到匿名内存 |
| `shm_to_all_host_memcpy` | shared memory -> all hosts | CPU `memcpy` 模拟共享内存到多个 host buffer |

## 环境变量

### GDR_NICS

GDR 使用 `GDR_NICS` 环境变量指定 GPU 与 RDMA 网卡的映射关系。

规则：

- 使用逗号分隔网卡名，不要写空格。
- 顺序按 device id 从 0 开始一一对应。
- 网卡数量必须与 `-d <count>` 指定的设备数量一致。

未设置 `GDR_NICS` 时使用默认映射：

```bash
mlx5_0,mlx5_2,mlx5_4,mlx5_6,mlx5_8,mlx5_10,mlx5_12,mlx5_14
```

8 卡示例：

```bash
GDR_NICS=mlx5_0,mlx5_2,mlx5_4,mlx5_6,mlx5_8,mlx5_10,mlx5_12,mlx5_14 \
./build/module/copy/copy -t all_host_to_all_device_gdr -s 16K -n 512 -i 128 -d 8
```
