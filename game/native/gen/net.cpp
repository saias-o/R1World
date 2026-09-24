#include "net.hpp"

#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <winhttp.h>
#endif

namespace r1::net {

namespace {
#ifdef _WIN32
std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
    std::wstring out(size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), out.data(), n);
    return out;
}

std::string narrow(const std::wstring& s) {
    if (s.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0, nullptr, nullptr);
    std::string out(size_t(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.data(), int(s.size()), out.data(), n, nullptr, nullptr);
    return out;
}

struct Handle {
    HINTERNET h = nullptr;
    explicit Handle(HINTERNET handle) : h(handle) {}
    ~Handle() { if (h) WinHttpCloseHandle(h); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
};

[[noreturn]] void fail(const char* step) {
    const DWORD code = GetLastError();
    // No route at all: no name resolution, nothing listening where we must
    // go (the offline test points the proxy at a closed port on purpose).
    const bool offline = code == ERROR_WINHTTP_NAME_NOT_RESOLVED || code == ERROR_WINHTTP_CANNOT_CONNECT ||
                         code == ERROR_WINHTTP_CONNECTION_ERROR;
    throw Unreachable(std::string(step) + " failed (WinHTTP error " + std::to_string(code) + ")", offline);
}

// A session honouring the proxy variables, as Python's urllib did: the
// offline test (CLAUDE.md §6) is a proxy on a closed port.
HINTERNET openSession(double timeoutSeconds) {
    const char* proxyEnv = std::getenv("HTTPS_PROXY");
    if (!proxyEnv || !*proxyEnv) proxyEnv = std::getenv("HTTP_PROXY");
    std::wstring proxy;
    if (proxyEnv && *proxyEnv) {
        std::string p = proxyEnv;
        const auto scheme = p.find("://");
        if (scheme != std::string::npos) p = p.substr(scheme + 3);
        while (!p.empty() && p.back() == '/') p.pop_back();
        proxy = widen(p);
    }
    HINTERNET session = WinHttpOpen(L"R1World/2.0",
                                    proxy.empty() ? WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY : WINHTTP_ACCESS_TYPE_NAMED_PROXY,
                                    proxy.empty() ? WINHTTP_NO_PROXY_NAME : proxy.c_str(), WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) fail("WinHttpOpen");
    // A server that has not accepted the connection in ten seconds is down:
    // waiting the whole answer's timeout for it was minutes per query while
    // one Overpass mirror was unreachable. The answer itself keeps its time.
    const int ms = int(timeoutSeconds * 1000), connect = std::min(ms, 10000);
    WinHttpSetTimeouts(session, connect, connect, ms, ms);
    return session;
}

// Host, port, path and scheme of a URL.
struct Target {
    std::wstring host, path;
    INTERNET_PORT port = 0;
    bool secure = false;
};

Target crack(const std::string& url) {
    const std::wstring wide = widen(url);
    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof parts;
    wchar_t host[256], path[8192], extra[8192];
    parts.lpszHostName = host; parts.dwHostNameLength = 256;
    parts.lpszUrlPath = path; parts.dwUrlPathLength = 8192;
    parts.lpszExtraInfo = extra; parts.dwExtraInfoLength = 8192;
    if (!WinHttpCrackUrl(wide.c_str(), 0, 0, &parts)) fail("WinHttpCrackUrl");
    return {std::wstring(host, parts.dwHostNameLength),
            std::wstring(path, parts.dwUrlPathLength) + std::wstring(extra, parts.dwExtraInfoLength), parts.nPort,
            parts.nScheme == INTERNET_SCHEME_HTTPS};
}

std::map<std::string, std::string> headersOf(HINTERNET request) {
    std::map<std::string, std::string> out;
    DWORD length = 0;
    WinHttpQueryHeaders(request, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX, nullptr, &length,
                        WINHTTP_NO_HEADER_INDEX);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || !length) return out;
    std::wstring raw(length / sizeof(wchar_t), L'\0');
    if (!WinHttpQueryHeaders(request, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX, raw.data(), &length,
                             WINHTTP_NO_HEADER_INDEX))
        return out;
    const std::string text = narrow(raw);
    for (size_t start = 0; start < text.size();) {
        size_t end = text.find("\r\n", start);
        if (end == std::string::npos) end = text.size();
        const std::string line = text.substr(start, end - start);
        const auto colon = line.find(':');
        if (colon != std::string::npos) {
            std::string name = line.substr(0, colon);
            for (char& c : name) c = char(std::tolower((unsigned char)c));
            std::string value = line.substr(colon + 1);
            while (!value.empty() && value.front() == ' ') value.erase(value.begin());
            out[name] = value;
        }
        start = end + 2;
    }
    return out;
}
#endif
}  // namespace

std::string urlEncode(const std::string& text) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : text) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') out += char(c);
        else if (c == ' ') out += '+';
        else { out += '%'; out += hex[c >> 4]; out += hex[c & 15]; }
    }
    return out;
}

Response request(const std::string& method, const std::string& url, const std::string& body,
                 const std::string& contentType, double timeoutSeconds) {
#ifdef _WIN32
    Handle session(openSession(timeoutSeconds));
    const Target target = crack(url);
    Handle connection(WinHttpConnect(session.h, target.host.c_str(), target.port, 0));
    if (!connection.h) fail("WinHttpConnect");
    Handle req(WinHttpOpenRequest(connection.h, widen(method).c_str(), target.path.c_str(), nullptr, WINHTTP_NO_REFERER,
                                  WINHTTP_DEFAULT_ACCEPT_TYPES, target.secure ? WINHTTP_FLAG_SECURE : 0));
    if (!req.h) fail("WinHttpOpenRequest");
    DWORD decompress = WINHTTP_DECOMPRESSION_FLAG_ALL;
    WinHttpSetOption(req.h, WINHTTP_OPTION_DECOMPRESSION, &decompress, sizeof decompress);
    std::wstring headers = L"Accept: application/json\r\n";
    if (!contentType.empty()) headers += L"Content-Type: " + widen(contentType) + L"\r\n";
    if (!WinHttpSendRequest(req.h, headers.c_str(), DWORD(-1L), body.empty() ? nullptr : (void*)body.data(),
                            DWORD(body.size()), DWORD(body.size()), 0))
        fail("sending the request");
    if (!WinHttpReceiveResponse(req.h, nullptr)) fail("receiving the response");
    Response out;
    DWORD status = 0, size = sizeof status;
    WinHttpQueryHeaders(req.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                        &status, &size, WINHTTP_NO_HEADER_INDEX);
    out.status = int(status);
    out.headers = headersOf(req.h);
    for (;;) {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(req.h, &available)) fail("reading the response");
        if (!available) break;
        std::vector<char> chunk(available);
        DWORD read = 0;
        if (!WinHttpReadData(req.h, chunk.data(), available, &read)) fail("reading the response");
        out.body.append(chunk.data(), read);
    }
    return out;
#else
    (void)method; (void)url; (void)body; (void)contentType; (void)timeoutSeconds;
    throw Unreachable("no HTTP client in this build", true);
#endif
}

std::string requestJson(const std::string& url, const std::string& body, const std::string& contentType,
                        double timeoutSeconds, int attempts) {
    static const int retryable[] = {408, 425, 429, 500, 502, 503, 504};
    double delay = 2.0;
    for (int attempt = 1; attempt <= attempts; ++attempt) {
        double wait = delay;
        try {
            const Response r = request(body.empty() ? "GET" : "POST", url, body, contentType, timeoutSeconds);
            if (r.status >= 200 && r.status < 300) return r.body;
            bool again = false;
            for (int s : retryable) again |= r.status == s;
            if (!again || attempt == attempts) throw std::runtime_error("HTTP Error " + std::to_string(r.status));
            // A server asking for longer than twenty seconds is one to leave
            // for the next endpoint rather than to wait for.
            auto it = r.headers.find("retry-after");
            if (it != r.headers.end()) {
                char* end = nullptr;
                const double seconds = std::strtod(it->second.c_str(), &end);
                if (end != it->second.c_str() && std::isfinite(seconds) && seconds >= 0) wait = std::min(seconds, 20.0);
            }
        } catch (const Unreachable& e) {
            if (attempt == attempts || e.offline) throw;
        }
        std::this_thread::sleep_for(std::chrono::duration<double>(wait));
        delay = std::min(delay * 2.0, 20.0);
    }
    throw std::runtime_error("unreachable");
}

WebSocket::WebSocket(const std::string& url, double timeoutSeconds) {
#ifdef _WIN32
    session_ = openSession(timeoutSeconds);
    const Target target = crack(url);
    connection_ = WinHttpConnect(HINTERNET(session_), target.host.c_str(), target.port, 0);
    if (!connection_) fail("WinHttpConnect");
    Handle req(WinHttpOpenRequest(HINTERNET(connection_), L"GET", target.path.c_str(), nullptr, WINHTTP_NO_REFERER,
                                  WINHTTP_DEFAULT_ACCEPT_TYPES, target.secure ? WINHTTP_FLAG_SECURE : 0));
    if (!req.h) fail("WinHttpOpenRequest");
    if (!WinHttpSetOption(req.h, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, nullptr, 0)) fail("asking for a websocket");
    if (!WinHttpSendRequest(req.h, WINHTTP_NO_ADDITIONAL_HEADERS, 0, nullptr, 0, 0, 0)) fail("opening the websocket");
    if (!WinHttpReceiveResponse(req.h, nullptr)) fail("opening the websocket");
    socket_ = WinHttpWebSocketCompleteUpgrade(req.h, 0);
    if (!socket_) fail("the websocket upgrade");
#else
    (void)url; (void)timeoutSeconds;
    throw Unreachable("no websocket client in this build", true);
#endif
}

WebSocket::~WebSocket() {
#ifdef _WIN32
    if (socket_) {
        WinHttpWebSocketClose(HINTERNET(socket_), WINHTTP_WEB_SOCKET_SUCCESS_CLOSE_STATUS, nullptr, 0);
        WinHttpCloseHandle(HINTERNET(socket_));
    }
    if (connection_) WinHttpCloseHandle(HINTERNET(connection_));
    if (session_) WinHttpCloseHandle(HINTERNET(session_));
#endif
}

void WebSocket::send(const std::string& text) {
#ifdef _WIN32
    const DWORD error = WinHttpWebSocketSend(HINTERNET(socket_), WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE,
                                             (PVOID)text.data(), DWORD(text.size()));
    if (error) throw Unreachable("websocket send failed (WinHTTP error " + std::to_string(error) + ")", false);
#else
    (void)text;
#endif
}

std::string WebSocket::receive(bool& closed) {
    closed = false;
    std::string message;
#ifdef _WIN32
    char buffer[16384];
    for (;;) {
        DWORD read = 0;
        WINHTTP_WEB_SOCKET_BUFFER_TYPE type;
        const DWORD error = WinHttpWebSocketReceive(HINTERNET(socket_), buffer, sizeof buffer, &read, &type);
        if (error) throw Unreachable("websocket receive failed (WinHTTP error " + std::to_string(error) + ")", false);
        if (type == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE) { closed = true; return {}; }
        message.append(buffer, read);
        if (type == WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE || type == WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE)
            return message;
    }
#else
    closed = true;
    return message;
#endif
}

}  // namespace r1::net
