# Client binary
add_executable(cc_client
    main.cpp
    decode/d3d11_decoder.cpp
    render/renderer.cpp
    input/input_capture.cpp
    audio/audio_player.cpp
    client_session.cpp
)

target_include_directories(cc_client PRIVATE
    ${CMAKE_SOURCE_DIR}/include
)

# FFmpeg (for D3D11VA decode)
# User must have FFmpeg dev libraries available
# Typical: vcpkg install ffmpeg[avcodec,avutil,swscale]:x64-windows
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
else()
    message(WARNING "FFmpeg not found. Set FFMPEG_DIR or install via vcpkg. "
                    "Client will not build until FFmpeg is available.")
endif()

target_link_libraries(cc_client PRIVATE
    cc_common
    d3d11
    dxgi
    d3dcompiler
)
