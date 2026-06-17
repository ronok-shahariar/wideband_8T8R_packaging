cmake_minimum_required(VERSION 3.10)
project(xrcomm_hello_world C)

# Force the output directory for executables to be the local bin/ folder
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/bin)

# Enable required C standards
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED True)

# Set common flags (matching the optimized zero-drop requirements)
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -O3 -mavx2 -pthread -Wall")

# Find DPDK for the fullpkt consumer
find_package(PkgConfig REQUIRED)
pkg_check_modules(DPDK REQUIRED libdpdk)

# 1. Standard IQ Hello World
add_executable(hello_world hello_world.c)
target_link_libraries(hello_world m pthread)

# 2. FULL_PACKET Hello World (Requires DPDK)
add_executable(hello_world_fullpkt hello_world_fullpkt.c)
target_compile_options(hello_world_fullpkt PRIVATE ${DPDK_CFLAGS})
target_link_libraries(hello_world_fullpkt ${DPDK_LDFLAGS} m pthread)
