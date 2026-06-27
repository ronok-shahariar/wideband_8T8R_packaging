cmake_minimum_required(VERSION 3.14)
project(xrcomm_8t8r_hello_world C)

# Keep the package's documented binary location stable.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/bin)

# Enable required C standards
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_executable(hello_world hello_world.c)
target_include_directories(hello_world PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../platform/XRComm_platform_drivers/include)
target_compile_options(hello_world PRIVATE -O3 -mavx2 -Wall)
target_link_libraries(hello_world PRIVATE rt pthread m)
