#ifndef EP_BUFFER_H
#define EP_BUFFER_H

#include <cuda.h>
#include <cuda_bf16.h>
#include <cuda_runtime.h>
#include <ep_configs.cuh>
#include <ep_exception.cuh>
#include <ep_ibgda/memheap.h>
#include <ep_ibgda/mlx5gda.h>
#include <glog/logging.h>
#include <netinet/in.h>
#include <string>
#include <tuple>
#include <vector>

namespace ep {

struct BufferLayout {
	int *rdma_send_signal_buffer;
	int *rdma_recv_signal_buffer;
	void *rdma_send_data_buffer;
	void *rdma_recv_data_buffer;
};

struct BufferPair {
	size_t total_bytes = 0;
	BufferLayout buffers[2];

	template <typename out_ptr_t = void *, typename count_ptr_t = uint8_t *,
			  typename in_ptr_t = void *>
	static out_ptr_t advance(const in_ptr_t &ptr, size_t count) {
		return reinterpret_cast<out_ptr_t>(reinterpret_cast<count_ptr_t>(ptr) +
										   count);
	}

	BufferPair(void *rdma_buffer, int num_max_dispatch_tokens_per_rank,
			   int hidden, int num_ranks, int num_experts) {
		size_t signaling_buffer_bytes = num_experts * sizeof(int);
		size_t send_recv_buffer_bytes =
			num_experts * num_max_dispatch_tokens_per_rank *
			(2 * sizeof(int4) + hidden * sizeof(nv_bfloat16));
		for (int i = 0; i < 2; ++i) {
			size_t rdma_base_offset = total_bytes +
									  2 * i * signaling_buffer_bytes +
									  2 * i * send_recv_buffer_bytes;
			buffers[i] = {
				advance<int *>(rdma_buffer, rdma_base_offset),
				advance<int *>(rdma_buffer,
							   rdma_base_offset + signaling_buffer_bytes),
				advance<int *>(rdma_buffer,
							   rdma_base_offset + 2 * signaling_buffer_bytes),
				advance<int *>(rdma_buffer, rdma_base_offset +
												2 * signaling_buffer_bytes +
												send_recv_buffer_bytes),
			};
		}
		total_bytes += 4 * signaling_buffer_bytes + 4 * send_recv_buffer_bytes;
	}
};

struct EpBuffer {
  private:
	int device_id;
	int rank, num_ranks;
	int clock_rate_khz;
	int buffer_idx{};
	int64_t num_ep_buffer_bytes;
	void *gdr_buffer = nullptr;

	static constexpr size_t CTRL_BUF_SIZE = 1024ULL * 1024 * 1024;
	void *ctrl_buf = nullptr;
	ibv_mr *mr = nullptr;
	std::vector<mlx5gda_qp *> qps;
	ibv_gid gid;
	void *raddrs = nullptr;
	void *rkeys = nullptr;
	void *qp_devctxs = nullptr;
	std::string device_name;
	bool is_roce_ = false;
	bool ibgda_disabled_ = false;
	int gid_index_ = -1;
	int USE_QP_COUNT = MAX_QP_COUNT;

	mlx5dv_devx_umem *ctrl_buf_umem;
	ibv_pd *pd;
	mlx5dv_pd mpd;
	memheap *ctrl_buf_heap;

	bool use_fabric_mem_ = false;
	CUmemGenericAllocationHandle fabric_mem_handle_{};
	size_t fabric_alloc_size_ = 0;

	int32_t *nvlink_available = nullptr;
	void **ipc_peer_ptrs_host = nullptr;
	void **ipc_peer_ptrs = nullptr;
	bool p2p_ipc_all_enabled_ = false;

	cudaStream_t comm_stream = nullptr;

	void *workspace = nullptr;

	bool supportFabricMem();
	int findBestGidIndex(ibv_context *ctx, uint8_t port,
						 ibv_port_attr &port_attr);
	bool ipv6_addr_v4mapped(const struct in6_addr *a);

  public:
	EpBuffer(int rank, int num_ranks, int64_t num_ep_buffer_bytes,
			 std::string device_name);

	~EpBuffer() noexcept(false);

	int init_ibgda();

	bool ibgda_disabled() { return ibgda_disabled_; }

	bool is_roce() { return is_roce_; }

	bool use_fast_path() {
		if (!ibgda_disabled_) {
			return true;
		}
		if (!p2p_ipc_all_enabled_) {
			LOG(WARNING) << "Failed to initialize IBGDA. "
						 << "Using fallback implementation. "
						 << "Performance will be degraded.";
		}
		return p2p_ipc_all_enabled_;
	}

	void update_local_qpns();

	void sync_ib(const std::vector<int64_t> &remote_addrs,
				 const std::vector<int32_t> &remote_keys,
				 const std::vector<int32_t> &remote_qpns,
				 const std::vector<int32_t> &remote_lids,
				 const std::vector<int> &active_ranks_mask);

	void sync_roce(const std::vector<int64_t> &remote_addrs,
				   const std::vector<int32_t> &remote_keys,
				   const std::vector<int32_t> &remote_qpns,
				   const std::vector<int64_t> &subnet_prefixes,
				   const std::vector<int64_t> &interface_ids,
				   const std::vector<int> &active_ranks_mask);

	std::tuple<int64_t, int32_t> get_mr_info() {
		return {(int64_t)mr->addr, (int32_t)mr->rkey};
	}

	std::tuple<int64_t, int64_t> get_gid() {
		return {(int64_t)gid.global.subnet_prefix,
				(int64_t)gid.global.interface_id};
	}

	std::vector<int32_t> get_local_qpns() {
		std::vector<int32_t> local_qpns;
		for (int i = 0; i < USE_QP_COUNT; ++i) {
			local_qpns.push_back((int32_t)qps[i]->qpn);
		}
		return local_qpns;
	}

	std::vector<int32_t> get_local_lids() {
		std::vector<int32_t> local_lids;
		for (int i = 0; i < USE_QP_COUNT; ++i) {
			local_lids.push_back((int32_t)qps[i]->port_attr.lid);
		}
		return local_lids;
	}

	std::vector<int32_t> get_ipc_handle();
	void sync_nvlink_ipc_handles(
		const std::vector<std::vector<int32_t>> &remote_handles,
		const std::vector<int> &active_ranks_mask);

	void *get_gdr_buffer() { return gdr_buffer; }
	void *get_workspace() { return workspace; }
	int get_buffer_idx() { return buffer_idx; }
	void *get_raddrs() { return raddrs; }
	void *get_rkeys() { return rkeys; }
	void *get_qp_devctxs() { return qp_devctxs; }
	int32_t *get_nvlink_available() { return nvlink_available; }
	void **get_ipc_peer_ptrs() { return ipc_peer_ptrs; }
	cudaStream_t get_comm_stream() { return comm_stream; }
	int get_num_ep_buffer_bytes() { return num_ep_buffer_bytes; }

	BufferPair get_buffer_pair(int num_max_dispatch_tokens_per_rank,
							   int hidden_size, int num_experts_count) {
		return BufferPair(gdr_buffer, num_max_dispatch_tokens_per_rank,
						  hidden_size, num_ranks, num_experts_count);
	}
};

inline size_t get_ep_buffer_size_hint(int num_max_dispatch_tokens_per_rank,
									  int hidden, int num_ranks,
									  int num_experts) {
	return BufferPair(nullptr, num_max_dispatch_tokens_per_rank, hidden,
					  num_ranks, num_experts)
		.total_bytes;
}

} // namespace ep

#endif
