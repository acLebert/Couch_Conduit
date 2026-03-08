# Common library — shared between host and client
add_library(cc_common STATIC
    transport/udp_socket.cpp
    transport/video_sender.cpp
    transport/video_receiver.cpp
    transport/input_sender.cpp
    transport/input_receiver.cpp
    protocol/session.cpp
    crypto/aes_gcm.cpp
    util/system_tuning.cpp
)

target_include_directories(cc_common PUBLIC
    ${CMAKE_SOURCE_DIR}/include
)

target_link_libraries(cc_common PUBLIC
    ws2_32      # Winsock
    dwmapi      # DWM
    avrt        # MMCSS
    gdi32       # D3DKMT
    bcrypt      # AES-GCM (CNG)
)

target_compile_definitions(cc_common PUBLIC
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    _CRT_SECURE_NO_WARNINGS
    UNICODE
    _UNICODE
)
