# ─── Dear ImGui (vendored) ──────────────────────────────────────────────
set(IMGUI_DIR "${CMAKE_SOURCE_DIR}/third_party/imgui")
set(IMGUI_SOURCES
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_demo.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/backends/imgui_impl_dx11.cpp
    ${IMGUI_DIR}/backends/imgui_impl_win32.cpp
)

# Client binary
add_executable(cc_client
    main.cpp
    decode/d3d11_decoder.cpp
    render/renderer.cpp
    render/overlay.cpp
    input/input_capture.cpp
    audio/audio_player.cpp
    client_session.cpp
    ${IMGUI_SOURCES}
)

target_include_directories(cc_client PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${IMGUI_DIR}
    ${IMGUI_DIR}/backends
)

# FFmpeg (for D3D11VA decode)
# Check multiple locations:
# 1. Bundled in third_party/ffmpeg
# 2. FFMPEG_DIR environment variable
# 3. vcpkg
find_path(FFMPEG_INCLUDE_DIR libavcodec/avcodec.h
    HINTS
        "${CMAKE_SOURCE_DIR}/third_party/ffmpeg/include"
        "$ENV{FFMPEG_DIR}/include"
)
find_library(AVCODEC_LIB avcodec
    HINTS
        "${CMAKE_SOURCE_DIR}/third_party/ffmpeg/lib"
        "$ENV{FFMPEG_DIR}/lib"
)
find_library(AVUTIL_LIB avutil
    HINTS
        "${CMAKE_SOURCE_DIR}/third_party/ffmpeg/lib"
        "$ENV{FFMPEG_DIR}/lib"
)

if(FFMPEG_INCLUDE_DIR AND AVCODEC_LIB AND AVUTIL_LIB)
    message(STATUS "Found FFmpeg: ${FFMPEG_INCLUDE_DIR}")
    target_include_directories(cc_client PRIVATE ${FFMPEG_INCLUDE_DIR})
    target_link_libraries(cc_client PRIVATE ${AVCODEC_LIB} ${AVUTIL_LIB})
    target_compile_definitions(cc_client PRIVATE HAS_FFMPEG=1)
else()
    message(WARNING "FFmpeg not found. Decoder/Renderer will compile as stubs. "
                    "Set FFMPEG_DIR or install via: vcpkg install ffmpeg[avcodec,avutil]:x64-windows")
endif()

target_link_libraries(cc_client PRIVATE
    cc_common
    d3d11
    dxgi
    d3dcompiler
)
