cmake_minimum_required(VERSION 3.10)
project(dpdk_client)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -march=native -mtune=native")

set(DPDK_ROOT "/usr/local")
set(DPDK_INCLUDE_DIR "${DPDK_ROOT}/include")
set(DPDK_LIB_DIR "${DPDK_ROOT}/lib/x86_64-linux-gnu")

find_library(RTE_EAL rte_eal PATHS ${DPDK_LIB_DIR} REQUIRED)
find_library(RTE_ETHDEV rte_ethdev PATHS ${DPDK_LIB_DIR} REQUIRED)
find_library(RTE_MBUF rte_mbuf PATHS ${DPDK_LIB_DIR} REQUIRED)
find_library(RTE_MEMPOOL rte_mempool PATHS ${DPDK_LIB_DIR} REQUIRED)
find_library(RTE_NET rte_net PATHS ${DPDK_LIB_DIR} REQUIRED)

include_directories(
    ${DPDK_INCLUDE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}
)

add_executable(dpdk_client
    dpdk_main.cpp
    dpdk_client.cpp
)

target_link_libraries(dpdk_client
    ${RTE_EAL}
    ${RTE_ETHDEV}
    ${RTE_MBUF}
    ${RTE_MEMPOOL}
    ${RTE_NET}
    pthread
    numa
    m
    dl
    stdc++
)

add_executable(dpdk_bimodal_client
    dpdk_bimodal_main.cpp
    dpdk_client.cpp
)

target_link_libraries(dpdk_bimodal_client
    ${RTE_EAL}
    ${RTE_ETHDEV}
    ${RTE_MBUF}
    ${RTE_MEMPOOL}
    ${RTE_NET}
    pthread
    numa
    m
    dl
    stdc++
)

# add_executable(merge_stats
#     merge_stats.cpp
# )

set_target_properties(dpdk_client PROPERTIES
    LINK_FLAGS "-Wl,-rpath,${DPDK_LIB_DIR}"
)

set_target_properties(dpdk_bimodal_client PROPERTIES
    LINK_FLAGS "-Wl,-rpath,${DPDK_LIB_DIR}"
)