cmake_minimum_required(VERSION 3.16)
project(xrcomm_hello_world C)

# Force the output directory for executables to be the local bin/ folder
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/bin)

# Enable required C standards
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Find DPDK for the fullpkt consumer
find_package(PkgConfig REQUIRED)
pkg_check_modules(DPDK REQUIRED libdpdk)
find_library(NUMA_LIB numa REQUIRED)

# 1. Standard IQ Hello World
add_executable(hello_world hello_world.c)
target_include_directories(hello_world PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../platform/XRComm_platform_drivers/include)
target_compile_options(hello_world PRIVATE -O3 -march=native -mavx2 -Wall -Wextra -Wno-unused-parameter)
target_link_libraries(hello_world PRIVATE pthread m rt)

# 2. FULL_PACKET Hello World (Requires DPDK)
add_executable(hello_world_fullpkt hello_world_fullpkt.c)
target_include_directories(hello_world_fullpkt PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../platform/XRComm_platform_drivers/include
    ${DPDK_INCLUDE_DIRS})
target_compile_options(hello_world_fullpkt PRIVATE ${DPDK_CFLAGS} -O3 -march=native -mavx2 -Wall -Wextra -Wno-unused-parameter)
target_link_libraries(hello_world_fullpkt PRIVATE ${DPDK_LIBRARIES} rte_bus_pci rte_pci ${NUMA_LIB} pthread m rt)
