# EP Demo 代码库分析

## 概述
此仓库实现了一个独立于 PyTorch 和 mooncake-transfer-engine 的 Expert Parallelism (EP) 初始化测试 Demo。它专注于使用 RDMA (InfiniBand/RoCE) 和 NVLink 进行 GPU-to-GPU 通信，以实现 Mixture-of-Experts 模型的高效专家并行。

## 核心组件

### 1. EpBuffer 类 (`include/ep_buffer.h` 和 `src/ep_buffer.cpp`)
这是中央组件，负责管理：
- GPU 内存分配（常规 CUDA malloc 和通过 cuMemCreate 的 Fabric 内存）
- InfiniBand 设备初始化和配置
- 用于 RDMA 通信的队列对 (QP) 创建和管理
- 进程间的内存注册和句柄交换
- IB 和 RoCE 协议的同步机制
- NVLink IPC 句柄交换以实现对等点到对等点的 GPU 通信

关键方法：
- `init_ibgda()`: 初始化 InfiniBand 设备，创建保护域，注册内存，并设置队列对
- `sync_ib()`/`sync_roce()`: 配置 QP 以进行与远程对等点的可靠连接模式通信
- `get_ipc_handle()`/`sync_nvlink_ipc_handles()`: 管理 NVLink 对等点到对等点内存访问
- 各种暴露内部状态的 getter 方法（MR 信息、GID、QPN、LID 等）

### 2. MLX5GDA 低级接口 (`include/ep_ibgda/mlx5gda.h` 和 `src/ep_ibgda/mlx5gda.cpp`)
提供对 Mellanox MLX5 硬件动词的直接访问，以实现高性能的 RDMA 操作：
- 队列对 (QP) 创建和管理
- 完成队列 (CQ) 创建
- QP 状态转换（RST→INIT→RTR→RTS）
- 工作队列元素 (WQE) 准备用于 RDMA 操作

### 3. EP 内核 (`src/ep_kernel.cu`)
包含核心专家并行计算内核：
- `dispatch()`: 根据路由决策将令牌嵌入发送到适当的专家 GPU
- `combine()`: 将专家输出聚合并返回到请求的 GPU

这些内核实现：
- GPU 之间的基于 RDMA 的数据传输
- 用于同步的原子操作
- 可用时的 NVLink 回退
- 用于高效 GPU 利用的合作组
- FP8/BF16 精度处理
- 延迟优化的通信模式

## 数据流

1. **初始化**：
    - 进程创建 EpBuffer 实例，参数为 rank、world size、缓冲区大小和 IB 设备名称
    - EpBuffer 分配 GPU 内存（Fabric 或常规）
    - 初始化 InfiniBand 上下文、保护域、内存区域
    - 为并行通信创建多个队列对
    - 通过外部机制（此代码中未显示）交换本地 QPN/LID 与对等点
    
    ### 详细初始化过程：
    
    **构造函数阶段**：
    1. 参数处理：保存 rank、num_ranks、缓冲区大小和设备名称
    2. 计算 USE_QP_COUNT 以在各 rank 之间均衡分配 QP
    3. 获取当前 CUDA 设备 ID 和时钟频率
    4. 内存分配策略选择：
       - 通过 supportFabricMem() 检查 Fabric 内存支持
       - 如果支持：使用 NVIDIA Fabric 内存（GPUDirect RDMA）分配，具有适当的访问权限
       - 否则：使用常规 cudaMalloc
    5. 分配辅助缓冲区：
       - 远程地址（raddrs）：每个 rank 的 uint64_t 数组
       - 远程密钥（rkeys）：每个 rank 的 uint32_t 数组
       - QP 设备上下文（qp_devctxs）：每个 QP 的 mlx5gda_qp_devctx 数组
       - NVLink 可用性跟踪（nvlink_available）：int32_t 数组
       - IPC 句柄主机/设备数组（ipc_peer_ptrs_host, ipc_peer_ptrs）
    6. 创建非阻塞 CUDA 流用于通信操作
    7. 通过 init_ibgda() 初始化 IBGDA
    8. 分配并清零工作空间缓冲区用于内核同步
    
    **IBGDA 初始化 (init_ibgda)**：
    1. 发现 InfiniBand 设备并按 device_name 匹配
    2. 打开 IB 设备上下文并查询端口属性（端口 1）
    3. 寻找最佳 GID 索引（优先选择 IB 或 RoCE v2 IPv4 映射 GID）
    4. 查询选中 GID 的完整值
    5. 分配保护域并转换为 MLX5 PD 对象
    6. 注册内存区域，授予本地写、远程读/写/原子访问权限
    7. 分配并注册控制缓冲区作为 UMEM
    8. 创建用于控制缓冲区管理的内存堆
    9. 对每个 QP（共 USE_QP_COUNT 个）：
       - 创建可靠连接 QP
       - 根据端口链路层判断是否为 RoCE
       - 将 QP 状态转换：RESET → INIT
       - 同步 CUDA 流确保操作完成
       - 构建 QP 设备上下文（包含 QPN、WQE 掩码、指向控制缓冲区的指针等）
       - 将 QP 设备上下文复制到设备内存 qp_devctxs
       - 将 QP 添加到 qps 向量中进行跟踪

2. **通信设置**：
   - 从所有进程收集远程地址、密钥、QPN 和 LID
   - 对于 IB：调用 `sync_ib()` 为每个对等点配置 QP
   - 对于 RoCE：调用 `sync_roce()` 并附加额外的 GID 信息
   - 在有利时交换 NVLink IPC 句柄以实现对等点到对等点的 GPU 访问

3. **专家并行计算**：
   - 调度内核使用 RDMA 写入将令牌路由到专家 GPU
   - 使用原子操作在发送方/接收方之间进行协调
   - 合并内核从专家 GPU 聚合结果
   - 实现双缓冲以实现计算和通信的重叠

## 关键 API 及其目的

### InfiniBand/RDMA API：
- `ibgda_disabled()`: 指示 IBGDA 初始化是否失败
- `is_roce()`: 如果使用 RoCE 协议则返回 true
- `use_fast_path()`: 如果 IBGDA 正常工作或 NVLink IPC 完全启用则返回 true
- `get_mr_info()`: 返回用于远程访问的内存区域地址和密钥
- `get_gid()`: 返回用于 RoCE 的子网前缀和接口 ID
- `get_local_qpns()`/`get_local_lids()`: 返回所有 QP 的 QP 号和 LID
- `get_ipc_handle()`: 返回用于 NVLink 对等点访问的 CUDA IPC 句柄
- `sync_ib()`/`sync_roce()`: 为与特定对等点通信配置 QP
- `sync_nvlink_ipc_handles()`: 建立 NVLink 对等点到对等点访问

### 内核 API：
- `dispatch()`: 主专家调度内核，将令牌发送到专家 GPU
- `combine()`: 主专家合并内核，聚合专家输出

## 技术细节

### 内存管理：
- 支持常规 CUDA malloc 和 NVIDIA Fabric 内存（GPUDirect RDMA）
- Fabric 内存实现无需 CPU 参与的直接 GPU-to-GPU RDMA
- 实现对等点 GPU 访问的适当访问权限

### 同步：
- 使用系统范围原子操作和互斥锁进行跨 GPU 协调
- 实现门铃寄存器以实现高效的 QP 信号传递
- 使用完成队列跟踪操作完成情况
- 实现故障容忍的超时机制

### 传输协议：
- 支持 InfiniBand 和 RoCE v2
- 自动检测并选择适当的 GID 类型
- 为每种协议处理不同的地址头格式

### 性能优化：
- 每个 rank 多个队列对以实现并行性
- 批量操作以减少开销
- 合作组以实现高效的 GPU 利用
- 双缓冲以重叠通信和计算
- NVLink 回退用于同节点通信
- FP8/BF16 精度支持，具有自动缩放

## 构建和测试

该仓库包括：
- `build.sh`: 编译脚本
- `ut.sh`: 带有各种模式的测试运行器
- 测试文件: `test_init.cpp`, `test_suite.cpp`, `test_multi.cpp`
- CMake 配置

测试验证：
1. 基础初始化和 IBGDA 状态
2. 内存区域信息
3. GID 信息
4. QPN/LID 查询
5. IPC 句柄交换
6. IB/RoCE 同步
7. NVLink IPC 句柄交换
8. 多进程专家并行操作

此实现为大型语言模型中的专家并行提供了高性能基础，利用 RDMA 和 NVLink 实现低延迟的 GPU-to-GPU 通信。