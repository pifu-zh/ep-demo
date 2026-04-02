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

### 多进程测试

```bash
# Rank 0 (server)
./build/test_multi 0 2 mlx5_0 127.0.0.1

# Rank 1 (client, 另一个终端)
./build/test_multi 1 2 mlx5_0 127.0.0.1 127.0.0.1
```

### 环境变量

| 变量 | 说明 | 默认值 |
|------|------|--------|
| `MC_IBGDA_DEVICE` | IB 设备名称 | `mlx5_0` |

### 参数说明

- `rank`: 当前进程 rank
- `num_ranks`: 总进程数
- `ib_device`: IB 设备名 (如 `mlx5_0`, `mlx5_1`)
- `my_ip`: 本机 IP 地址
- `peer_ips`: 对端 IP 地址列表 (供 rank > 0 使用)

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
| 8 | NVLink IPC | `sync_nvlink_ipc_handles()` |

## 项目结构

```
ep-demo/
├── CMakeLists.txt
├── build.sh              # 编译脚本
├── ut.sh                 # 运行脚本
├── README.md
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
    ├── test_suite.cpp    # 完整测试套件
    └── test_multi.cpp   # 多进程测试
```

## 示例输出

```
=== EP Multi-Process Test ===
Rank: 0/2
Device: mlx5_0
My IP: 127.0.0.1
Buffer size: 256 MiB

============================================================
Test 1: Basic Initialization [Rank 0]
============================================================
IBGDA disabled: no
Is RoCE: no
Use fast path: yes
PASSED!
...
============================================================
ALL TESTS PASSED [Rank 0]!
```
