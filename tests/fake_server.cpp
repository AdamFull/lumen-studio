// Stand-in for llama-server / sd-server in supervisor and installer tests. No Qt, linked
// statically, so it runs from any folder (it gets copied into fake runtime installs).
//
//   --version                 print a version line and exit
//   --port N / --listen-port N listen on 127.0.0.1:N
//   --fake-ready-ms M         /health answers 503 for M ms, then 200 (default 300)
//   --fake-exit-after-ms M    exit with code 3 after M ms
//   --fake-child              start a sleeping child process and print "child <pid>"
//   --fake-sleep              sleep forever (the child)
// Unknown arguments (the real servers' options) are ignored.

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

int main(int argc, char **argv)
{
    int port = 0;
    long readyMs = 300;
    long exitAfterMs = -1;
    bool child = false;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&](long def) { return i + 1 < argc ? std::strtol(argv[++i], nullptr, 10) : def; };
        if (a == "--version") {
            std::printf("ggml_cuda_init: no devices\nversion: 42 (fake)\nbuilt with test\n");
            return 0;
        } else if (a == "--port" || a == "--listen-port") {
            port = int(next(0));
        } else if (a == "--fake-ready-ms") {
            readyMs = next(300);
        } else if (a == "--fake-exit-after-ms") {
            exitAfterMs = next(-1);
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
    for (int i = 0; i < 3; ++i)
        std::printf("load_tensors: loading layer %d\n", i);
    for (;;) {
        socket_t c = accept(srv, nullptr, nullptr);
        if (c == INVALID_SOCKET)
            continue;
        char buf[2048];
        const int n = recv(c, buf, sizeof(buf) - 1, 0);
        buf[n > 0 ? n : 0] = 0;
        const long elapsed = long(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count());
        const bool ready = elapsed >= readyMs;
        const std::string body = ready ? "{\"status\":\"ok\"}" : "{\"error\":\"loading\"}";
        const std::string resp = std::string("HTTP/1.1 ") + (ready ? "200 OK" : "503 Service Unavailable")
                                 + "\r\nContent-Type: application/json\r\nConnection: close\r\nContent-Length: "
                                 + std::to_string(body.size()) + "\r\n\r\n" + body;
        send(c, resp.data(), int(resp.size()), 0);
        CLOSESOCK(c);
    }
}
