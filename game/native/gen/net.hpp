// HTTP for the sources the world is built from: Overpass, IGN, Open-Meteo.
//
// One blocking call, made from a worker thread, never from the frame. The
// desktop build answers it with WinHTTP; a web build answers it with the
// browser's fetch (every one of these services allows cross-origin calls).
#pragma once

#include <map>
#include <stdexcept>
#include <string>

namespace r1::net {

struct Response {
    int status = 0;
    std::string body;
    std::map<std::string, std::string> headers;  // lower-case names
};

// A failure to reach the server at all. `offline` means the machine has no
// route to anywhere (no DNS, no network), which no retry will fix.
class Unreachable : public std::runtime_error {
public:
    Unreachable(const std::string& what, bool offline) : std::runtime_error(what), offline(offline) {}
    bool offline;
};

// Throws Unreachable; any HTTP status, success or not, is a Response.
Response request(const std::string& method, const std::string& url, const std::string& body = {},
                 const std::string& contentType = {}, double timeoutSeconds = 120.0);

// A JSON request retried while the failure is "not now" (429, 503...) rather
// than "no". Throws Unreachable, or
// std::runtime_error naming the status.
std::string requestJson(const std::string& url, const std::string& body = {}, const std::string& contentType = {},
                        double timeoutSeconds = 120.0, int attempts = 3);

std::string urlEncode(const std::string& text);

// One websocket subscription (aisstream.io's live positions): a text message
// out, text messages in. Honours the proxy variables like `request`.
class WebSocket {
public:
    explicit WebSocket(const std::string& url, double timeoutSeconds = 20.0);  // throws Unreachable
    ~WebSocket();
    WebSocket(const WebSocket&) = delete;
    WebSocket& operator=(const WebSocket&) = delete;
    void send(const std::string& text);
    // The next message; empty with `closed` set when the server closed.
    std::string receive(bool& closed);

private:
    void* session_ = nullptr;
    void* connection_ = nullptr;
    void* socket_ = nullptr;
};

}  // namespace r1::net
