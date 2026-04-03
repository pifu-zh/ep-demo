cmake_minimum_required(VERSION 3.16)
project(ep-demo LANGUAGES C CXX CUDA)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(CUDAToolkit REQUIRED)

set(CMAKE_CUDA_IMPLICIT_INCLUDE_DIRECTORIES_INIT "${CUDAToolkit_INCLUDE_DIRS}")
find_package(Threads REQUIRED)

include_directories(
    include
    include/ep_ibgda
)

set(EP_KERNEL_SOURCES src/ep_kernel.cu)
set(CMAKE_CUDA_FLAGS "-I/usr/local/cuda-12.6/targets/x86_64-linux/include")

add_executable(test_init test/test_init.cpp
    src/ep_buffer.cpp
    src/ep_ibgda/mlx5gda.cpp
    ${EP_KERNEL_SOURCES}
)

add_executable(test_suite test/test_suite.cpp
    src/ep_buffer.cpp
    src/ep_ibgda/mlx5gda.cpp
    ${EP_KERNEL_SOURCES}
)

add_executable(test_multi test/test_multi.cpp
    src/ep_buffer.cpp
    src/ep_ibgda/mlx5gda.cpp
    ${EP_KERNEL_SOURCES}
)

add_executable(test_dispatch_combine test/test_dispatch_combine.cpp
    src/ep_buffer.cpp
    src/ep_ibgda/mlx5gda.cpp
    ${EP_KERNEL_SOURCES}
)

foreach(target test_init test_suite test_multi test_dispatch_combine)
    target_link_libraries(${target} PRIVATE
        CUDA::cuda_driver
        CUDA::cudart
        Threads::Threads
        ibverbs
        mlx5
        glog
    )

    target_include_directories(${target} PRIVATE
        ${CUDAToolkit_INCLUDE_DIRS}
    )
endforeach()