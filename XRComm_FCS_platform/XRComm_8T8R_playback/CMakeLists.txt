cmake_minimum_required(VERSION 3.10)
project(xrcomm_playback C)

find_package(PkgConfig REQUIRED)

# --- Required dependencies ---
pkg_check_modules(DPDK    REQUIRED libdpdk)
pkg_check_modules(OPENSSL REQUIRED openssl)
pkg_check_modules(UUID    REQUIRED uuid)
pkg_check_modules(ISAL    REQUIRED libisal)

pkg_check_modules(FUSE fuse3)
if(NOT FUSE_FOUND)
    pkg_check_modules(FUSE REQUIRED fuse)
endif()

# --- Optional NVMe / SPDK support ---
# Build with NVMe enabled:   cmake -DENABLE_NVME=ON ..
# Build without (default):   cmake ..
option(ENABLE_NVME "Enable NVMe streaming via SPDK" OFF)

if(ENABLE_NVME)
    pkg_check_modules(SPDK REQUIRED
        spdk_nvme spdk_env_dpdk spdk_sock spdk_sock_posix spdk_util)
    message(STATUS "NVMe/SPDK support: ENABLED")
else()
    message(STATUS "NVMe/SPDK support: DISABLED (file playback only)")
endif()

# --- Executable ---
add_executable(xrcomm_playback
    src/main.c
    src/reader.c
    src/file_reader.c
    src/protocol.c
    src/tx.c
    src/stats.c
    src/config.c
)

target_include_directories(xrcomm_playback PRIVATE src)

# --- Link libraries ---
# SPDK must be wrapped in --whole-archive so its constructor-based
# registration callbacks are not stripped by the linker.
if(ENABLE_NVME)
    target_link_libraries(xrcomm_playback PRIVATE
        -Wl,--whole-archive
        ${SPDK_LDFLAGS}
        ${DPDK_LDFLAGS}
        -Wl,--no-whole-archive
        ${OPENSSL_LIBRARIES}
        ${UUID_LIBRARIES}
        ${ISAL_LIBRARIES}
        ${FUSE_LIBRARIES}
        numa dl m pthread rt
    )
    target_compile_definitions(xrcomm_playback PRIVATE XRCOMM_NVME_ENABLED)
else()
    target_link_libraries(xrcomm_playback PRIVATE
        -Wl,--whole-archive
        ${DPDK_LDFLAGS}
        -Wl,--no-whole-archive
        ${OPENSSL_LIBRARIES}
        ${UUID_LIBRARIES}
        ${ISAL_LIBRARIES}
        ${FUSE_LIBRARIES}
        numa dl m pthread rt
    )
endif()

target_compile_options(xrcomm_playback PRIVATE -march=native -O3)