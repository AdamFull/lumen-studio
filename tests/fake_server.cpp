// Stand-in for llama-server / sd-server in supervisor and installer tests. No Qt, linked
// statically, so it runs from any folder (it gets copied into fake runtime installs).
//
//   --version                 print a version line and exit
//   --port N / --listen-port N listen on 127.0.0.1:N
//   --fake-ready-ms M         /health answers 503 for M ms, then 200 (default 300)
//   --fake-exit-after-ms M    exit with code 3 after M ms
//   --fake-child              start a sleeping child process and print "child <pid>"
//   --fake-sleep              sleep forever (the child)
//   --fake-proxy P            once ready, forward every request to 127.0.0.1:P (a MockHttpServer
//                             in the test) and relay its answer
// Unknown arguments (the real servers' options) are ignored.

#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

#ifdef _WIN32
#include <winsock2.h>
#include <windows.h>
using socket_t = SOCKET;
#define CLOSESOCK closesocket
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
using socket_t = int;
#define INVALID_SOCKET (-1)
#define CLOSESOCK close
#endif

namespace {

// One whole HTTP request: the headers, then Content-Length bytes of body.
std::string readRequest(socket_t c)
{
    std::string data;
    char buf[65536];
    size_t need = std::string::npos;
    for (;;) {
        if (need != std::string::npos && data.size() >= need)
            return data;
        const int n = recv(c, buf, sizeof(buf), 0);
        if (n <= 0)
            return data;
        data.append(buf, size_t(n));
        const size_t end = data.find("\r\n\r\n");
        if (need == std::string::npos && end != std::string::npos) {
            size_t length = 0;
            std::string headers = data.substr(0, end);
            for (char &ch : headers)
                ch = char(std::tolower(static_cast<unsigned char>(ch)));
            const size_t at = headers.find("content-length:");
            if (at != std::string::npos)
                length = size_t(std::strtoul(headers.c_str() + at + 15, nullptr, 10));
            need = end + 4 + length;
        }
    }
}

// Sends `request` to 127.0.0.1:port with "Connection: close" and returns everything it answers.
std::string forward(int port, std::string request)
{
    const size_t end = request.find("\r\n\r\n");
    if (end != std::string::npos)
        request.insert(end, "\r\nConnection: close");
    socket_t s = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<unsigned short>(port));
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (s == INVALID_SOCKET || connect(s, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0)
        return "HTTP/1.1 502 Bad Gateway\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
    for (size_t sent = 0; sent < request.size();) {
        const int n = send(s, request.data() + sent, int(request.size() - sent), 0);
        if (n <= 0)
            break;
        sent += size_t(n);
    }
    std::string answer;
    char buf[65536];
    for (;;) {
        const int n = recv(s, buf, sizeof(buf), 0);
        if (n <= 0)
            break;
        answer.append(buf, size_t(n));
    }
    CLOSESOCK(s);
    return answer;
}

} // namespace

int main(int argc, char **argv)
{
    int port = 0;
    long readyMs = 300;
    long exitAfterMs = -1;
    bool child = false;
    int proxyPort = 0;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&](long def) { return i + 1 < argc ? std::strtol(argv[++i], nullptr, 10) : def; };
        if (a == "--version") {
            std::printf("ggml_cuda_init: no devices\nversion: 42 (fake)\nbuilt with test\n");
            return 0;
        } else if (a == "--help") {
            std::printf("usage: fake\n  -m,    --model FNAME   model path\n  --models-preset PATH    presets\n  --port PORT\n");
            return 0;
        } else if (a == "--port" || a == "--listen-port") {
            port = int(next(0));
        } else if (a == "--fake-ready-ms") {
            readyMs = next(300);
        } else if (a == "--fake-exit-after-ms") {
            exitAfterMs = next(-1);
        } else if (a == "--fake-proxy") {
            proxyPort = int(next(0));
        } else if (a == "--fake-child") {
            child = true;
        } else if (a == "--fake-sleep") {
            for (;;)
                std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    }
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("fake server starting\n");

#ifdef _WIN32
    if (child) {
        char exe[MAX_PATH];
        GetModuleFileNameA(nullptr, exe, MAX_PATH);
        std::string cmd = std::string("\"") + exe + "\" --fake-sleep";
        STARTUPINFOA si = {};
        si.cb = sizeof(si);
        PROCESS_INFORMATION pi = {};
        if (CreateProcessA(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
            std::printf("child %lu\n", static_cast<unsigned long>(pi.dwProcessId));
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
        }
    }
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#else
    (void)child;
#endif
    socket_t srv = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<unsigned short>(port));
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (srv == INVALID_SOCKET || bind(srv, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0 || listen(srv, 16) != 0) {
        std::printf("error: cannot listen on port %d\n", port);
        return 2;
    }
    const auto start = std::chrono::steady_clock::now();
    if (exitAfterMs >= 0) {
        std::thread([exitAfterMs] {
            std::this_thread::sleep_for(std::chrono::milliseconds(exitAfterMs));
            std::printf("fake crash\n");
            std::_Exit(3);
        }).detach();
    }
    long unloadAt = -1;
    for (int i = 0; i < 3; ++i)
        std::printf("load_tensors: loading layer %d\n", i);
    for (;;) {
        socket_t c = accept(srv, nullptr, nullptr);
        if (c == INVALID_SOCKET)
            continue;
        const std::string request = readRequest(c);
        const long elapsed = long(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count());
        const bool ready = elapsed >= readyMs;
        if (ready && proxyPort > 0) {
            const std::string answer = forward(proxyPort, request);
            for (size_t sent = 0; sent < answer.size();) {
                const int n = send(c, answer.data() + sent, int(answer.size() - sent), 0);
                if (n <= 0)
                    break;
                sent += size_t(n);
            }
            CLOSESOCK(c);
            continue;
        }
        std::string body = ready ? "{\"status\":\"ok\"}" : "{\"error\":\"loading\"}";
        if (request.rfind("POST /models/unload", 0) == 0) {
            unloadAt = elapsed + 300;
            body = "{\"success\":true}";
        } else if (request.rfind("GET /models", 0) == 0) {
            const bool loaded = unloadAt < 0 || elapsed < unloadAt;
            body = std::string("{\"data\":[{\"id\":\"fake-model\",\"status\":{\"value\":\"") + (loaded ? "loaded" : "unloaded") + "\"}}]}";
        }
        const std::string resp = std::string("HTTP/1.1 ") + (ready ? "200 OK" : "503 Service Unavailable")
                                 + "\r\nContent-Type: application/json\r\nConnection: close\r\nContent-Length: "
                                 + std::to_string(body.size()) + "\r\n\r\n" + body;
        send(c, resp.data(), int(resp.size()), 0);
        CLOSESOCK(c);
    }
}
