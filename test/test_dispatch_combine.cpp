#include <arpa/inet.h>
#include <chrono>
#include <cmath>
#include <ep_buffer.h>
#include <ep_kernel.h>
#include <fcntl.h>
#include <iomanip>
#include <iostream>
#include <poll.h>
#include <random>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

using namespace ep;

constexpr int BASE_PORT = 12345;

int create_server_socket(int port) {
	int sock = socket(AF_INET, SOCK_STREAM, 0);
	if (sock < 0) {
		perror("socket");
		return -1;
	}

	int opt = 1;
	setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

	struct sockaddr_in addr = {};
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = INADDR_ANY;
	addr.sin_port = htons(port);

	if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		perror("bind");
		close(sock);
		return -1;
	}

	if (listen(sock, 10) < 0) {
		perror("listen");
		close(sock);
		return -1;
	}

	return sock;
}

int create_client_socket(const char *ip, int port) {
	int sock = socket(AF_INET, SOCK_STREAM, 0);
	if (sock < 0) {
		perror("socket");
		return -1;
	}

	struct sockaddr_in addr = {};
	addr.sin_family = AF_INET;
	addr.sin_port = htons(port);

	if (inet_pton(AF_INET, ip, &addr.sin_addr) <= 0) {
		perror("inet_pton");
		close(sock);
		return -1;
	}

	struct pollfd pfd = {};
	pfd.fd = sock;
	pfd.events = POLLOUT;

	if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		perror("connect");
		close(sock);
		return -1;
	}

	return sock;
}

bool send_all(int sock, const void *data, size_t len) {
	const char *ptr = (const char *)data;
	size_t sent = 0;
	while (sent < len) {
		ssize_t n = send(sock, ptr + sent, len - sent, 0);
		if (n <= 0)
			return false;
		sent += n;
	}
	return true;
}

bool recv_all(int sock, void *data, size_t len) {
	char *ptr = (char *)data;
	size_t received = 0;
	while (received < len) {
		ssize_t n = recv(sock, ptr + received, len - received, 0);
		if (n <= 0)
			return false;
		received += n;
	}
	return true;
}

template <typename T> bool send_vector(int sock, const std::vector<T> &vec) {
	uint32_t size = vec.size();
	if (!send_all(sock, &size, sizeof(size)))
		return false;
	if (size > 0 && !send_all(sock, vec.data(), size * sizeof(T)))
		return false;
	return true;
}

template <typename T> bool recv_vector(int sock, std::vector<T> &vec) {
	uint32_t size;
	if (!recv_all(sock, &size, sizeof(size)))
		return false;
	vec.resize(size);
	if (size > 0 && !recv_all(sock, vec.data(), size * sizeof(T)))
		return false;
	return true;
}

struct RankInfo {
	std::vector<int32_t> qpns;
	std::vector<int32_t> lids;
	int64_t mr_addr;
	int32_t mr_rkey;
	int64_t gid_subnet_prefix;
	int64_t gid_interface_id;
	std::vector<int32_t> ipc_handle;
	bool is_roce;
};

bool exchange_rank_info(int my_rank, int num_ranks,
						const std::vector<std::string> &peer_ips,
						RankInfo &my_info, std::vector<RankInfo> &all_info) {
	std::cerr << "[Rank " << my_rank << "] exchange_rank_info ENTERED at " << __LINE__ << std::endl;
	all_info.resize(num_ranks);

	if (my_rank == 0) {
		std::cerr << "[Rank 0] Creating server socket on port " << BASE_PORT << "..." << std::endl;
		int server_sock = create_server_socket(BASE_PORT);
		if (server_sock < 0) {
			std::cerr << "Failed to create server socket" << std::endl;
			return false;
		}
		std::cerr << "[Rank 0] Server socket created!" << std::endl;

		std::vector<int> client_socks;
		for (int i = 1; i < num_ranks; ++i) {
			struct sockaddr_in client_addr;
			socklen_t len = sizeof(client_addr);
			int client_sock =
				accept(server_sock, (struct sockaddr *)&client_addr, &len);
			if (client_sock < 0) {
				perror("accept");
				return false;
			}
			client_socks.push_back(client_sock);
			std::cout << "[Rank 0] Accepted connection from rank " << i
					  << std::endl;
		}

		all_info[0] = my_info;

for (int i = 1; i < num_ranks; ++i) {
			int sock = client_socks[i - 1];
			std::cerr << "[Rank 0] Receiving from rank " << i << "..." << std::endl;
			if (!recv_vector(sock, all_info[i].qpns)) return false;
			if (!recv_vector(sock, all_info[i].lids)) return false;
			if (!recv_all(sock, &all_info[i].mr_addr, sizeof(int64_t))) return false;
			if (!recv_all(sock, &all_info[i].mr_rkey, sizeof(int32_t))) return false;
			if (!recv_all(sock, &all_info[i].gid_subnet_prefix, sizeof(int64_t))) return false;
			if (!recv_all(sock, &all_info[i].gid_interface_id, sizeof(int64_t))) return false;
if (!recv_vector(sock, all_info[i].ipc_handle)) return false;
			if (!recv_all(sock, &all_info[i].is_roce, sizeof(bool))) return false;
			std::cerr << "[Rank 0] Received from rank " << i << "!" << std::endl;
		}

		std::cerr << "[Rank 0] Sending all_info back to rank 1..." << std::endl;
		for (int i = 1; i < num_ranks; ++i) {
			int sock = client_socks[i - 1];
			for (int j = 0; j < num_ranks; ++j) {
				std::cerr << "[Rank 0] Sending info to rank " << j << "..." << std::endl;
				if (!send_vector(sock, all_info[j].qpns)) return false;
				if (!send_vector(sock, all_info[j].lids)) return false;
				if (!send_all(sock, &all_info[j].mr_addr, sizeof(int64_t))) return false;
				if (!send_all(sock, &all_info[j].mr_rkey, sizeof(int32_t))) return false;
				if (!send_all(sock, &all_info[j].gid_subnet_prefix, sizeof(int64_t))) return false;
				if (!send_all(sock, &all_info[j].gid_interface_id, sizeof(int64_t))) return false;
				if (!send_vector(sock, all_info[j].ipc_handle)) return false;
				if (!send_all(sock, &all_info[j].is_roce, sizeof(bool))) return false;
			}
		}
		std::cerr << "[Rank 0] All info sent back!" << std::endl;

		for (int sock : client_socks)
			close(sock);
		close(server_sock);

} else {
		std::this_thread::sleep_for(std::chrono::milliseconds(500));
		std::cerr << "[Rank " << my_rank
				  << "] Creating client socket to " << peer_ips[0] << ":"
				  << BASE_PORT << "..." << std::endl;

		int client_sock = create_client_socket(peer_ips[0].c_str(), BASE_PORT);
		if (client_sock < 0) {
			std::cerr << "Failed to connect to rank 0" << std::endl;
			return false;
		}
		std::cerr << "[Rank " << my_rank << "] Connected to server!" << std::endl;
		if (client_sock < 0) {
			std::cerr << "Failed to connect to rank 0" << std::endl;
			return false;
		}

		std::cerr << "[Rank " << my_rank << "] Sending qpns..." << std::endl;
		if (!send_vector(client_sock, my_info.qpns))
			return false;
		std::cerr << "[Rank " << my_rank << "] Sending lids..." << std::endl;
		if (!send_vector(client_sock, my_info.lids))
			return false;
		std::cerr << "[Rank " << my_rank << "] Sending mr_addr..." << std::endl;
		if (!send_all(client_sock, &my_info.mr_addr, sizeof(int64_t)))
			return false;
		std::cerr << "[Rank " << my_rank << "] Sending mr_rkey..." << std::endl;
		if (!send_all(client_sock, &my_info.mr_rkey, sizeof(int32_t)))
			return false;
		std::cerr << "[Rank " << my_rank << "] Sending gid_subnet_prefix..." << std::endl;
		if (!send_all(client_sock, &my_info.gid_subnet_prefix, sizeof(int64_t)))
			return false;
		std::cerr << "[Rank " << my_rank << "] Sending gid_interface_id..." << std::endl;
		if (!send_all(client_sock, &my_info.gid_interface_id, sizeof(int64_t)))
			return false;
		std::cerr << "[Rank " << my_rank << "] Sending ipc_handle..." << std::endl;
		if (!send_vector(client_sock, my_info.ipc_handle))
			return false;
		std::cerr << "[Rank " << my_rank << "] Sending is_roce..." << std::endl;
		if (!send_all(client_sock, &my_info.is_roce, sizeof(bool)))
			return false;
		std::cerr << "[Rank " << my_rank << "] All info sent! Receiving response..." << std::endl;

		all_info.resize(num_ranks);

		for (int j = 0; j < num_ranks; ++j) {
			std::cerr << "[Rank " << my_rank << "] Receiving qpns from rank " << j << "..." << std::endl;
			if (!recv_vector(client_sock, all_info[j].qpns))
				return false;
			std::cerr << "[Rank " << my_rank << "] Receiving lids..." << std::endl;
			if (!recv_vector(client_sock, all_info[j].lids))
				return false;
			std::cerr << "[Rank " << my_rank << "] Receiving mr_addr..." << std::endl;
			if (!recv_all(client_sock, &all_info[j].mr_addr, sizeof(int64_t)))
				return false;
			std::cerr << "[Rank " << my_rank << "] Receiving mr_rkey..." << std::endl;
			if (!recv_all(client_sock, &all_info[j].mr_rkey, sizeof(int32_t)))
				return false;
			std::cerr << "[Rank " << my_rank << "] Receiving gid_subnet_prefix..." << std::endl;
			if (!recv_all(client_sock, &all_info[j].gid_subnet_prefix,
						  sizeof(int64_t)))
				return false;
			std::cerr << "[Rank " << my_rank << "] Receiving gid_interface_id..." << std::endl;
			if (!recv_all(client_sock, &all_info[j].gid_interface_id,
						  sizeof(int64_t)))
				return false;
			std::cerr << "[Rank " << my_rank << "] Receiving ipc_handle..." << std::endl;
			if (!recv_vector(client_sock, all_info[j].ipc_handle))
				return false;
			std::cerr << "[Rank " << my_rank << "] Receiving is_roce..." << std::endl;
			if (!recv_all(client_sock, &all_info[j].is_roce, sizeof(bool)))
				return false;
		}
		std::cerr << "[Rank " << my_rank << "] All received!" << std::endl;

close(client_sock);
	}
	
	std::cerr << "[Rank " << my_rank << "] exchange_rank_info returning TRUE" << std::endl;
	return true;
}

void collect_rank_info(EpBuffer &ep, int rank, RankInfo &info) {
	info.qpns = ep.get_local_qpns();
	info.lids = ep.get_local_lids();
	auto [addr, rkey] = ep.get_mr_info();
	info.mr_addr = addr;
	info.mr_rkey = rkey;
	auto [subnet_prefix, interface_id] = ep.get_gid();
	info.gid_subnet_prefix = subnet_prefix;
	info.gid_interface_id = interface_id;
	info.ipc_handle = ep.get_ipc_handle();
	info.is_roce = ep.is_roce();
}

void print_separator(const std::string &title) {
	std::cout << "\n" << std::string(60, '=') << "\n";
	std::cout << title << "\n";
	std::cout << std::string(60, '=') << "\n";
}

__nv_bfloat16 float2bf16(float f) { return __float2bfloat16(f); }

float bf162float(__nv_bfloat16 b) { return __bfloat162float(b); }

void test_dispatch_combine(EpBuffer &ep, int rank, int num_ranks,
						   const std::vector<RankInfo> &all_info,
						   const std::vector<int> &active_ranks, int num_tokens,
						   int hidden, int num_topk, int num_experts,
						   int num_max_dispatch_tokens_per_rank) {
	print_separator("Test Dispatch + Combine [Rank " + std::to_string(rank) +
					"]");

	if (all_info.empty()) {
		std::cerr << "Skipped: Need multi-rank info" << std::endl;
		std::cout << "PASSED (skipped)!" << std::endl;
		return;
	}
	
	std::cerr << "[Rank " << rank << "] all_info.size=" << all_info.size() << ", num_ranks=" << num_ranks << std::endl;

	std::mt19937 rng(rank * 12345);
	std::uniform_real_distribution<float> dist(0.1f, 1.0f);

	void *gdr_buffer = ep.get_gdr_buffer();
	void *workspace = ep.get_workspace();

	auto buffer_pair = ep.get_buffer_pair(num_max_dispatch_tokens_per_rank,
										  hidden, num_experts);
	auto &send_buf = buffer_pair.buffers[ep.get_buffer_idx()];

	__nv_bfloat16 *x = nullptr;
	int64_t *topk_idx = nullptr;
	float *topk_weights = nullptr;
	int32_t *active_ranks_d = nullptr;

	cudaMalloc(&x, num_tokens * hidden * sizeof(__nv_bfloat16));
	cudaMalloc(&topk_idx, num_tokens * num_topk * sizeof(int64_t));
	cudaMalloc(&topk_weights, num_tokens * num_topk * sizeof(float));
	cudaMalloc(&active_ranks_d, num_ranks * sizeof(int32_t));

	__nv_bfloat16 *packed_recv_x = nullptr;
	float *packed_recv_x_scales = nullptr;
	int *packed_recv_src_info = nullptr;
	int64_t *packed_recv_layout_range = nullptr;
	int *packed_recv_count = nullptr;
	__nv_bfloat16 *combined_x = nullptr;
	int *next_clean_buffer = nullptr;

	cudaMalloc(&packed_recv_x, num_experts * num_ranks *
								   num_max_dispatch_tokens_per_rank * hidden *
								   sizeof(__nv_bfloat16));
	cudaMalloc(&packed_recv_x_scales, num_experts * num_ranks *
										  num_max_dispatch_tokens_per_rank *
										  (hidden / 128) * sizeof(float));
	cudaMalloc(&packed_recv_src_info, num_experts * num_ranks *
										  num_max_dispatch_tokens_per_rank *
										  sizeof(int));
	cudaMalloc(&packed_recv_layout_range,
			   num_experts * num_ranks * sizeof(int64_t));
	cudaMalloc(&packed_recv_count, num_experts * sizeof(int));
	cudaMalloc(&combined_x, num_tokens * hidden * sizeof(__nv_bfloat16));
	cudaMalloc(&next_clean_buffer, num_experts * sizeof(int));

	std::vector<__nv_bfloat16> x_host(num_tokens * hidden);
	std::vector<int64_t> topk_idx_host(num_tokens * num_topk);
	std::vector<float> topk_weights_host(num_tokens * num_topk);
	std::vector<int32_t> active_ranks_host = active_ranks;

	int rank_offset = 128;
	for (int i = 0; i < num_tokens; ++i) {
		for (int j = 0; j < hidden; ++j) {
			x_host[i * hidden + j] = float2bf16((float)(rank - rank_offset));
		}
	}

	for (int i = 0; i < num_tokens; ++i) {
		std::vector<int64_t> expert_candidates(num_experts);
		for (int j = 0; j < num_experts; ++j) {
			expert_candidates[j] = j;
		}
		std::shuffle(expert_candidates.begin(), expert_candidates.end(), rng);
		for (int j = 0; j < num_topk; ++j) {
			topk_idx_host[i * num_topk + j] = expert_candidates[j];
		}
	}

	for (int i = 0; i < num_tokens; ++i) {
		for (int j = 0; j < num_topk; ++j) {
			topk_weights_host[i * num_topk + j] = dist(rng);
		}
	}

	cudaMemcpy(x, x_host.data(), num_tokens * hidden * sizeof(__nv_bfloat16),
			   cudaMemcpyHostToDevice);
	cudaMemcpy(topk_idx, topk_idx_host.data(),
			   num_tokens * num_topk * sizeof(int64_t), cudaMemcpyHostToDevice);
	cudaMemcpy(topk_weights, topk_weights_host.data(),
			   num_tokens * num_topk * sizeof(float), cudaMemcpyHostToDevice);
	cudaMemcpy(active_ranks_d, active_ranks_host.data(),
			   num_ranks * sizeof(int32_t), cudaMemcpyHostToDevice);
	cudaMemset(packed_recv_x, 0,
			   num_experts * num_ranks * num_max_dispatch_tokens_per_rank *
				   hidden * sizeof(__nv_bfloat16));
	cudaMemset(packed_recv_count, 0, num_experts * sizeof(int));

	cudaStream_t stream;
	cudaStreamCreate(&stream);

	std::cout << "Calling dispatch kernel..." << std::endl;
	ep::dispatch(packed_recv_x, packed_recv_x_scales, packed_recv_src_info,
				 packed_recv_layout_range, packed_recv_count, active_ranks_d,
				 gdr_buffer, send_buf.rdma_send_signal_buffer,
				 send_buf.rdma_recv_signal_buffer,
				 send_buf.rdma_send_data_buffer, send_buf.rdma_recv_data_buffer,
				 workspace, nullptr, ep.get_raddrs(), ep.get_rkeys(),
				 ep.get_qp_devctxs(), ep.get_nvlink_available(),
				 ep.get_ipc_peer_ptrs(), x, topk_idx, next_clean_buffer,
				 num_tokens, hidden, num_max_dispatch_tokens_per_rank, num_topk,
				 num_experts, rank, num_ranks, false, workspace, stream, -1,
				 LOW_LATENCY_SEND_PHASE | LOW_LATENCY_RECV_PHASE);

	cudaStreamSynchronize(stream);
	std::cout << "Dispatch kernel completed" << std::endl;

	std::vector<int> packed_recv_count_host(num_experts);
	cudaMemcpy(packed_recv_count_host.data(), packed_recv_count,
			   num_experts * sizeof(int), cudaMemcpyDeviceToHost);

	int total_recv_tokens = 0;
	for (int i = 0; i < num_experts; ++i) {
		total_recv_tokens += packed_recv_count_host[i];
		std::cout << "Expert " << i << " received " << packed_recv_count_host[i]
				  << " tokens" << std::endl;
	}
	std::cout << "Total received tokens: " << total_recv_tokens << std::endl;

	if (total_recv_tokens > 0) {
		std::vector<__nv_bfloat16> recv_x_host(
			num_experts * num_ranks * num_max_dispatch_tokens_per_rank *
			hidden);
		cudaMemcpy(recv_x_host.data(), packed_recv_x,
				   num_experts * num_ranks * num_max_dispatch_tokens_per_rank *
					   hidden * sizeof(__nv_bfloat16),
				   cudaMemcpyDeviceToHost);

		std::cout << "Sample received data (first token, first 5 dims): ";
		for (int j = 0; j < 5; ++j) {
			std::cout << bf162float(recv_x_host[j]) << " ";
		}
		std::cout << std::endl;
	}

	std::cout << "Calling combine kernel..." << std::endl;
	ep::combine(
		combined_x, active_ranks_d, gdr_buffer,
		send_buf.rdma_send_signal_buffer, send_buf.rdma_recv_signal_buffer,
		send_buf.rdma_send_data_buffer, send_buf.rdma_recv_data_buffer,
		workspace, nullptr, ep.get_raddrs(), ep.get_rkeys(),
		ep.get_qp_devctxs(), ep.get_nvlink_available(), ep.get_ipc_peer_ptrs(),
		packed_recv_x, topk_idx, topk_weights, packed_recv_src_info,
		packed_recv_layout_range, next_clean_buffer, num_tokens, hidden,
		num_max_dispatch_tokens_per_rank, num_topk, num_experts, rank,
		num_ranks, workspace, stream, -1,
		LOW_LATENCY_SEND_PHASE | LOW_LATENCY_RECV_PHASE, false);

	cudaStreamSynchronize(stream);
	std::cout << "Combine kernel completed" << std::endl;

	std::vector<__nv_bfloat16> combined_x_host(num_tokens * hidden);
	cudaMemcpy(combined_x_host.data(), combined_x,
			   num_tokens * hidden * sizeof(__nv_bfloat16),
			   cudaMemcpyDeviceToHost);

	std::cout << "Sample combined output (first 5 dims): ";
	for (int j = 0; j < 5; ++j) {
		std::cout << bf162float(combined_x_host[j]) << " ";
	}
	std::cout << std::endl;

	cudaStreamDestroy(stream);

	cudaFree(x);
	cudaFree(topk_idx);
	cudaFree(topk_weights);
	cudaFree(active_ranks_d);
	cudaFree(packed_recv_x);
	cudaFree(packed_recv_x_scales);
	cudaFree(packed_recv_src_info);
	cudaFree(packed_recv_layout_range);
	cudaFree(packed_recv_count);
	cudaFree(combined_x);
	cudaFree(next_clean_buffer);

	std::cout << "PASSED!" << std::endl;
}

void test_sync_ib(EpBuffer &ep, int rank, const std::vector<RankInfo> &all_info,
				  const std::vector<int> &active_ranks) {
	print_separator("Test sync_ib [Rank " + std::to_string(rank) + "]");

	if (ep.ibgda_disabled()) {
		std::cout << "Skipped: IBGDA disabled" << std::endl;
		std::cout << "PASSED (skipped)!" << std::endl;
		return;
	}

	std::vector<int64_t> remote_addrs;
	std::vector<int32_t> remote_keys;
	std::vector<int32_t> remote_qpns;
	std::vector<int32_t> remote_lids;

	for (size_t i = 0; i < all_info.size(); ++i) {
		remote_addrs.push_back(all_info[i].mr_addr);
		remote_keys.push_back(all_info[i].mr_rkey);
		remote_qpns.push_back(all_info[i].qpns.empty() ? 0
													   : all_info[i].qpns[0]);
		remote_lids.push_back(all_info[i].lids.empty() ? 0
													   : all_info[i].lids[0]);
	}

	std::cout << "Calling sync_ib..." << std::endl;
	ep.sync_ib(remote_addrs, remote_keys, remote_qpns, remote_lids,
			   active_ranks);
	std::cout << "sync_ib completed" << std::endl;
	std::cout << "PASSED!" << std::endl;
}

void test_sync_roce(EpBuffer &ep, int rank,
					const std::vector<RankInfo> &all_info,
					const std::vector<int> &active_ranks) {
	print_separator("Test sync_roce [Rank " + std::to_string(rank) + "]");

	if (!ep.is_roce()) {
		std::cout << "Skipped: Not using RoCE" << std::endl;
		std::cout << "PASSED (skipped)!" << std::endl;
		return;
	}

	std::vector<int64_t> remote_addrs;
	std::vector<int32_t> remote_keys;
	std::vector<int32_t> remote_qpns;
	std::vector<int64_t> subnet_prefixes;
	std::vector<int64_t> interface_ids;

	for (size_t i = 0; i < all_info.size(); ++i) {
		remote_addrs.push_back(all_info[i].mr_addr);
		remote_keys.push_back(all_info[i].mr_rkey);
		remote_qpns.push_back(all_info[i].qpns.empty() ? 0
													   : all_info[i].qpns[0]);
		subnet_prefixes.push_back(all_info[i].gid_subnet_prefix);
		interface_ids.push_back(all_info[i].gid_interface_id);
	}

	std::cout << "Calling sync_roce..." << std::endl;
	ep.sync_roce(remote_addrs, remote_keys, remote_qpns, subnet_prefixes,
				 interface_ids, active_ranks);
	std::cout << "sync_roce completed" << std::endl;
	std::cout << "PASSED!" << std::endl;
}

void test_nvlink_ipc(EpBuffer &ep, int rank,
					 const std::vector<RankInfo> &all_info,
					 const std::vector<int> &active_ranks) {
	print_separator("Test NVLink IPC [Rank " + std::to_string(rank) + "]");

	std::vector<std::vector<int32_t>> remote_handles;
	for (const auto &info : all_info) {
		remote_handles.push_back(info.ipc_handle);
	}

	std::cout << "Calling sync_nvlink_ipc_handles..." << std::endl;
	ep.sync_nvlink_ipc_handles(remote_handles, active_ranks);
	std::cout << "sync_nvlink_ipc_handles completed" << std::endl;
	std::cout << "PASSED!" << std::endl;
}

int main(int argc, char **argv) {
	int rank = 0;
	int num_ranks = 1;
	std::string device_name = "mlx5_0";
	std::string my_ip = "127.0.0.1";
	std::vector<std::string> peer_ips;

	if (argc >= 3) {
		rank = std::atoi(argv[1]);
		num_ranks = std::atoi(argv[2]);
	}
	if (argc >= 4)
		device_name = argv[3];
	if (argc >= 5)
		my_ip = argv[4];
	if (argc >= 6) {
		for (int i = 5; i < argc; ++i) {
			peer_ips.push_back(argv[i]);
		}
	} else {
		peer_ips.push_back("127.0.0.1");
	}

	std::cout << "=== EP Dispatch/Combine Test ===" << std::endl;
	std::cout << "Rank: " << rank << "/" << num_ranks << std::endl;
	std::cout << "Device: " << device_name << std::endl;
	std::cout << "My IP: " << my_ip << std::endl;

	int num_tokens = 128;
	int hidden = 7168;
	int num_topk = 8;
	int num_experts = 288;
	int num_max_dispatch_tokens_per_rank = 128;

	std::cout << "Test parameters:" << std::endl;
	std::cout << "	num_tokens: " << num_tokens << std::endl;
	std::cout << "	hidden: " << hidden << std::endl;
	std::cout << "	num_topk: " << num_topk << std::endl;
	std::cout << "	num_experts: " << num_experts << std::endl;
	std::cout << "	num_max_dispatch_tokens_per_rank: "
			  << num_max_dispatch_tokens_per_rank << std::endl;

	int64_t buffer_size = get_ep_buffer_size_hint(
		num_max_dispatch_tokens_per_rank, hidden, num_ranks, num_experts);
	std::cout << "Buffer size: " << buffer_size / (1024 * 1024) << " MiB"
			  << std::endl;

	try {
		EpBuffer ep_buffer(rank, num_ranks, buffer_size, device_name);

		std::vector<int> active_ranks_vec(num_ranks, 1);
		std::vector<RankInfo> all_info;

		if (num_ranks > 1) {
			std::cerr << "[Rank " << rank << "] Before FIRST exchange_rank_info..." << std::endl;
			RankInfo my_info;
			collect_rank_info(ep_buffer, rank, my_info);
			std::cerr << "[Rank " << rank << "] Calling FIRST exchange_rank_info..." << std::endl;
			bool result = exchange_rank_info(rank, num_ranks, peer_ips, my_info, all_info);
			std::cerr << "[Rank " << rank << "] exchange_rank_info returned: " << result << std::endl;
			
			if (!result) {
				std::cerr << "Failed to exchange rank info at line " << __LINE__ << " (FIRST)" << std::endl;
				return 1;
			}
			std::cerr << "[Rank " << rank << "] FIRST exchange_rank_info succeeded! all_info.size=" << all_info.size() << std::endl;

			test_sync_ib(ep_buffer, rank, all_info, active_ranks_vec);
			test_sync_roce(ep_buffer, rank, all_info, active_ranks_vec);
			test_nvlink_ipc(ep_buffer, rank, all_info, active_ranks_vec);
		}

		std::cerr << "[Rank " << rank << "] Calling test_dispatch_combine..." << std::endl;
		test_dispatch_combine(ep_buffer, rank, num_ranks, all_info,
							  active_ranks_vec, num_tokens, hidden, num_topk,
							  num_experts, num_max_dispatch_tokens_per_rank);
		std::cerr << "[Rank " << rank << "] test_dispatch_combine returned" << std::endl;

		print_separator("ALL TESTS PASSED [Rank " + std::to_string(rank) +
						"]!");
		return 0;

	} catch (const std::exception &e) {
		std::cerr << "ERROR [Rank " << rank << "]: " << e.what() << std::endl;
		return 1;
	}
}
