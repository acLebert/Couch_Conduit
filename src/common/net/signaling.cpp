// Couch Conduit — Signaling Client (WinHTTP)
//
// HTTP client for the signaling server.  Uses WinHTTP which is built
// into Windows — no external dependencies beyond ws2_32 (already linked).

#include <couch_conduit/common/signaling.h>
#include <couch_conduit/common/log.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <winhttp.h>
#include <WinSock2.h>
#include <WS2tcpip.h>

#pragma comment(lib, "winhttp.lib")

#include <string>
#include <algorithm>

namespace {

// ─── Minimal JSON helpers (we control server response format) ──────────

std::string ExtractJsonString(const std::string& json, const std::string& key) {
    std::string needle = "\"" + key + "\":\"";
    auto pos = json.find(needle);
    if (pos == std::string::npos) return "";
    pos += needle.size();
    auto end = json.find('"', pos);
    if (end == std::string::npos) return "";
    return json.substr(pos, end - pos);
}

int ExtractJsonInt(const std::string& json, const std::string& key) {
    std::string needle = "\"" + key + "\":";
    auto pos = json.find(needle);
    if (pos == std::string::npos) return 0;
    pos += needle.size();
    while (pos < json.size() && json[pos] == ' ') ++pos;
    std::string num;
    while (pos < json.size() && json[pos] >= '0' && json[pos] <= '9')
        num += json[pos++];
    return num.empty() ? 0 : std::stoi(num);
}

// ─── URL parser ────────────────────────────────────────────────────────

struct UrlParts {
    bool        https = true;
    std::wstring host;
    uint16_t    port  = 443;
    std::wstring path;
};

UrlParts ParseUrl(const std::string& url) {
    UrlParts p;
    std::string u = url;

    if (u.rfind("https://", 0) == 0) { p.https = true;  p.port = 443; u = u.substr(8); }
    else if (u.rfind("http://", 0) == 0) { p.https = false; p.port = 80;  u = u.substr(7); }

    auto slash = u.find('/');
    std::string hostPart = (slash != std::string::npos) ? u.substr(0, slash) : u;
    std::string pathPart = (slash != std::string::npos) ? u.substr(slash)    : "/";

    auto colon = hostPart.find(':');
    if (colon != std::string::npos) {
        p.port = static_cast<uint16_t>(std::stoi(hostPart.substr(colon + 1)));
        hostPart = hostPart.substr(0, colon);
    }

    p.host.assign(hostPart.begin(), hostPart.end());
    p.path.assign(pathPart.begin(), pathPart.end());
    return p;
}

// ─── HTTP request via WinHTTP ──────────────────────────────────────────

std::string HttpRequest(const std::string& url,
                        const std::string& method,
                        const std::string& body = "",
                        int timeoutMs = 5000) {
    auto parts = ParseUrl(url);

    HINTERNET hSession = WinHttpOpen(
        L"CouchConduit/0.1",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) return "";

    WinHttpSetTimeouts(hSession, timeoutMs, timeoutMs, timeoutMs, timeoutMs);

    HINTERNET hConnect = WinHttpConnect(hSession, parts.host.c_str(), parts.port, 0);
    if (!hConnect) { WinHttpCloseHandle(hSession); return ""; }

    std::wstring wMethod(method.begin(), method.end());
    DWORD flags = parts.https ? WINHTTP_FLAG_SECURE : 0;

    HINTERNET hRequest = WinHttpOpenRequest(
        hConnect, wMethod.c_str(), parts.path.c_str(),
        nullptr, WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!hRequest) {
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return "";
    }

    // Send
    BOOL sent;
    if (!body.empty()) {
        const wchar_t* ct = L"Content-Type: application/json\r\n";
        sent = WinHttpSendRequest(hRequest, ct, (DWORD)-1L,
                                  (LPVOID)body.c_str(), (DWORD)body.size(),
                                  (DWORD)body.size(), 0);
    } else {
        sent = WinHttpSendRequest(hRequest,
                                  WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                  WINHTTP_NO_REQUEST_DATA, 0, 0, 0);
    }

    if (!sent || !WinHttpReceiveResponse(hRequest, nullptr)) {
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return "";
    }

    // Status code
    DWORD statusCode = 0, sz = sizeof(statusCode);
    WinHttpQueryHeaders(hRequest,
                        WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX,
                        &statusCode, &sz, WINHTTP_NO_HEADER_INDEX);

    // Read body
    std::string response;
    DWORD avail = 0;
    while (WinHttpQueryDataAvailable(hRequest, &avail) && avail > 0) {
        std::string chunk(avail, '\0');
        DWORD bytesRead = 0;
        WinHttpReadData(hRequest, chunk.data(), avail, &bytesRead);
        response.append(chunk.data(), bytesRead);
    }

    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);

    if (statusCode >= 400) {
        CC_WARN("HTTP %s %s → %u: %s",
                method.c_str(), url.c_str(), statusCode, response.c_str());
        return "";
    }

    return response;
}

}  // anonymous namespace

// ═══════════════════════════════════════════════════════════════════════
namespace cc::net {

bool SignalingClient::CreateRoom(const std::string& serverUrl,
                                 const std::string& hostIp,
                                 uint16_t hostPort,
                                 std::string& outCode) {
    std::string body = "{\"host_ip\":\"" + hostIp +
                       "\",\"host_port\":" + std::to_string(hostPort) + "}";
    std::string url = serverUrl + "/api/rooms";

    CC_INFO("Registering room at %s ...", url.c_str());
    std::string resp = HttpRequest(url, "POST", body);
    if (resp.empty()) { CC_WARN("Signaling: failed to create room"); return false; }

    outCode = ExtractJsonString(resp, "code");
    if (outCode.empty()) { CC_WARN("Signaling: no code in response"); return false; }

    CC_INFO("Room created: %s", outCode.c_str());
    return true;
}

bool SignalingClient::ResolveRoom(const std::string& serverUrl,
                                   const std::string& code,
                                   std::string& outIp,
                                   uint16_t& outPort) {
    std::string url = serverUrl + "/api/rooms/" + code;

    CC_INFO("Resolving room %s ...", code.c_str());
    std::string resp = HttpRequest(url, "GET");
    if (resp.empty()) { CC_WARN("Room '%s' not found", code.c_str()); return false; }

    outIp   = ExtractJsonString(resp, "host_ip");
    outPort = static_cast<uint16_t>(ExtractJsonInt(resp, "host_port"));

    if (outIp.empty() || outPort == 0) {
        CC_WARN("Invalid room data for '%s'", code.c_str());
        return false;
    }

    CC_INFO("Room %s → %s:%u", code.c_str(), outIp.c_str(), outPort);
    return true;
}

bool SignalingClient::DeleteRoom(const std::string& serverUrl,
                                  const std::string& code) {
    std::string url = serverUrl + "/api/rooms/" + code;
    CC_INFO("Deleting room %s ...", code.c_str());
    return !HttpRequest(url, "DELETE").empty();
}

bool SignalingClient::HealthCheck(const std::string& serverUrl) {
    return !HttpRequest(serverUrl + "/health", "GET", "", 3000).empty();
}

std::string SignalingClient::GetLocalIp() {
    char hostname[256] = {};
    if (gethostname(hostname, sizeof(hostname)) != 0) return "127.0.0.1";

    struct addrinfo hints = {};
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    struct addrinfo* result = nullptr;
    if (getaddrinfo(hostname, nullptr, &hints, &result) != 0 || !result)
        return "127.0.0.1";

    char ip[64] = {};
    auto* addr = reinterpret_cast<struct sockaddr_in*>(result->ai_addr);
    inet_ntop(AF_INET, &addr->sin_addr, ip, sizeof(ip));
    freeaddrinfo(result);

    std::string ipStr(ip);
    return (ipStr == "0.0.0.0") ? "127.0.0.1" : ipStr;
}

}  // namespace cc::net
