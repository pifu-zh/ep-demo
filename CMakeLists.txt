cmake_minimum_required(VERSION 3.16)
project(ep-demo)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(CUDAToolkit REQUIRED)
find_package(Threads REQUIRED)

include_directories(include)

add_executable(test_init test/test_init.cpp
    src/ep_buffer.cpp
    src/ep_ibgda/mlx5gda.cpp
)

add_executable(test_suite test/test_suite.cpp
    src/ep_buffer.cpp
    src/ep_ibgda/mlx5gda.cpp
)

add_executable(test_multi test/test_multi.cpp
    src/ep_buffer.cpp
    src/ep_ibgda/mlx5gda.cpp
)

foreach(target test_init test_suite test_multi)
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
