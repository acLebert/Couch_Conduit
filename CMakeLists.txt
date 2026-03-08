cmake_minimum_required(VERSION 3.24)
project(CouchConduit VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ─── Options ────────────────────────────────────────────────────────────
option(CC_BUILD_HOST   "Build the host (server) binary"  ON)
option(CC_BUILD_CLIENT "Build the client (guest) binary" ON)
option(CC_ASAN         "Enable AddressSanitizer"         OFF)

# ─── Platform checks ───────────────────────────────────────────────────
if(NOT WIN32)
    message(FATAL_ERROR "Couch Conduit currently targets Windows only")
endif()

# ─── Compiler flags ────────────────────────────────────────────────────
add_compile_options(
    /W4 /WX-          # Warnings level 4, don't error on warnings yet
    /MP                # Multi-process compilation
    /permissive-       # Strict conformance
    /Zc:__cplusplus    # Correct __cplusplus macro
    /utf-8             # Source and execution charset
)

if(CMAKE_BUILD_TYPE STREQUAL "Release" OR CMAKE_BUILD_TYPE STREQUAL "RelWithDebInfo")
    add_compile_options(/O2 /Ob3 /GL)  # Full optimization, LTCG
    add_link_options(/LTCG /OPT:REF /OPT:ICF)
endif()

if(CC_ASAN)
    add_compile_options(/fsanitize=address)
endif()

# ─── Find system packages ──────────────────────────────────────────────
# NVIDIA Video Codec SDK (NVENC/NVDEC)
# User must set NVCODEC_SDK_DIR or have it in PATH
if(DEFINED ENV{NVCODEC_SDK_DIR})
    set(NVCODEC_SDK_DIR "$ENV{NVCODEC_SDK_DIR}")
elseif(NOT DEFINED NVCODEC_SDK_DIR)
    # Try common install location
    set(NVCODEC_SDK_DIR "C:/Program Files/NVIDIA GPU Computing Toolkit/VideoCodecSDK")
endif()

if(EXISTS "${NVCODEC_SDK_DIR}/Interface")
    message(STATUS "Found NVIDIA Video Codec SDK: ${NVCODEC_SDK_DIR}")
else()
    message(WARNING "NVIDIA Video Codec SDK not found at ${NVCODEC_SDK_DIR}. "
                    "Set NVCODEC_SDK_DIR to the SDK root directory. "
                    "Download from https://developer.nvidia.com/nvidia-video-codec-sdk")
endif()

# ─── Common library (shared by host and client) ────────────────────────
add_subdirectory(src/common)

# ─── Host (server) ─────────────────────────────────────────────────────
if(CC_BUILD_HOST)
    add_subdirectory(src/host)
endif()

# ─── Client (guest) ────────────────────────────────────────────────────
if(CC_BUILD_CLIENT)
    add_subdirectory(src/client)
endif()
