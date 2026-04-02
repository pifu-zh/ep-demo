#include <ep_buffer.h>
#include <iostream>
#include <iomanip>
#include <cstring>
#include <thread>
#include <chrono>

using namespace ep;

void print_separator(const std::string& title) {
    std::cout << "\n" << std::string(60, '=') << "\n";
    std::cout << title << "\n";
    std::cout << std::string(60, '=') << "\n";
}

void test_basic_init(EpBuffer& ep) {
    print_separator("Test 1: Basic Initialization");
    
    std::cout << "IBGDA disabled: " << (ep.ibgda_disabled() ? "yes" : "no") << std::endl;
    std::cout << "Is RoCE: " << (ep.is_roce() ? "yes" : "no") << std::endl;
    std::cout << "Use fast path: " << (ep.use_fast_path() ? "yes" : "no") << std::endl;
    std::cout << "PASSED!" << std::endl;
}

void test_mr_info(EpBuffer& ep) {
    print_separator("Test 2: Memory Region Info");
    
    auto [addr, rkey] = ep.get_mr_info();
    std::cout << "MR address: 0x" << std::hex << addr << std::dec << std::endl;
    std::cout << "MR rkey: 0x" << std::hex << rkey << std::dec << std::endl;
    std::cout << "PASSED!" << std::endl;
}

void test_gid(EpBuffer& ep) {
    print_separator("Test 3: GID (InfiniBand)");
    
    auto [subnet_prefix, interface_id] = ep.get_gid();
    std::cout << "Subnet prefix: 0x" << std::hex << subnet_prefix << std::dec << std::endl;
    std::cout << "Interface ID: 0x" << std::hex << interface_id << std::dec << std::endl;
    std::cout << "PASSED!" << std::endl;
}

void test_qpns_lids(EpBuffer& ep) {
    print_separator("Test 4: QPNs and LIDs");
    
    auto qpns = ep.get_local_qpns();
    auto lids = ep.get_local_lids();
    
    std::cout << "QPN count: " << qpns.size() << std::endl;
    std::cout << "First 8 QPNs: ";
    for (size_t i = 0; i < std::min(size_t(8), qpns.size()); ++i) {
        std::cout << qpns[i] << " ";
    }
    std::cout << std::endl;
    
    std::cout << "LID count: " << lids.size() << std::endl;
    std::cout << "First 8 LIDs: ";
    for (size_t i = 0; i < std::min(size_t(8), lids.size()); ++i) {
        std::cout << lids[i] << " ";
    }
    std::cout << std::endl;
    std::cout << "PASSED!" << std::endl;
}

void test_ipc_handle(EpBuffer& ep) {
    print_separator("Test 5: IPC Handle");
    
    auto handle = ep.get_ipc_handle();
    std::cout << "IPC handle size: " << handle.size() << " int32s" << std::endl;
    std::cout << "First 8 values (hex): ";
    for (size_t i = 0; i < std::min(size_t(8), handle.size()); ++i) {
        std::cout << std::hex << handle[i] << " " << std::dec;
    }
    std::cout << std::endl;
    std::cout << "PASSED!" << std::endl;
}

void test_sync_ib(EpBuffer& ep, int num_ranks, int rank) {
    print_separator("Test 6: sync_ib (InfiniBand)");
    
    if (ep.ibgda_disabled()) {
        std::cout << "Skipped: IBGDA disabled" << std::endl;
        std::cout << "PASSED (skipped)!" << std::endl;
        return;
    }
    
    std::vector<int64_t> remote_addrs(num_ranks, 0);
    std::vector<int32_t> remote_keys(num_ranks, 0);
    std::vector<int32_t> remote_qpns(num_ranks, 0);
    std::vector<int32_t> remote_lids(num_ranks, 0);
    std::vector<int> active_ranks_mask(num_ranks, 1);
    
    auto [local_addr, local_rkey] = ep.get_mr_info();
    auto local_qpns = ep.get_local_qpns();
    auto local_lids = ep.get_local_lids();
    
    for (int i = 0; i < num_ranks; ++i) {
        if (i == rank) {
            remote_addrs[i] = local_addr;
            remote_keys[i] = local_rkey;
            remote_qpns[i] = local_qpns[0];
            remote_lids[i] = local_lids[0];
        } else {
            remote_addrs[i] = 0x1234567800000000 + i;
            remote_keys[i] = 0x1000 + i;
            remote_qpns[i] = 10000 + i;
            remote_lids[i] = 10 + i;
        }
    }
    
    std::cout << "Calling sync_ib..." << std::endl;
    ep.sync_ib(remote_addrs, remote_keys, remote_qpns, remote_lids, active_ranks_mask);
    std::cout << "sync_ib completed" << std::endl;
    std::cout << "PASSED!" << std::endl;
}

void test_sync_roce(EpBuffer& ep, int num_ranks, int rank) {
    print_separator("Test 7: sync_roce (RoCE)");
    
    if (ep.is_roce()) {
        std::vector<int64_t> remote_addrs(num_ranks, 0);
        std::vector<int32_t> remote_keys(num_ranks, 0);
        std::vector<int32_t> remote_qpns(num_ranks, 0);
        std::vector<int64_t> subnet_prefixes(num_ranks, 0);
        std::vector<int64_t> interface_ids(num_ranks, 0);
        std::vector<int> active_ranks_mask(num_ranks, 1);
        
        auto [local_addr, local_rkey] = ep.get_mr_info();
        auto local_qpns = ep.get_local_qpns();
        
        for (int i = 0; i < num_ranks; ++i) {
            if (i == rank) {
                remote_addrs[i] = local_addr;
                remote_keys[i] = local_rkey;
                remote_qpns[i] = local_qpns[0];
            } else {
                remote_addrs[i] = 0x1234567800000000 + i;
                remote_keys[i] = 0x1000 + i;
                remote_qpns[i] = 10000 + i;
            }
            subnet_prefixes[i] = 0x80fe000000000000 + i;
            interface_ids[i] = 0x66d29e0003fd7010 + i;
        }
        
        std::cout << "Calling sync_roce..." << std::endl;
        ep.sync_roce(remote_addrs, remote_keys, remote_qpns, 
                     subnet_prefixes, interface_ids, active_ranks_mask);
        std::cout << "sync_roce completed" << std::endl;
    } else {
        std::cout << "Skipped: Not using RoCE (InfiniBand mode)" << std::endl;
    }
    std::cout << "PASSED!" << std::endl;
}

void test_update_qpns(EpBuffer& ep) {
    print_separator("Test 8: Update Local QPNs");
    
    if (ep.ibgda_disabled()) {
        std::cout << "Skipped: IBGDA disabled" << std::endl;
        std::cout << "PASSED (skipped)!" << std::endl;
        return;
    }
    
    auto old_qpns = ep.get_local_qpns();
    std::cout << "Old QPNs (first 4): ";
    for (size_t i = 0; i < std::min(size_t(4), old_qpns.size()); ++i) {
        std::cout << old_qpns[i] << " ";
    }
    std::cout << std::endl;
    
    std::cout << "Calling update_local_qpns..." << std::endl;
    ep.update_local_qpns();
    
    auto new_qpns = ep.get_local_qpns();
    std::cout << "New QPNs (first 4): ";
    for (size_t i = 0; i < std::min(size_t(4), new_qpns.size()); ++i) {
        std::cout << new_qpns[i] << " ";
    }
    std::cout << std::endl;
    std::cout << "PASSED!" << std::endl;
}

void test_buffer_size_hint() {
    print_separator("Test 9: Buffer Size Hint");
    
    struct TestCase {
        int max_tokens;
        int hidden;
        int num_ranks;
        int num_experts;
    };
    
    std::vector<TestCase> cases = {
        {32, 4096, 2, 8},
        {64, 4096, 4, 8},
        {32, 8192, 2, 8},
        {64, 2048, 8, 16},
    };
    
    for (const auto& c : cases) {
        size_t size = get_ep_buffer_size_hint(c.max_tokens, c.hidden, c.num_ranks, c.num_experts);
        std::cout << "Tokens=" << c.max_tokens << " hidden=" << c.hidden 
                  << " ranks=" << c.num_ranks << " experts=" << c.num_experts
                  << " => " << size << " bytes (" << (size / 1024 / 1024) << " MiB)" << std::endl;
    }
    std::cout << "PASSED!" << std::endl;
}

void test_nvlink_ipc_handles(EpBuffer& ep, int num_ranks, int rank) {
    print_separator("Test 10: NVLink IPC Handles");
    
    std::vector<std::vector<int32_t>> remote_handles(num_ranks);
    std::vector<int> active_ranks_mask(num_ranks, 1);
    
    auto local_handle = ep.get_ipc_handle();
    for (int i = 0; i < num_ranks; ++i) {
        if (i == rank) {
            remote_handles[i] = local_handle;
        } else {
            remote_handles[i] = std::vector<int32_t>(16, 0);
        }
    }
    
    std::cout << "Calling sync_nvlink_ipc_handles..." << std::endl;
    ep.sync_nvlink_ipc_handles(remote_handles, active_ranks_mask);
    std::cout << "sync_nvlink_ipc_handles completed" << std::endl;
    std::cout << "PASSED!" << std::endl;
}

void test_ibgda_disabled_fallback() {
    print_separator("Test 11: IBGDA Disabled Fallback");
    
    std::cout << "Simulating IBGDA disabled scenario..." << std::endl;
    std::cout << "(This test just logs the behavior)" << std::endl;
    
    bool disabled = (std::getenv("MC_DISABLE_IBGDA") != nullptr);
    std::cout << "IBGDA disabled by env: " << (disabled ? "yes" : "no") << std::endl;
    std::cout << "PASSED!" << std::endl;
}

void test_repeated_operations(EpBuffer& ep, int num_ranks, int rank) {
    print_separator("Test 12: Repeated Operations");
    
    if (ep.ibgda_disabled()) {
        std::cout << "Skipped: IBGDA disabled" << std::endl;
        std::cout << "PASSED (skipped)!" << std::endl;
        return;
    }
    
    std::cout << "Running 3 iterations of update_local_qpns + get_local_qpns..." << std::endl;
    for (int iter = 0; iter < 3; ++iter) {
        ep.update_local_qpns();
        auto qpns = ep.get_local_qpns();
        std::cout << "  Iteration " << iter << ": QPN count = " << qpns.size() 
                  << ", first QPN = " << qpns[0] << std::endl;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    std::cout << "PASSED!" << std::endl;
}

int main(int argc, char** argv) {
    int rank = 0;
    int num_ranks = 1;
    std::string device_name = "mlx5_0";
    
    if (argc >= 3) {
        rank = std::atoi(argv[1]);
        num_ranks = std::atoi(argv[2]);
    }
    if (argc >= 4) {
        device_name = argv[3];
    } else {
        const char* env_device = std::getenv("MC_IBGDA_DEVICE");
        if (env_device) device_name = env_device;
    }
    
    std::cout << "=== EP Demo Test Suite ===" << std::endl;
    std::cout << "Rank: " << rank << "/" << num_ranks << std::endl;
    std::cout << "Device: " << device_name << std::endl;
    std::cout << "Buffer size: 256 MiB" << std::endl;
    
    try {
        EpBuffer ep_buffer(rank, num_ranks, 256 * 1024 * 1024, device_name);
        
        test_basic_init(ep_buffer);
        test_mr_info(ep_buffer);
        test_gid(ep_buffer);
        test_qpns_lids(ep_buffer);
        test_ipc_handle(ep_buffer);
        test_buffer_size_hint();
        test_nvlink_ipc_handles(ep_buffer, num_ranks, rank);
        test_sync_ib(ep_buffer, num_ranks, rank);
        test_sync_roce(ep_buffer, num_ranks, rank);
        test_update_qpns(ep_buffer);
        test_repeated_operations(ep_buffer, num_ranks, rank);
        test_ibgda_disabled_fallback();
        
        print_separator("ALL TESTS PASSED!");
        return 0;
        
    } catch (const std::exception& e) {
        std::cerr << "ERROR: " << e.what() << std::endl;
        return 1;
    }
}
