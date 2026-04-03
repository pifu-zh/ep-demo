#include <arpa/inet.h>
#include <ep_buffer.h>

namespace ep {

bool EpBuffer::supportFabricMem() {
	const char *nvlink_ipc = getenv("MC_USE_NVLINK_IPC");

	bool fabric_enabled = nvlink_ipc && strcmp(nvlink_ipc, "0") == 0;
	if (!fabric_enabled)
		return false;

	int num_devices = 0;
	cudaError_t err = cudaGetDeviceCount(&num_devices);
	if (err != cudaSuccess || num_devices == 0)
		return false;

	for (int dev = 0; dev < num_devices; ++dev) {
		int supported = 0;
		cuDeviceGetAttribute(
			&supported, CU_DEVICE_ATTRIBUTE_HANDLE_TYPE_FABRIC_SUPPORTED, dev);
		if (!supported)
			return false;
	}
	return true;
}

bool EpBuffer::ipv6_addr_v4mapped(const struct in6_addr *a) {
	return ((a->s6_addr32[0] | a->s6_addr32[1]) == 0 &&
			a->s6_addr32[2] == htonl(0x0000ffff));
}

int EpBuffer::findBestGidIndex(ibv_context *ctx, uint8_t port,
							   ibv_port_attr &port_attr) {
	for (int i = 0; i < port_attr.gid_tbl_len; i++) {
		ibv_gid_entry gid_entry;
		int ret = ibv_query_gid_ex(ctx, port, i, &gid_entry, 0);
		if (ret) {
			continue;
		}

		bool is_v4mapped = ipv6_addr_v4mapped(
			reinterpret_cast<const struct in6_addr *>(gid_entry.gid.raw));

		if ((is_v4mapped && gid_entry.gid_type == IBV_GID_TYPE_ROCE_V2) ||
			gid_entry.gid_type == IBV_GID_TYPE_IB) {
			return i;
		}
	}
	return -1;
}

EpBuffer::EpBuffer(int rank, int num_ranks, int64_t num_ep_buffer_bytes,
				   std::string device_name)
	: rank(rank), num_ranks(num_ranks),
	  num_ep_buffer_bytes(num_ep_buffer_bytes),
	  device_name(std::move(device_name)) {
	USE_QP_COUNT = MAX_QP_COUNT / num_ranks * num_ranks;
	CUDA_CHECK(cudaGetDevice(&device_id));
	CUDA_CHECK(cudaDeviceGetAttribute(&clock_rate_khz, cudaDevAttrClockRate,
									  device_id));

	use_fabric_mem_ = supportFabricMem();
	if (use_fabric_mem_) {
		CUdevice cu_dev;
		CUresult res = cuDeviceGet(&cu_dev, device_id);
		if (res != CUDA_SUCCESS) {
			LOG(ERROR) << "[EP] cuDeviceGet failed: " << res;
			throw std::runtime_error("cuDeviceGet failed");
		}

		CUmemAllocationProp prop = {};
		prop.type = CU_MEM_ALLOCATION_TYPE_PINNED;
		prop.location.type = CU_MEM_LOCATION_TYPE_DEVICE;
		prop.location.id = cu_dev;
		prop.requestedHandleTypes = CU_MEM_HANDLE_TYPE_FABRIC;

		int rdma_flag = 0;
		cuDeviceGetAttribute(
			&rdma_flag,
			CU_DEVICE_ATTRIBUTE_GPU_DIRECT_RDMA_WITH_CUDA_VMM_SUPPORTED,
			cu_dev);
		if (rdma_flag)
			prop.allocFlags.gpuDirectRDMACapable = 1;

		size_t granularity = 0;
		res = cuMemGetAllocationGranularity(&granularity, &prop,
											CU_MEM_ALLOC_GRANULARITY_MINIMUM);
		if (res != CUDA_SUCCESS) {
			LOG(ERROR) << "[EP] cuMemGetAllocationGranularity failed: " << res;
			throw std::runtime_error("cuMemGetAllocationGranularity failed");
		}

		fabric_alloc_size_ =
			(num_ep_buffer_bytes + granularity - 1) & ~(granularity - 1);
		if (fabric_alloc_size_ == 0)
			fabric_alloc_size_ = granularity;

		res = cuMemCreate(&fabric_mem_handle_, fabric_alloc_size_, &prop, 0);
		if (res != CUDA_SUCCESS) {
			LOG(ERROR) << "[EP] cuMemCreate(FABRIC) failed: " << res;
			throw std::runtime_error("cuMemCreate failed");
		}

		CUdeviceptr dptr = 0;
		res = cuMemAddressReserve(&dptr, fabric_alloc_size_, granularity, 0, 0);
		if (res != CUDA_SUCCESS) {
			cuMemRelease(fabric_mem_handle_);
			LOG(ERROR) << "[EP] cuMemAddressReserve failed: " << res;
			throw std::runtime_error("cuMemAddressReserve failed");
		}

		res = cuMemMap(dptr, fabric_alloc_size_, 0, fabric_mem_handle_, 0);
		if (res != CUDA_SUCCESS) {
			cuMemAddressFree(dptr, fabric_alloc_size_);
			cuMemRelease(fabric_mem_handle_);
			LOG(ERROR) << "[EP] cuMemMap failed: " << res;
			throw std::runtime_error("cuMemMap failed");
		}

		int device_count = 0;
		cudaGetDeviceCount(&device_count);
		std::vector<CUmemAccessDesc> access(device_count);
		for (int i = 0; i < device_count; ++i) {
			access[i].location.type = CU_MEM_LOCATION_TYPE_DEVICE;
			access[i].location.id = i;
			access[i].flags = CU_MEM_ACCESS_FLAGS_PROT_READWRITE;
		}
		res = cuMemSetAccess(dptr, fabric_alloc_size_, access.data(),
							 device_count);
		if (res != CUDA_SUCCESS) {
			cuMemUnmap(dptr, fabric_alloc_size_);
			cuMemAddressFree(dptr, fabric_alloc_size_);
			cuMemRelease(fabric_mem_handle_);
			LOG(ERROR) << "[EP] cuMemSetAccess failed: " << res;
			throw std::runtime_error("cuMemSetAccess failed");
		}

		gdr_buffer = reinterpret_cast<void *>(dptr);
		LOG(INFO) << "[EP] Allocated " << fabric_alloc_size_
				  << " bytes with fabric handle on GPU " << device_id;
	} else {
		CUDA_CHECK(cudaMalloc(&gdr_buffer, num_ep_buffer_bytes));
	}
	CUDA_CHECK(cudaMalloc(&raddrs, num_ranks * sizeof(uint64_t)));
	CUDA_CHECK(cudaMalloc(&rkeys, num_ranks * sizeof(uint32_t)));
	CUDA_CHECK(
		cudaMalloc(&qp_devctxs, USE_QP_COUNT * sizeof(mlx5gda_qp_devctx)));

	CUDA_CHECK(cudaMalloc(&nvlink_available, num_ranks * sizeof(int32_t)));
	CUDA_CHECK(cudaMemset(nvlink_available, 0, num_ranks * sizeof(int32_t)));
	CUDA_CHECK(cudaMallocHost(&ipc_peer_ptrs_host, num_ranks * sizeof(void *)));
	CUDA_CHECK(cudaMalloc(&ipc_peer_ptrs, num_ranks * sizeof(void *)));
	for (int i = 0; i < num_ranks; ++i) {
		ipc_peer_ptrs_host[i] = nullptr;
	}
	CUDA_CHECK(cudaMemset(ipc_peer_ptrs, 0, num_ranks * sizeof(void *)));

	CUDA_CHECK(cudaStreamCreateWithFlags(&comm_stream, cudaStreamNonBlocking));

	int ret = init_ibgda();
	if (ret != 0) {
		ibgda_disabled_ = true;
	}

	CUDA_CHECK(cudaMalloc(&workspace, NUM_WORKSPACE_BYTES));
	CUDA_CHECK(cudaMemsetAsync(workspace, 0, NUM_WORKSPACE_BYTES, comm_stream));
}

EpBuffer::~EpBuffer() noexcept(false) {
	if (use_fabric_mem_) {
		CUdeviceptr dptr = reinterpret_cast<CUdeviceptr>(gdr_buffer);
		cuMemUnmap(dptr, fabric_alloc_size_);
		cuMemAddressFree(dptr, fabric_alloc_size_);
		cuMemRelease(fabric_mem_handle_);
	} else {
		cudaFree(gdr_buffer);
	}
	cudaFree(raddrs);
	cudaFree(rkeys);
	cudaFree(qp_devctxs);
	if (nvlink_available)
		cudaFree(nvlink_available);
	if (ipc_peer_ptrs)
		cudaFree(ipc_peer_ptrs);
	if (ipc_peer_ptrs_host) {
		for (int i = 0; i < num_ranks; ++i) {
			if (ipc_peer_ptrs_host[i] != nullptr &&
				ipc_peer_ptrs_host[i] != gdr_buffer) {
				cudaIpcCloseMemHandle(ipc_peer_ptrs_host[i]);
			}
		}
		cudaFreeHost(ipc_peer_ptrs_host);
	}
	if (workspace)
		cudaFree(workspace);
	if (comm_stream)
		cudaStreamDestroy(comm_stream);
}

int EpBuffer::init_ibgda() {
	int num_devices;
	ibv_device **dev_list = ibv_get_device_list(&num_devices);
	int nic_id = -1;
	for (int i = 0; i < num_devices; ++i) {
		const char *name = ibv_get_device_name(dev_list[i]);
		if (name && device_name == name) {
			nic_id = i;
			break;
		}
	}
	if (nic_id == -1) {
		throw std::runtime_error("Device matching name '" + device_name +
								 "' not found.");
	}
	LOG(INFO) << "[EP] GPU " << device_id << " uses NIC " << nic_id
			  << " out of " << num_devices << " NIC(s)";
	ibv_context *ctx = ibv_open_device(dev_list[nic_id]);
	if (!ctx) {
		perror("Failed to open device");
		return -1;
	}

	ibv_port_attr port_attr;
	const uint8_t port_num = 1;
	if (ibv_query_port(ctx, port_num, &port_attr)) {
		perror("Failed to query port");
		return -1;
	}

	gid_index_ = findBestGidIndex(ctx, port_num, port_attr);
	if (gid_index_ < 0) {
		LOG(ERROR) << "[EP] Failed to find a suitable GID index on "
				   << device_name;
		return -1;
	}

	if (ibv_query_gid(ctx, port_num, gid_index_, &gid)) {
		perror("Failed to query gid");
		return -1;
	}
	ibv_free_device_list(dev_list);

	pd = ibv_alloc_pd(ctx);
	if (!pd) {
		perror("Failed to allocate protection domain");
		return -1;
	}
	mlx5dv_obj dv_obj = {};
	dv_obj.pd.in = pd;
	dv_obj.pd.out = &mpd;
	if (mlx5dv_init_obj(&dv_obj, MLX5DV_OBJ_PD)) {
		perror("Failed to initialize mlx5dv object");
	}
	mr = ibv_reg_mr(pd, gdr_buffer, num_ep_buffer_bytes,
					IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ |
						IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_ATOMIC);
	if (!mr) {
		perror("Failed to reg mr");
	}

	CUDA_CHECK(cudaMalloc(&ctrl_buf, CTRL_BUF_SIZE));
	ctrl_buf_umem = mlx5dv_devx_umem_reg(ctx, ctrl_buf, CTRL_BUF_SIZE,
										 IBV_ACCESS_LOCAL_WRITE);
	if (!ctrl_buf_umem) {
		perror("Failed to register control buffer as umem");
		fprintf(stderr,
				"If the error is `Bad address`, probably because your GPU "
				"does not support GPUDirect RDMA.\n");
		if (mr) {
			ibv_dereg_mr(mr);
			mr = nullptr;
		}
		return -1;
	}
	ctrl_buf_heap = memheap_create(CTRL_BUF_SIZE);
	if (!ctrl_buf_heap) {
		perror("Failed to create memory heap");
		return -1;
	}
	for (int i = 0; i < USE_QP_COUNT; ++i) {
		mlx5gda_qp *qp =
			mlx5gda_create_rc_qp(mpd, ctrl_buf, ctrl_buf_umem, ctrl_buf_heap,
								 pd, 16384, 1, comm_stream);
		if (!qp) {
			perror("Failed to create QP");
			return -1;
		}
		is_roce_ = qp->port_attr.link_layer == IBV_LINK_LAYER_ETHERNET;
		if (mlx5gda_modify_rc_qp_rst2init(qp, 0)) {
			perror("Failed to mlx5gda_modify_rc_qp_rst2init");
			return -1;
		}
		CUDA_CHECK(cudaStreamSynchronize(comm_stream));

		mlx5gda_qp_devctx qp_devctx = {
			.qpn = qp->qpn,
			.wqeid_mask = qp->num_wqebb - 1,
			.wq = (mlx5gda_wqebb *)(ctrl_buf + qp->wq_offset),
			.cq = (mlx5_cqe64 *)(ctrl_buf + qp->send_cq->cq_offset),
			.dbr = (mlx5gda_wq_dbr *)(ctrl_buf + qp->dbr_offset),
			.bf = (char *)qp->uar->reg_addr,
		};
		cudaMemcpy(qp_devctxs + i * sizeof(mlx5gda_qp_devctx), &qp_devctx,
				   sizeof(mlx5gda_qp_devctx), cudaMemcpyHostToDevice);
		qps.push_back(qp);
	}
	return 0;
}

void EpBuffer::update_local_qpns() {
	for (int i = 0; i < USE_QP_COUNT; ++i) {
		if (qps[i]) {
			mlx5gda_destroy_qp(ctrl_buf_heap, qps[i]);
			qps[i] = nullptr;
		}
	}

	for (int i = 0; i < USE_QP_COUNT; ++i) {
		mlx5gda_qp *qp =
			mlx5gda_create_rc_qp(mpd, ctrl_buf, ctrl_buf_umem, ctrl_buf_heap,
								 pd, 16384, 1, comm_stream);
		if (!qp) {
			perror("Failed to recreate QP");
			ibgda_disabled_ = true;
			return;
		}
		is_roce_ = qp->port_attr.link_layer == IBV_LINK_LAYER_ETHERNET;
		if (mlx5gda_modify_rc_qp_rst2init(qp, 0)) {
			perror("Failed to mlx5gda_modify_rc_qp_rst2init");
			ibgda_disabled_ = true;
			return;
		}
		CUDA_CHECK(cudaStreamSynchronize(comm_stream));

		mlx5gda_qp_devctx qp_devctx = {
			.qpn = qp->qpn,
			.wqeid_mask = qp->num_wqebb - 1,
			.wq = (mlx5gda_wqebb *)(ctrl_buf + qp->wq_offset),
			.cq = (mlx5_cqe64 *)(ctrl_buf + qp->send_cq->cq_offset),
			.dbr = (mlx5gda_wq_dbr *)(ctrl_buf + qp->dbr_offset),
			.bf = (char *)qp->uar->reg_addr,
		};
		cudaMemcpy(qp_devctxs + i * sizeof(mlx5gda_qp_devctx), &qp_devctx,
				   sizeof(mlx5gda_qp_devctx), cudaMemcpyHostToDevice);
		qps[i] = qp;
	}
}

void EpBuffer::sync_ib(const std::vector<int64_t> &remote_addrs,
					   const std::vector<int32_t> &remote_keys,
					   const std::vector<int32_t> &remote_qpns,
					   const std::vector<int32_t> &remote_lids,
					   const std::vector<int> &active_ranks_mask) {
	for (int i = 0; i < USE_QP_COUNT; ++i) {
		int peer_rank = i * num_ranks / USE_QP_COUNT;
		if (active_ranks_mask[peer_rank] == 0)
			continue;
		ibv_ah_attr ah_attr = {
			.dlid = (uint16_t)remote_lids[i],
			.port_num = 0,
		};
		if (mlx5gda_modify_rc_qp_init2rtr(
				qps[i], ah_attr, (uint32_t)remote_qpns[i], IBV_MTU_4096)) {
			perror("Failed to mlx5gda_modify_rc_qp_init2rtr");
			exit(1);
		}
		if (mlx5gda_modify_rc_qp_rtr2rts(qps[i])) {
			perror("Failed to mlx5gda_modify_rc_qp_rtr2rts");
			exit(1);
		}
	}
	for (int i = 0; i < num_ranks; ++i) {
		if (active_ranks_mask[i] == 0)
			continue;
		uint64_t raddr =
			i == rank ? (uint64_t)mr->addr : (uint64_t)remote_addrs[i];
		cudaMemcpy(raddrs + i * sizeof(uint64_t), &raddr, sizeof(uint64_t),
				   cudaMemcpyHostToDevice);
		uint32_t rkey = i == rank ? mr->lkey : (uint32_t)remote_keys[i];
		cudaMemcpy(rkeys + i * sizeof(uint32_t), &rkey, sizeof(uint32_t),
				   cudaMemcpyHostToDevice);
	}
}

void EpBuffer::sync_roce(const std::vector<int64_t> &remote_addrs,
						 const std::vector<int32_t> &remote_keys,
						 const std::vector<int32_t> &remote_qpns,
						 const std::vector<int64_t> &subnet_prefixes,
						 const std::vector<int64_t> &interface_ids,
						 const std::vector<int> &active_ranks_mask) {
	for (int i = 0; i < USE_QP_COUNT; ++i) {
		int peer_rank = i * num_ranks / USE_QP_COUNT;
		if (active_ranks_mask[peer_rank] == 0)
			continue;
		ibv_gid remote_gid{};
		remote_gid.global.subnet_prefix = subnet_prefixes[peer_rank];
		remote_gid.global.interface_id = interface_ids[peer_rank];
		ibv_ah_attr ah_attr = {};
		ah_attr.is_global = 1;
		ah_attr.grh.dgid = remote_gid;
		ah_attr.grh.sgid_index = gid_index_;
		ah_attr.grh.hop_limit = 1;
		ah_attr.port_num = 1;
		ah_attr.dlid = qps[i]->port_attr.lid | 0xC000;
		if (mlx5gda_modify_rc_qp_init2rtr(
				qps[i], ah_attr, (uint32_t)remote_qpns[i], IBV_MTU_4096)) {
			perror("Failed to mlx5gda_modify_rc_qp_init2rtr");
			exit(1);
		}
		if (mlx5gda_modify_rc_qp_rtr2rts(qps[i])) {
			perror("Failed to mlx5gda_modify_rc_qp_rtr2rts");
			exit(1);
		}
	}
	for (int i = 0; i < num_ranks; ++i) {
		if (active_ranks_mask[i] == 0)
			continue;
		uint64_t raddr =
			i == rank ? (uint64_t)mr->addr : (uint64_t)remote_addrs[i];
		cudaMemcpy(raddrs + i * sizeof(uint64_t), &raddr, sizeof(uint64_t),
				   cudaMemcpyHostToDevice);
		uint32_t rkey = i == rank ? mr->lkey : (uint32_t)remote_keys[i];
		cudaMemcpy(rkeys + i * sizeof(uint32_t), &rkey, sizeof(uint32_t),
				   cudaMemcpyHostToDevice);
	}
}

std::vector<int32_t> EpBuffer::get_ipc_handle() {
	if (use_fabric_mem_) {
		return {};
	}
	cudaIpcMemHandle_t handle;
	CUDA_CHECK(cudaIpcGetMemHandle(&handle, gdr_buffer));
	const size_t handle_size = sizeof(cudaIpcMemHandle_t);
	const size_t num_int32s =
		(handle_size + sizeof(int32_t) - 1) / sizeof(int32_t);
	std::vector<int32_t> handle_ints(num_int32s);
	memcpy(handle_ints.data(), &handle, handle_size);
	return handle_ints;
}

void EpBuffer::sync_nvlink_ipc_handles(
	const std::vector<std::vector<int32_t>> &remote_handles,
	const std::vector<int> &active_ranks_mask) {
	int device_count = 0;
	CUDA_CHECK(cudaGetDeviceCount(&device_count));

	std::vector<int32_t> nvlink_array(num_ranks, 0);
	nvlink_array[rank] = 1;

	if (use_fabric_mem_) {
		for (int i = 0; i < num_ranks; ++i) {
			if (active_ranks_mask[i] == 0)
				continue;
			nvlink_array[i] = 1;
			ipc_peer_ptrs_host[i] = (i == rank) ? gdr_buffer : nullptr;
		}
		p2p_ipc_all_enabled_ = true;
		LOG(INFO) << "[EP] Fabric memory enabled, skipping IPC handle exchange";
	} else {
		int node_id = rank / device_count;
		int group_start = node_id * device_count;
		int group_end = std::min(group_start + device_count, num_ranks);

		for (int dst_rank = group_start; dst_rank < group_end; ++dst_rank) {
			if (active_ranks_mask[dst_rank] == 0)
				continue;
			if (dst_rank == rank) {
				ipc_peer_ptrs_host[dst_rank] = gdr_buffer;
				continue;
			}

			int dst_device = dst_rank % device_count;
			int can_access_peer = 0;
			cudaError_t err = cudaDeviceCanAccessPeer(&can_access_peer,
													  device_id, dst_device);
			if (err == cudaSuccess && can_access_peer) {
				cudaError_t peer_err =
					cudaDeviceEnablePeerAccess(dst_device, 0);
				if (peer_err == cudaSuccess ||
					peer_err == cudaErrorPeerAccessAlreadyEnabled) {
					if (peer_err == cudaErrorPeerAccessAlreadyEnabled) {
						cudaGetLastError();
					}
					nvlink_array[dst_rank] = 1;

					if (dst_rank >= static_cast<int>(remote_handles.size())) {
						LOG(WARNING)
							<< "[EP] Rank " << rank
							<< " missing IPC handle for rank " << dst_rank;
						continue;
					}

					const size_t handle_size = sizeof(cudaIpcMemHandle_t);
					const size_t num_int32s =
						(handle_size + sizeof(int32_t) - 1) / sizeof(int32_t);
					const auto &handle_ints = remote_handles[dst_rank];
					if (handle_ints.size() < num_int32s) {
						LOG(WARNING)
							<< "[EP] Rank " << rank
							<< " invalid IPC handle size for rank " << dst_rank;
						continue;
					}

					cudaIpcMemHandle_t remote_handle;
					memcpy(&remote_handle, handle_ints.data(), handle_size);

					void *peer_ptr = nullptr;
					cudaError_t ipc_err =
						cudaIpcOpenMemHandle(&peer_ptr, remote_handle,
											 cudaIpcMemLazyEnablePeerAccess);
					if (ipc_err != cudaSuccess) {
						LOG(WARNING)
							<< "[EP] Rank " << rank
							<< " failed to open IPC handle for rank "
							<< dst_rank << ": " << cudaGetErrorString(ipc_err);
						nvlink_array[dst_rank] = 0;
					} else {
						ipc_peer_ptrs_host[dst_rank] = peer_ptr;
					}
				}
			}
		}

		p2p_ipc_all_enabled_ = true;
		for (int i = 0; i < num_ranks; ++i) {
			if (active_ranks_mask[i] == 0)
				continue;
			if (nvlink_array[i] == 0 || ipc_peer_ptrs_host[i] == nullptr) {
				p2p_ipc_all_enabled_ = false;
				break;
			}
		}
		if (p2p_ipc_all_enabled_ && num_ranks > 1) {
			int first_node_id = 0 / device_count;
			int last_node_id = (num_ranks - 1) / device_count;
			if (first_node_id != last_node_id) {
				p2p_ipc_all_enabled_ = false;
			}
		}
	}

	CUDA_CHECK(cudaMemcpy(nvlink_available, nvlink_array.data(),
						  num_ranks * sizeof(int32_t), cudaMemcpyHostToDevice));
	CUDA_CHECK(cudaMemcpy(ipc_peer_ptrs, ipc_peer_ptrs_host,
						  num_ranks * sizeof(void *), cudaMemcpyHostToDevice));
}

} // namespace ep
