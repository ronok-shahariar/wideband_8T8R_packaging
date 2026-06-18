cmake_minimum_required(VERSION 3.10)
project(xrcomm_8t8r_hello_world C)

# Force the output directory for executables to be the local bin/ folder
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/bin)

# Enable required C standards
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED True)

# Set common flags (matching the optimized zero-drop requirements)
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -O3 -mavx2 -pthread -Wall")

# Include directories
include_directories(../platform/XRComm_platform_drivers/include)

# Standard IQ Hello World for 8T8R
add_executable(hello_world hello_world.c)
target_link_libraries(hello_world m rt pthread)
