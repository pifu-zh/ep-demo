# EP Demo

独立于 PyTorch 和 mooncake-transfer-engine 的 EP (Expert Parallelism) 初始化测试 Demo。

## 依赖

- CUDA 12.x
- RDMA 设备 (InfiniBand/RoCE)
- libibverbs, libmlx5
- glog
- gflags

## 编译

```bash
./build.sh
```

## 运行

### 简单初始化测试

```bash
./ut.sh [rank] [num_ranks] [ib_device]

# 示例
./ut.sh 0 1 mlx5_0
```

### 完整测试套件

```bash
./ut.sh 0 1 test_suite
```

### 环境变量

| 变量 | 说明 | 默认值 |
|------|------|--------|
| `MC_IBGDA_DEVICE` | IB 设备名称 | `mlx5_0` |

### 参数说明

- `rank`: 当前进程 rank
- `num_ranks`: 总进程数
- `ib_device`: IB 设备名 (如 `mlx5_0`, `mlx5_1`)

可通过 `ibdev_to_netdev` 或 `ls /sys/class/infiniband/` 查看可用设备。

## 测试用例

| # | 测试内容 | 验证 API |
|---|----------|----------|
| 1 | 基础初始化 | `ibgda_disabled()`, `is_roce()`, `use_fast_path()` |
| 2 | 内存区域信息 | `get_mr_info()` |
| 3 | GID 信息 | `get_gid()` |
| 4 | QPN/LID 查询 | `get_local_qpns()`, `get_local_lids()` |
| 5 | IPC Handle | `get_ipc_handle()` |
| 6 | InfiniBand 同步 | `sync_ib()` |
| 7 | RoCE 同步 | `sync_roce()` |
| 8 | QP 重建 | `update_local_qpns()` |
| 9 | Buffer 大小计算 | `get_ep_buffer_size_hint()` |
| 10 | NVLink IPC | `sync_nvlink_ipc_handles()` |
| 11 | Fallback 模式 | IBGDA 禁用时行为 |
| 12 | 压力测试 | 重复操作 |

## 项目结构

```
ep-demo/
├── CMakeLists.txt
├── build.sh              # 编译脚本
├── ut.sh                 # 运行脚本
├── include/
│   ├── ep_buffer.h
│   ├── ep_configs.cuh
│   ├── ep_exception.cuh
│   └── ep_ibgda/        # IBGDA 头文件
├── src/
│   ├── ep_buffer.cpp
│   └── ep_ibgda/mlx5gda.cpp
└── test/
    ├── test_init.cpp     # 简单初始化测试
    └── test_suite.cpp    # 完整测试套件
```

## 示例输出

```
=== EP Demo Test Suite ===
Rank: 0/1
Device: mlx5_0
Buffer size: 256 MiB

============================================================
Test 1: Basic Initialization
============================================================
IBGDA disabled: no
Is RoCE: no
Use fast path: yes
PASSED!
...
============================================================
ALL TESTS PASSED!
```
# ep-demo
