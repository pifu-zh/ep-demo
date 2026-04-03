# EP Demo Repository Analysis

## Overview
This repository implements an Expert Parallelism (EP) initialization test demo that operates independently of PyTorch and mooncake-transfer-engine. It focuses on GPU-to-GPU communication using RDMA (InfiniBand/RoCE) and NVLink for efficient expert parallelism in Mixture-of-Experts models.

## Core Components

### 1. EpBuffer Class (`include/ep_buffer.h` and `src/ep_buffer.cpp`)
This is the central component that manages:
- GPU memory allocation (both regular CUDA malloc and fabric memory via cuMemCreate)
- InfiniBand device initialization and configuration
- Queue Pair (QP) creation and management for RDMA communication
- Memory registration and handle exchange between processes
- Synchronization mechanisms for both IB and RoCE protocols
- NVLink IPC handle exchange for peer-to-peer GPU communication

Key methods:
- `init_ibgda()`: Initializes InfiniBand devices, creates protection domains, registers memory, and sets up Queue Pairs
- `sync_ib()`/`sync_roce()`: Configures QPs for reliable connected mode communication with remote peers
- `get_ipc_handle()`/`sync_nvlink_ipc_handles()`: Manages NVLink peer-to-peer memory access
- Various getter methods exposing internal state (MR info, GID, QPNs, LIDs, etc.)

### 2. MLX5GDA Low-Level Interface (`include/ep_ibgda/mlx5gda.h` and `src/ep_ibgda/mlx5gda.cpp`)
Provides direct access to Mellanox MLX5 hardware verbs for high-performance RDMA operations:
- Queue Pair (QP) creation and management
- Completion Queue (CQ) creation
- QP state transitions (RST→INIT→RTR→RTS)
- Work Queue Element (WQE) preparation for RDMA operations

### 3. EP Kernels (`src/ep_kernel.cu`)
Contains the core expert parallelism computation kernels:
- `dispatch()`: Sends token embeddings to appropriate expert GPUs based on routing decisions
- `combine()`: Aggregates expert outputs and returns them to requesting GPUs

These kernels implement:
- RDMA-based data transfer between GPUs
- Atomic operations for synchronization
- NVLink fallback when available
- Cooperative groups for efficient GPU utilization
- FP8/BF16 precision handling
- Latency-optimized communication patterns

## Data Flow

1. **Initialization**:
    - Process creates EpBuffer instance with rank, world size, buffer size, and IB device name
    - EpBuffer allocates GPU memory (fabric or regular)
    - Initializes InfiniBand context, protection domain, memory region
    - Creates multiple Queue Pairs for parallel communication
    - Exchanges local QPNs/LIDs with peers via external mechanism (not shown in this code)
    
    ### Detailed Initialization Process:
    
    **Constructor Phase**:
    1. Parameter handling: Store rank, num_ranks, buffer size, and device name
    2. Calculate USE_QP_COUNT for balanced QP distribution across ranks
    3. Get current CUDA device ID and clock rate
    4. Memory allocation strategy selection:
       - Check Fabric memory support via supportFabricMem()
       - If supported: Allocate using NVIDIA Fabric memory (GPUDirect RDMA) with proper access permissions
       - Otherwise: Use regular cudaMalloc
    5. Allocate auxiliary buffers:
       - Remote addresses (raddrs): uint64_t array for each rank
       - Remote keys (rkeys): uint32_t array for each rank
       - QP device contexts (qp_devctxs): mlx5gda_qp_devctx array for each QP
       - NVLink availability tracking (nvlink_available): int32_t array
       - IPC handle host/device arrays (ipc_peer_ptrs_host, ipc_peer_ptrs)
    6. Create non-blocking CUDA stream for communication operations
    7. Initialize IBGDA via init_ibgda()
    8. Allocate and zero workspace buffer for kernel synchronization
    
    **IBGDA Initialization (init_ibgda)**:
    1. Discover InfiniBand devices and match by device_name
    2. Open IB device context and query port attributes (port 1)
    3. Find best GID index (preferring IB or RoCE v2 IPv4-mapped GIDs)
    4. Query the selected GID value
    5. Allocate protection domain and convert to MLX5 PD object
    6. Register memory region with local write, remote read/write/atomic access
    7. Allocate and register control buffer as UMEM
    8. Create memory heap for control buffer management
    9. For each QP (USE_QP_COUNT total):
       - Create reliable connection QP
       - Determine if RoCE based on port link layer
       - Transition QP state: RESET → INIT
       - Synchronize CUDA stream
       - Build QP device context (QPN, WQE mask, pointers to CQ/WQ/DBF/UAR)
       - Copy QP device context to device memory
       - Track QP in qps vector

2. **Communication Setup**:
   - Remote addresses, keys, QPNs, and LIDs are collected from all processes
   - For IB: Calls `sync_ib()` to configure QPs for each peer
   - For RoCE: Calls `sync_roce()` with additional GID information
   - Exchanges NVLink IPC handles for peer-to-peer GPU access when beneficial

3. **Expert Parallelism Computation**:
   - Dispatch kernel routes tokens to expert GPUs using RDMA writes
   - Uses atomic operations for coordination between sender/receiver
   - Combine kernel aggregates results from expert GPUs
   - Implements double-buffering for overlapping computation and communication

## Key APIs and Their Purposes

### InfiniBand/RDMA APIs:
- `ibgda_disabled()`: Indicates if IBGDA initialization failed
- `is_roce()`: Returns true if using RoCE protocol
- `use_fast_path()`: Returns true if either IBGDA is working or NVLink IPC is fully enabled
- `get_mr_info()`: Returns memory region address and key for remote access
- `get_gid()`: Returns subnet prefix and interface ID for RoCE
- `get_local_qpns()`/`get_local_lids()`: Returns QP numbers and LIDs for all QPs
- `get_ipc_handle()`: Returns CUDA IPC handle for NVLink peer access
- `sync_ib()`/`sync_roce()`: Configures QPs for communication with specific peers
- `sync_nvlink_ipc_handles()`: Establishes NVLink peer-to-peer access

### Kernel APIs:
- `dispatch()`: Main expert dispatch kernel that sends tokens to expert GPUs
- `combine()`: Main expert combine kernel that aggregates expert outputs

## Technical Details

### Memory Management:
- Supports both regular CUDA malloc and NVIDIA Fabric memory (GPUDirect RDMA)
- Fabric memory enables direct GPU-to-GPU RDMA without CPU involvement
- Implements proper access permissions for peer GPU access

### Synchronization:
- Uses system-scope atomics and mutexes for cross-GPU coordination
- Implements doorbell registers for efficient QP signaling
- Uses completion queues to track operation completion
- Implements timeout mechanisms for fault tolerance

### Transport Protocols:
- Supports both InfiniBand and RoCE v2
- Automatically detects and selects appropriate GID type
- Handles different address header formats for each protocol

### Performance Optimizations:
- Multiple Queue Pairs per rank for parallelism
- Batched operations to reduce overhead
- Cooperative groups for efficient GPU utilization
- Double-buffering to overlap communication and computation
- NVLink fallback for same-node communication
- FP8/BF16 precision support with automatic scaling

## Build and Test

The repository includes:
- `build.sh`: Compilation script
- `ut.sh`: Test runner with various modes
- Test files: `test_init.cpp`, `test_suite.cpp`, `test_multi.cpp`
- CMake configuration

The tests validate:
1. Basic initialization and IBGDA status
2. Memory region information
3. GID information
4. QPN/LID queries
5. IPC handle exchange
6. IB/RoCE synchronization
7. NVLink IPC handle exchange
8. Multi-process expert parallelism operation

This implementation provides a high-performance foundation for expert parallelism in large language models, leveraging RDMA and NVLink for low-latency GPU-to-GPU communication.