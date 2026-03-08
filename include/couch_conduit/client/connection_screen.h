#pragma once
// Couch Conduit — Connection Screen
//
// Shown when the client starts without a --host argument.
// Renders a D3D11 + Dear ImGui UI for entering a room code or direct IP.
// Blocks until the user connects or closes the window.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

#include <string>
#include <cstdint>

namespace cc::client {

struct ConnectionResult {
    bool        connected    = false;  // True if user chose to connect
    bool        cancelled    = false;  // True if user closed the window
    std::string hostAddr;              // Resolved host IP
    uint16_t    controlPort  = 47100;  // TCP control port
};

/// Shows the connection screen.  Blocks until the user clicks Connect
/// or closes the window.  Creates a temporary D3D11 + ImGui context.
///
/// @param hwnd           Window to render in
/// @param width          Client area width
/// @param height         Client area height
/// @param signalingUrl   Signaling server URL (empty = room codes disabled)
ConnectionResult ShowConnectionScreen(HWND hwnd,
                                      uint32_t width, uint32_t height,
                                      const std::string& signalingUrl = "");

}  // namespace cc::client
