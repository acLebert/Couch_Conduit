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

# NVENC SDK headers (if available)
if(EXISTS "${NVCODEC_SDK_DIR}/Interface")
    target_include_directories(cc_host PRIVATE "${NVCODEC_SDK_DIR}/Interface")
endif()

target_link_libraries(cc_host PRIVATE
    cc_common
    d3d11       # Direct3D 11
    dxgi        # DXGI
    d3dcompiler # Shader compilation (for renderer)
)
