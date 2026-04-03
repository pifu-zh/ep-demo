#include <arpa/inet.h>
#include <chrono>
#include <cstring>
#include <ep_buffer.h>
#include <fcntl.h>
#include <iomanip>
#include <iostream>
#include <poll.h>
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
	all_info.resize(num_ranks);

	if (my_rank == 0) {
		int server_sock = create_server_socket(BASE_PORT);
		if (server_sock < 0) {
			std::cerr << "Failed to create server socket" << std::endl;
			return false;
		}

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
			std::cerr << "[Rank 0] Receiving info from rank " << i << "..."
					  << std::endl;
			if (!recv_vector(sock, all_info[i].qpns))
				return false;
			if (!recv_vector(sock, all_info[i].lids))
				return false;
			if (!recv_all(sock, &all_info[i].mr_addr, sizeof(int64_t)))
				return false;
			if (!recv_all(sock, &all_info[i].mr_rkey, sizeof(int32_t)))
				return false;
			if (!recv_all(sock, &all_info[i].gid_subnet_prefix,
						  sizeof(int64_t)))
				return false;
			if (!recv_all(sock, &all_info[i].gid_interface_id, sizeof(int64_t)))
				return false;
			if (!recv_vector(sock, all_info[i].ipc_handle))
				return false;
			if (!recv_all(sock, &all_info[i].is_roce, sizeof(bool)))
				return false;
			std::cerr << "[Rank 0] Received info from rank " << i << "!"
					  << std::endl;
		}

		std::cerr << "[Rank 0] Sending all_info back to rank 1..." << std::endl;
		for (int i = 1; i < num_ranks; ++i) {
			int sock = client_socks[i - 1];
			for (int j = 0; j < num_ranks; ++j) {
				std::cerr << "[Rank 0] Sending info to rank " << j << "..."
						  << std::endl;
				if (!send_vector(sock, all_info[j].qpns))
					return false;
				if (!send_vector(sock, all_info[j].lids))
					return false;
				if (!send_all(sock, &all_info[j].mr_addr, sizeof(int64_t)))
					return false;
				if (!send_all(sock, &all_info[j].mr_rkey, sizeof(int32_t)))
					return false;
				if (!send_all(sock, &all_info[j].gid_subnet_prefix,
							  sizeof(int64_t)))
					return false;
				if (!send_all(sock, &all_info[j].gid_interface_id,
							  sizeof(int64_t)))
					return false;
				if (!send_vector(sock, all_info[j].ipc_handle))
					return false;
				if (!send_all(sock, &all_info[j].is_roce, sizeof(bool)))
					return false;
			}
			std::cerr << "[Rank 0] Sent all_info to rank " << i << "!"
					  << std::endl;
		}

		for (int sock : client_socks)
			close(sock);
		close(server_sock);

	} else {
		std::this_thread::sleep_for(std::chrono::milliseconds(500));
		std::cerr << "[Rank " << my_rank << "] Creating client socket to "
				  << peer_ips[0] << ":" << BASE_PORT << "..." << std::endl;

		int client_sock = create_client_socket(peer_ips[0].c_str(), BASE_PORT);
		if (client_sock < 0) {
			std::cerr << "Failed to connect to rank 0" << std::endl;
			return false;
		}
		std::cerr << "[Rank " << my_rank << "] Connected! Sending info..."
				  << std::endl;

		if (!send_vector(client_sock, my_info.qpns))
			return false;
		if (!send_vector(client_sock, my_info.lids))
			return false;
		if (!send_all(client_sock, &my_info.mr_addr, sizeof(int64_t)))
			return false;
		if (!send_all(client_sock, &my_info.mr_rkey, sizeof(int32_t)))
			return false;
		if (!send_all(client_sock, &my_info.gid_subnet_prefix, sizeof(int64_t)))
			return false;
		if (!send_all(client_sock, &my_info.gid_interface_id, sizeof(int64_t)))
			return false;
		if (!send_vector(client_sock, my_info.ipc_handle))
			return false;
		if (!send_all(client_sock, &my_info.is_roce, sizeof(bool)))
			return false;

		all_info.resize(num_ranks);

		for (int j = 0; j < num_ranks; ++j) {
			std::cerr << "[Rank " << my_rank << "] Receiving info from rank "
					  << j << "..." << std::endl;
			if (!recv_vector(client_sock, all_info[j].qpns))
				return false;
			if (!recv_vector(client_sock, all_info[j].lids))
				return false;
			if (!recv_all(client_sock, &all_info[j].mr_addr, sizeof(int64_t)))
				return false;
			if (!recv_all(client_sock, &all_info[j].mr_rkey, sizeof(int32_t)))
				return false;
			if (!recv_all(client_sock, &all_info[j].gid_subnet_prefix,
						  sizeof(int64_t)))
				return false;
			if (!recv_all(client_sock, &all_info[j].gid_interface_id,
						  sizeof(int64_t)))
				return false;
			if (!recv_vector(client_sock, all_info[j].ipc_handle))
				return false;
			if (!recv_all(client_sock, &all_info[j].is_roce, sizeof(bool)))
				return false;
		}

		std::cerr << "[Rank " << my_rank
				  << "] All info received! Closing socket..." << std::endl;
		close(client_sock);
	}

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

void test_basic_init(EpBuffer &ep, int rank) {
	print_separator("Test 1: Basic Initialization [Rank " +
					std::to_string(rank) + "]");

	std::cout << "IBGDA disabled: " << (ep.ibgda_disabled() ? "yes" : "no")
			  << std::endl;
	std::cout << "Is RoCE: " << (ep.is_roce() ? "yes" : "no") << std::endl;
	std::cout << "Use fast path: " << (ep.use_fast_path() ? "yes" : "no")
			  << std::endl;
	std::cout << "PASSED!" << std::endl;
}

void test_mr_info(EpBuffer &ep, int rank) {
	print_separator("Test 2: Memory Region Info [Rank " + std::to_string(rank) +
					"]");

	auto [addr, rkey] = ep.get_mr_info();
	std::cout << "MR address: 0x" << std::hex << addr << std::dec << std::endl;
	std::cout << "MR rkey: 0x" << std::hex << rkey << std::dec << std::endl;
	std::cout << "PASSED!" << std::endl;
}

void test_gid(EpBuffer &ep, int rank) {
	print_separator("Test 3: GID [Rank " + std::to_string(rank) + "]");

	auto [subnet_prefix, interface_id] = ep.get_gid();
	std::cout << "Subnet prefix: 0x" << std::hex << subnet_prefix << std::dec
			  << std::endl;
	std::cout << "Interface ID: 0x" << std::hex << interface_id << std::dec
			  << std::endl;
	std::cout << "PASSED!" << std::endl;
}

void test_qpns_lids(EpBuffer &ep, int rank) {
	print_separator("Test 4: QPNs and LIDs [Rank " + std::to_string(rank) +
					"]");

	auto qpns = ep.get_local_qpns();
	auto lids = ep.get_local_lids();

	std::cout << "QPN count: " << qpns.size() << std::endl;
	std::cout << "First 4 QPNs: ";
	for (size_t i = 0; i < std::min(size_t(4), qpns.size()); ++i) {
		std::cout << qpns[i] << " ";
	}
	std::cout << std::endl;

	std::cout << "LID count: " << lids.size() << std::endl;
	std::cout << "First 4 LIDs: ";
	for (size_t i = 0; i < std::min(size_t(4), lids.size()); ++i) {
		std::cout << lids[i] << " ";
	}
	std::cout << std::endl;
	std::cout << "PASSED!" << std::endl;
}

void test_ipc_handle(EpBuffer &ep, int rank) {
	print_separator("Test 5: IPC Handle [Rank " + std::to_string(rank) + "]");

	auto handle = ep.get_ipc_handle();
	std::cout << "IPC handle size: " << handle.size() << std::endl;
	std::cout << "PASSED!" << std::endl;
}

void test_sync_ib(EpBuffer &ep, int rank, const std::vector<RankInfo> &all_info,
				  const std::vector<int> &active_ranks) {
	print_separator("Test 6: sync_ib [Rank " + std::to_string(rank) + "]");

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
	print_separator("Test 7: sync_roce [Rank " + std::to_string(rank) + "]");

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
	print_separator("Test 8: NVLink IPC [Rank " + std::to_string(rank) + "]");

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

	std::cout << "=== EP Multi-Process Test ===" << std::endl;
	std::cout << "Rank: " << rank << "/" << num_ranks << std::endl;
	std::cout << "Device: " << device_name << std::endl;
	std::cout << "My IP: " << my_ip << std::endl;
	std::cout << "Buffer size: 256 MiB" << std::endl;

	try {
		EpBuffer ep_buffer(rank, num_ranks, 256 * 1024 * 1024, device_name);

		std::vector<int> active_ranks(num_ranks, 1);

		std::cerr << "[Rank " << rank << "] Before exchange_rank_info..."
				  << std::endl;
		if (num_ranks > 1) {
			RankInfo my_info;
			collect_rank_info(ep_buffer, rank, my_info);

			std::vector<RankInfo> all_info;
			if (!exchange_rank_info(rank, num_ranks, peer_ips, my_info,
									all_info)) {
				std::cerr << "Failed to exchange rank info" << std::endl;
				return 1;
			}

			std::cerr << "[Rank " << rank
					  << "] exchange_rank_info succeeded! all_info.size="
					  << all_info.size() << std::endl;

			test_basic_init(ep_buffer, rank);
			test_mr_info(ep_buffer, rank);
			test_gid(ep_buffer, rank);
			test_qpns_lids(ep_buffer, rank);
			test_ipc_handle(ep_buffer, rank);
			test_sync_ib(ep_buffer, rank, all_info, active_ranks);
			test_sync_roce(ep_buffer, rank, all_info, active_ranks);
			test_nvlink_ipc(ep_buffer, rank, all_info, active_ranks);
		} else {
			test_basic_init(ep_buffer, rank);
			test_mr_info(ep_buffer, rank);
			test_gid(ep_buffer, rank);
			test_qpns_lids(ep_buffer, rank);
			test_ipc_handle(ep_buffer, rank);
		}

		print_separator("ALL TESTS PASSED [Rank " + std::to_string(rank) +
						"]!");
		return 0;

	} catch (const std::exception &e) {
		std::cerr << "ERROR [Rank " << rank << "]: " << e.what() << std::endl;
		return 1;
	}
}
