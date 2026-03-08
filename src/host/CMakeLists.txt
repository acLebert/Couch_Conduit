# Host binary
add_executable(cc_host
    main.cpp
    capture/dxgi_capture.cpp
    encode/nvenc_encoder.cpp
    input/input_injector.cpp
    audio/wasapi_capture.cpp
    host_session.cpp
)

target_include_directories(cc_host PRIVATE
    ${CMAKE_SOURCE_DIR}/include
)

# NVENC SDK headers: check multiple locations
# 1. Bundled nv-codec-headers (third_party)
# 2. NVCODEC_SDK_DIR environment/cmake variable
# 3. System CUDA toolkit
set(_NVENC_FOUND FALSE)

# Check bundled nv-codec-headers first (preferred — no external dependency)
if(EXISTS "${CMAKE_SOURCE_DIR}/third_party/nv-codec-headers/include/ffnvcodec/nvEncodeAPI.h")
    target_include_directories(cc_host PRIVATE
        "${CMAKE_SOURCE_DIR}/third_party/nv-codec-headers/include/ffnvcodec"
    )
    set(_NVENC_FOUND TRUE)
    message(STATUS "Found NVENC headers: bundled nv-codec-headers")
# Then try NVENC SDK
elseif(EXISTS "${NVCODEC_SDK_DIR}/Interface/nvEncodeAPI.h")
    target_include_directories(cc_host PRIVATE "${NVCODEC_SDK_DIR}/Interface")
    set(_NVENC_FOUND TRUE)
    message(STATUS "Found NVENC headers: ${NVCODEC_SDK_DIR}")
endif()

if(_NVENC_FOUND)
    target_compile_definitions(cc_host PRIVATE HAS_NVENC=1)
else()
    message(WARNING "NVENC headers not found. Encoder will compile as stub. "
                    "Run: git clone --depth 1 https://github.com/FFmpeg/nv-codec-headers.git third_party/nv-codec-headers")
endif()

target_link_libraries(cc_host PRIVATE
    cc_common
    d3d11       # Direct3D 11
    dxgi        # DXGI
    d3dcompiler # Shader compilation (for renderer)
)
