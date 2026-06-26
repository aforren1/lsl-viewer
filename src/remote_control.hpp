#pragma once
// Simple TCP remote-control server for recording. A client (telnet/nc/script)
// connects and sends newline-terminated commands; replies are human-readable lines.
//
//   help                      list commands
//   status                    recording? + file/seconds/MB/streams
//   streams                   one resolved stream per line: key | name | type | Nch | rate
//   selected                  the keys currently connected (= what gets recorded)
//   select all|none|k1,k2,..  connect/disconnect streams (recording captures all connected)
//   filename <path>           set the output .xdf path
//   start [path]              begin recording (optional path)
//   stop                      stop recording
//   get [path]                stream a finished recording to the client: a header line
//                             "OK <bytes> <filename>" then <bytes> of raw file data
//   quit                      close the connection
//
// Discovery: while running we also publish an LSL outlet (name "LSLViewerControl",
// type "ViewerControl"); a client resolves it, takes the host from info.hostname() and
// the port from source_id "lsl-viewer-rc:<port>", then connects over TCP. No mDNS.
//
// Threading: the server thread only touches a mutex-guarded RemoteState — it sets
// request fields and reads snapshots that the main loop publishes each frame. The
// main loop owns the Recorder/Discovery and applies the requests, so there are no
// cross-thread races on the recorder. TCP via BSD sockets (Linux/macOS) or Winsock
// (Windows) — the socket layer is abstracted below so the server logic is shared.

#include <lsl_cpp.h>

#include <algorithm>
#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <cstdint>
#include <fstream>
#include <string>
#include <thread>
#include <vector>
#include "thread_compat.hpp"   // jthread / stop_token (with an Apple-libc++ polyfill)

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX                // keep windows.h from defining min/max macros
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

// ---- Thin cross-platform socket layer (Winsock <-> BSD) ---------------------
#if defined(_WIN32)
using rc_socket_t = SOCKET;
static constexpr rc_socket_t RC_INVALID = INVALID_SOCKET;
static constexpr int         RC_SHUT_RDWR = SD_BOTH;
inline int rc_close(rc_socket_t s) { return ::closesocket(s); }
// Winsock needs per-process init; a function-local static does it once, thread-safe,
// and tears it down at exit. Referenced from RemoteControl::start().
struct RcWsaInit { RcWsaInit() { WSADATA w; WSAStartup(MAKEWORD(2, 2), &w); } ~RcWsaInit() { WSACleanup(); } };
#else
using rc_socket_t = int;
static constexpr rc_socket_t RC_INVALID = -1;
static constexpr int         RC_SHUT_RDWR = SHUT_RDWR;
inline int rc_close(rc_socket_t s) { return ::close(s); }
#endif

// Shared between the server thread and the main loop.
struct RemoteState {
    std::mutex mtx;
    // published by the main loop (kept fresh each frame):
    std::string streamsText;          // `streams` reply body
    std::string statusText;           // `status` reply body
    std::string selectedText = "all"; // `selected` reply body
    bool        recording   = false;  // a recording is in progress (can't `get` mid-record)
    std::string lastFile;             // path of the last completed + flushed recording ("" = none)
    // requests from the server, consumed by the main loop:
    std::optional<std::string>               setFilename;
    std::optional<std::vector<std::string>>  setSelection;  // empty vector = none, absent = unchanged; {"*"} = all
    std::vector<std::pair<std::string, std::string>> setVars;  // template fields (subject/task/...)
    bool startReq = false, stopReq = false;
};

class RemoteControl {
public:
    ~RemoteControl() { stop(); }

    bool listening() const { return up_.load(std::memory_order_acquire); }
    int  port()      const { return port_; }
    std::string error() const { std::lock_guard<std::mutex> lk(emtx_); return error_; }

    bool start(int port, RemoteState* state) {
        if (up_) return true;
#if defined(_WIN32)
        static RcWsaInit s_wsa;   // process-lifetime Winsock init on first start()
        (void)s_wsa;
#endif
        st_ = state; port_ = port;
        listenfd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (listenfd_ == RC_INVALID) { setError("socket() failed"); return false; }
        int yes = 1;
        ::setsockopt(listenfd_, SOL_SOCKET, SO_REUSEADDR, (const char*)&yes, sizeof(yes));
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_ANY);   // reachable on the network (lab tool)
        addr.sin_port = htons((uint16_t)port);
        if (::bind(listenfd_, (sockaddr*)&addr, sizeof(addr)) != 0) {
            setError("bind() failed (port in use?)"); rc_close(listenfd_); listenfd_ = RC_INVALID; return false;
        }
        if (::listen(listenfd_, 1) != 0) {
            setError("listen() failed"); rc_close(listenfd_); listenfd_ = RC_INVALID; return false;
        }
        up_.store(true, std::memory_order_release);
        th_ = jthread([this](stop_token s) { serve(s); });
        // LSL announcement so clients DISCOVER the control endpoint without knowing
        // host:port — they resolve type "ViewerControl"; LSL supplies the hostname, and
        // the TCP port is encoded in source_id ("lsl-viewer-rc:<port>") so it's readable
        // from the resolve result alone (desc isn't, without opening an inlet). The
        // outlet stays resolvable while we're up.
        try {
            const std::string sid = "lsl-viewer-rc:" + std::to_string(port);
            lsl::stream_info ai("LSLViewerControl", "ViewerControl", 1,
                                lsl::IRREGULAR_RATE, lsl::cf_string, sid);
            ai.desc().append_child_value("port", std::to_string(port));
            ai.desc().append_child_value("protocol", "tcp-text-lines");
            announce_ = std::make_unique<lsl::stream_outlet>(ai);
        } catch (...) { /* discovery is best-effort */ }
        return true;
    }

    void stop() {
        if (!up_.exchange(false)) return;
        announce_.reset();
        if (listenfd_ != RC_INVALID) { ::shutdown(listenfd_, RC_SHUT_RDWR); rc_close(listenfd_); listenfd_ = RC_INVALID; }
        th_.request_stop();
        if (th_.joinable()) th_.join();
    }

private:
    void setError(const std::string& e) { std::lock_guard<std::mutex> lk(emtx_); error_ = e; }

    void serve(stop_token stoke) {
        while (!stoke.stop_requested() && up_) {
            rc_socket_t fd = ::accept(listenfd_, nullptr, nullptr);
            if (fd == RC_INVALID) break;                    // listenfd closed on stop()
            ::send(fd, kHello, (int)(sizeof(kHello) - 1), 0);
            std::string buf;
            char tmp[1024];
            bool open = true;
            while (open && !stoke.stop_requested()) {
                const int n = (int)::recv(fd, tmp, (int)sizeof(tmp), 0);
                if (n <= 0) break;
                buf.append(tmp, (std::size_t)n);
                std::size_t nl;
                while ((nl = buf.find('\n')) != std::string::npos) {
                    std::string line = buf.substr(0, nl);
                    buf.erase(0, nl + 1);
                    while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
                    if (!dispatch(fd, line)) { open = false; break; }
                }
            }
            rc_close(fd);
        }
    }

    static std::string verb(const std::string& s, std::string& rest) {
        std::size_t sp = s.find(' ');
        if (sp == std::string::npos) { rest.clear(); return s; }
        rest = s.substr(sp + 1);
        while (!rest.empty() && rest.front() == ' ') rest.erase(0, 1);
        return s.substr(0, sp);
    }

    void reply(rc_socket_t fd, const std::string& s) { ::send(fd, s.data(), (int)s.size(), 0); }

    // Stream the last completed recording (or a given path) to the client: a header line
    // "OK <bytes> <filename>\n" followed by exactly <bytes> of raw file data. Blocking, on the
    // server thread (not the recording/UI thread); only the state mutex is held, briefly, to
    // read the path. The client reads the header, then reads <bytes> and saves them.
    void sendFile(rc_socket_t fd, const std::string& arg) {
        std::string path; bool recording;
        { std::lock_guard<std::mutex> lk(st_->mtx);
          recording = st_->recording;
          path      = arg.empty() ? st_->lastFile : arg; }
        if (recording)    { reply(fd, "error: stop the recording before `get`\n"); return; }
        if (path.empty()) { reply(fd, "error: no completed recording yet\n"); return; }
        std::ifstream f(path, std::ios::binary | std::ios::ate);
        if (!f) { reply(fd, "error: cannot open " + path + "\n"); return; }
        const std::uint64_t size = (std::uint64_t)f.tellg();
        f.seekg(0);
        const std::size_t slash = path.find_last_of("/\\");
        reply(fd, "OK " + std::to_string(size) + " " +
                  (slash == std::string::npos ? path : path.substr(slash + 1)) + "\n");
        char buf[65536];
        while (f) {
            f.read(buf, sizeof buf);
            std::size_t off = 0, got = (std::size_t)f.gcount();
            while (off < got) {                          // a send() can be partial
                const int n = (int)::send(fd, buf + off, (int)(got - off), 0);
                if (n <= 0) return;                      // client gone
                off += (std::size_t)n;
            }
        }
    }

    bool dispatch(rc_socket_t fd, const std::string& line) {
        if (line.empty()) return true;
        std::string arg, v = verb(line, arg);
        if (v == "get") { sendFile(fd, arg); return true; }   // binary transfer; locks only briefly
        std::lock_guard<std::mutex> lk(st_->mtx);
        if (v == "help") {
            reply(fd, "commands: help status streams selected select filename set start stop get quit\n"
                      "  filename <template>   e.g. sub-{subject}_task-{task}_run-{run}_eeg.xdf\n"
                      "  set <field> <value>   subject|session|task|run  (also {datetime}/{date}/{time})\n"
                      "  get [path]            stream a finished recording: 'OK <bytes> <name>' + raw data\n");
        } else if (v == "status") {
            reply(fd, st_->statusText + "\n");
        } else if (v == "streams") {
            reply(fd, st_->streamsText);
        } else if (v == "selected") {
            reply(fd, st_->selectedText + "\n");
        } else if (v == "select") {                 // connect/disconnect (recording = connected set)
            if (st_->recording) {                    // set is locked while recording (matches the UI)
                reply(fd, "error: currently recording -- stop the recording before changing streams\n");
            } else if (arg == "all") {
                st_->setSelection = std::vector<std::string>{"*"};
                reply(fd, "ok: connecting all\n");
            } else if (arg == "none") {
                st_->setSelection = std::vector<std::string>{};
                reply(fd, "ok: disconnecting all\n");
            } else {
                const auto keys  = splitCsv(arg);
                const auto avail = availKeys();      // from the published streams list
                std::string matched, unknown;
                for (const auto& k : keys)
                    ((std::find(avail.begin(), avail.end(), k) != avail.end()) ? matched : unknown) += k + " ";
                st_->setSelection = keys;            // unknown keys simply won't match a stream
                std::string r = "ok: connecting " + (matched.empty() ? std::string("(none)") : matched);
                if (!unknown.empty()) r += "; unknown: " + unknown;
                reply(fd, r + "\n");
            }
        } else if (v == "filename") {
            if (arg.empty()) reply(fd, "error: filename requires a path/template\n");
            else { st_->setFilename = arg; reply(fd, "ok\n"); }
        } else if (v == "set") {                 // set <field> <value> (subject/session/task/run)
            std::string val; const std::string key = verb(arg, val);
            if (key.empty()) reply(fd, "error: set requires <field> <value>\n");
            else { st_->setVars.emplace_back(key, val); reply(fd, "ok\n"); }
        } else if (v == "start") {
            if (!arg.empty()) st_->setFilename = arg;
            st_->startReq = true;
            reply(fd, "ok: starting (poll `status`)\n");
        } else if (v == "stop") {
            st_->stopReq = true;
            reply(fd, "ok: stopping\n");
        } else if (v == "quit" || v == "exit") {
            reply(fd, "bye\n");
            return false;
        } else {
            reply(fd, "error: unknown command (try `help`)\n");
        }
        return true;
    }

    std::vector<std::string> availKeys() const {     // keys from the published streams list
        std::vector<std::string> out;
        std::stringstream ss(st_->streamsText);
        std::string line;
        while (std::getline(ss, line)) {
            const std::size_t bar = line.find(" |");
            if (bar != std::string::npos) out.push_back(line.substr(0, bar));
        }
        return out;
    }

    static std::vector<std::string> splitCsv(const std::string& s) {
        std::vector<std::string> out;
        std::stringstream ss(s);
        std::string tok;
        while (std::getline(ss, tok, ',')) {
            while (!tok.empty() && tok.front() == ' ') tok.erase(0, 1);
            while (!tok.empty() && tok.back() == ' ')  tok.pop_back();
            if (!tok.empty()) out.push_back(tok);
        }
        return out;
    }

    static constexpr char kHello[] = "lsl-viewer remote control. type `help`.\n";
    rc_socket_t       listenfd_ = RC_INVALID;
    jthread      th_;
    RemoteState*      st_ = nullptr;
    int               port_ = 0;
    std::atomic<bool> up_{false};
    std::unique_ptr<lsl::stream_outlet> announce_;   // LSL discovery beacon
    mutable std::mutex emtx_;
    std::string        error_;
};
