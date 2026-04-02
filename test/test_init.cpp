#include <ep_buffer.h>
#include <iostream>
#include <cstring>

using namespace ep;

int main(int argc, char** argv) {
    int rank = 0;
    int num_ranks = 1;
    int64_t buffer_size = 256 * 1024 * 1024;  // 256 MiB
    std::string device_name = "";

    if (argc >= 3) {
        rank = std::atoi(argv[1]);
        num_ranks = std::atoi(argv[2]);
    }

    if (argc >= 4) {
        device_name = argv[3];
    } else {
        const char* env_device = std::getenv("MC_IBGDA_DEVICE");
        if (env_device) {
            device_name = env_device;
        }
    }

    std::cout << "[Test] Rank " << rank << "/" << num_ranks 
              << ", buffer size: " << buffer_size << " bytes"
              << ", device: " << (device_name.empty() ? "(auto)" : device_name)
              << std::endl;

    try {
        ep::EpBuffer ep_buffer(rank, num_ranks, buffer_size, device_name);

        std::cout << "[Test] Buffer created successfully!" << std::endl;
        std::cout << "[Test] IBGDA disabled: " << (ep_buffer.ibgda_disabled() ? "yes" : "no") << std::endl;
        std::cout << "[Test] Is RoCE: " << (ep_buffer.is_roce() ? "yes" : "no") << std::endl;
        std::cout << "[Test] Use fast path: " << (ep_buffer.use_fast_path() ? "yes" : "no") << std::endl;

        if (!ep_buffer.ibgda_disabled()) {
            auto [addr, rkey] = ep_buffer.get_mr_info();
            std::cout << "[Test] MR addr: 0x" << std::hex << addr << std::dec 
                      << ", rkey: 0x" << std::hex << rkey << std::dec << std::endl;

            auto [subnet_prefix, interface_id] = ep_buffer.get_gid();
            std::cout << "[Test] GID: subnet_prefix=0x" << std::hex << subnet_prefix
                      << ", interface_id=0x" << interface_id << std::dec << std::endl;

            auto qpns = ep_buffer.get_local_qpns();
            std::cout << "[Test] Local QPNs: ";
            for (size_t i = 0; i < qpns.size() && i < 4; ++i) {
                std::cout << qpns[i] << " ";
            }
            if (qpns.size() > 4) std::cout << "... ";
            std::cout << "(total " << qpns.size() << ")" << std::endl;

            auto lids = ep_buffer.get_local_lids();
            std::cout << "[Test] Local LIDs: ";
            for (size_t i = 0; i < lids.size() && i < 4; ++i) {
                std::cout << lids[i] << " ";
            }
            if (lids.size() > 4) std::cout << "... ";
            std::cout << "(total " << lids.size() << ")" << std::endl;
        }

        auto ipc_handle = ep_buffer.get_ipc_handle();
        std::cout << "[Test] IPC handle size: " << ipc_handle.size() << std::endl;

        std::cout << "[Test] PASSED!" << std::endl;
        return 0;

    } catch (const std::exception& e) {
        std::cerr << "[Test] Exception: " << e.what() << std::endl;
        return 1;
    }
}
