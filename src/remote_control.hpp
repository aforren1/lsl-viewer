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
//   get                       stream the last completed recording to the client: a header
//                             line "OK <bytes> <filename>" then <bytes> of raw file data
//   quit                      close the connection
//
// Replies: one line, either "ok: ..." or "error: ...", except `streams` (one line per
// stream, fields separated by " | ", first field is the key) and `get` (below). Commands
// that the main loop has to carry out -- select/start/stop -- do not answer until it has,
// so the reply carries the real outcome ("ok: recording -> file" / "error: no streams
// connected") and a `status` after one of them cannot report the state from before it.
//
// Security: `get` only serves the viewer's own last completed recording, never a
// client-supplied path, so it is not an arbitrary-file read. The listener binds
// loopback (127.0.0.1) by default; pass bindAll=true (LSL_RC_BIND=all) to expose it
// on the network. There is no authentication, so only open it up on a trusted LAN.
// clients() exposes the live peers for the GUI to show: on a trusted network the
// failure worth catching is a stray script -- last week's rig, a second copy of the
// experiment -- driving a recording nobody meant it to touch, and seeing it connect
// catches that where a credential check only prevents one instance of it.
//
// Discovery: while running we also publish an LSL outlet (name "LSLViewerControl",
// type "ViewerControl"); a client resolves it, takes the host from info.hostname() and
// the port from the last field of source_id "lsl-viewer-rc:<host>:<pid>:<port>", then
// connects over TCP. No mDNS. The host and pid are in there because LSL treats source_id
// as the identity of a logical stream: with the port alone, two viewers on two machines
// that both use 22345 would publish the same id and a resolver could conflate them.
//
// Ports: a second viewer on one host cannot have 22345 as well, so start() can fall back
// to an ephemeral port (bind 0, read the real one back with getsockname) and announce
// that instead. A client that resolves never notices; one with 22345 hard-coded still
// reaches the viewer that got there first. The fallback is off when the user pinned a
// port -- silently moving somewhere else is worse than saying the port is taken.
//
// Threading: an accept thread hands each connection to its own session thread (up to
// kMaxClients), so one idle or file-downloading client can't block another. Session
// threads only touch a mutex-guarded RemoteState: they queue requests and read snapshots
// that the main loop publishes each frame. The main loop owns the Recorder/Discovery and
// applies the requests, so there are no cross-thread races on the recorder. TCP via BSD
// sockets (Linux/macOS) or Winsock (Windows) — the socket layer is abstracted below so
// the server logic is shared.

#include <lsl_cpp.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <cstdint>
#include <cstdio>
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
using rc_socklen_t = int;                 // Winsock's getsockname takes int*, not socklen_t
static constexpr rc_socket_t RC_INVALID = INVALID_SOCKET;
static constexpr int         RC_SHUT_RDWR = SD_BOTH;
inline int rc_close(rc_socket_t s) { return ::closesocket(s); }
// Winsock needs per-process init; a function-local static does it once, thread-safe,
// and tears it down at exit. Referenced from RemoteControl::start().
struct RcWsaInit { RcWsaInit() { WSADATA w; WSAStartup(MAKEWORD(2, 2), &w); } ~RcWsaInit() { WSACleanup(); } };
#else
using rc_socket_t = int;
using rc_socklen_t = socklen_t;
static constexpr rc_socket_t RC_INVALID = -1;
static constexpr int         RC_SHUT_RDWR = SHUT_RDWR;
inline int rc_close(rc_socket_t s) { return ::close(s); }
#endif

// Host name and pid, for the discovery beacon's source_id (see the header comment).
inline std::string rc_hostname() {
    char h[256] = {0};
    if (::gethostname(h, (int)sizeof(h) - 1) != 0) return "unknown";
    return h[0] ? std::string(h) : std::string("unknown");
}
inline long rc_pid() {
#if defined(_WIN32)
    return (long)::GetCurrentProcessId();
#else
    return (long)::getpid();
#endif
}
// "ip:port" of a peer, for the GUI's client list. Hand-formatted from the four octets
// rather than inet_ntop, which differs in header and availability between the two stacks;
// the listener is AF_INET, so there is no IPv6 case to miss.
inline std::string rc_peer_string(const sockaddr_in& a) {
    const std::uint32_t h = ntohl(a.sin_addr.s_addr);
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%u.%u.%u.%u:%u",
                  (h >> 24) & 0xFF, (h >> 16) & 0xFF, (h >> 8) & 0xFF, h & 0xFF,
                  (unsigned)ntohs(a.sin_port));
    return buf;
}
// Identity of this viewer's control endpoint. The "lsl-viewer-rc" prefix is a contract:
// tools/xdf_record.cpp filters beacons out of a recording by it. The port is last so a
// client can take it from the text after the final ':'.
inline std::string rc_beacon_source_id(int port) {
    return "lsl-viewer-rc:" + rc_hostname() + ":" + std::to_string(rc_pid()) + ":" +
           std::to_string(port);
}

// One command that only the main loop can carry out. The session thread queues it and
// blocks until the main loop fills in `msg`, then sends that back verbatim — so the
// client learns whether the recording actually started instead of "ok, poll status".
struct RcRequest {
    enum class Kind { Start, Stop, Select };
    Kind                     kind;
    std::vector<std::string> keys;   // Select: {"*"} = all, {} = none
    bool                     done = false;
    std::string              msg;    // reply line, written by the main loop
};

// Shared between the session threads and the main loop.
struct RemoteState {
    std::mutex              mtx;
    std::condition_variable cv;       // main loop -> sessions: a queued request is done
    // published by the main loop (kept fresh each frame, and right after any request):
    std::string streamsText;          // `streams` reply body
    std::string statusText;           // `status` reply body
    std::string selectedText = "none";// `selected` reply body
    bool        recording   = false;  // a recording is in progress (can't `get` mid-record)
    std::string lastFile;             // path of the last completed + flushed recording ("" = none)
    bool        closePending = false; // stopped, but the writer is still flushing (`get` waits)
    // requests from the sessions, consumed by the main loop:
    std::optional<std::string>                       setFilename;
    std::vector<std::pair<std::string, std::string>> setVars;  // template fields (subject/task/...)
    std::vector<std::shared_ptr<RcRequest>>          queue;     // FIFO, drained every frame
};

class RemoteControl {
public:
    ~RemoteControl() { stop(); }

    bool listening() const { return up_.load(std::memory_order_acquire); }
    int  port()      const { return port_; }
    std::string error() const { std::lock_guard<std::mutex> lk(emtx_); return error_; }

    // Peers of the live sessions ("ip:port" each), for the GUI to show. Cheap: the
    // strings are built once at accept time, and the usual answer is an empty vector.
    std::vector<std::string> clients() const {
        std::vector<std::string> out;
        std::lock_guard<std::mutex> lk(cmtx_);
        out.reserve(clients_.size());
        for (const auto& c : clients_) out.push_back(c.peer);
        return out;
    }

    // portFallback: if `port` is taken, bind an ephemeral one instead of failing. Off by
    // default — only pass true when the port is our own default, never when the user
    // pinned one (see the header comment).
    bool start(int port, RemoteState* state, bool bindAll = false, bool portFallback = false) {
        if (up_) return true;
#if defined(_WIN32)
        static RcWsaInit s_wsa;   // process-lifetime Winsock init on first start()
        (void)s_wsa;
#endif
        st_ = state; port_ = port;
        // A fresh socket per attempt: a socket whose bind() failed is not portably
        // re-bindable, and the retry below needs a clean one.
        auto bindTo = [&](uint16_t p) {
            listenfd_ = ::socket(AF_INET, SOCK_STREAM, 0);
            if (listenfd_ == RC_INVALID) { setError("socket() failed"); return false; }
#if !defined(_WIN32)
            // POSIX only. Windows SO_REUSEADDR means the opposite of what it does here:
            // it lets a second socket bind a port that is already live, so a second
            // viewer would silently steal connections from the first instead of falling
            // back to another port. Windows rebinds a closed listener without it.
            int yes = 1;
            ::setsockopt(listenfd_, SOL_SOCKET, SO_REUSEADDR, (const char*)&yes, sizeof(yes));
#endif
            sockaddr_in addr{};
            addr.sin_family = AF_INET;
            // Loopback by default (no auth); bindAll exposes it on the network for lab use.
            addr.sin_addr.s_addr = htonl(bindAll ? INADDR_ANY : INADDR_LOOPBACK);
            addr.sin_port = htons(p);
            if (::bind(listenfd_, (sockaddr*)&addr, sizeof(addr)) != 0) {
                setError("bind() failed (port in use?)");
                rc_close(listenfd_); listenfd_ = RC_INVALID; return false;
            }
            return true;
        };
        if (!bindTo((uint16_t)port) && !(portFallback && bindTo(0))) return false;
        setError({});   // a first attempt that lost the race left its message behind
        // Read back what we actually got: with the fallback it's whatever the OS picked,
        // and everything downstream (the beacon, the UI label) has to announce that.
        sockaddr_in bound{};
        rc_socklen_t blen = (rc_socklen_t)sizeof(bound);
        if (::getsockname(listenfd_, (sockaddr*)&bound, &blen) == 0) port_ = (int)ntohs(bound.sin_port);
        if (::listen(listenfd_, kMaxClients) != 0) {
            setError("listen() failed"); rc_close(listenfd_); listenfd_ = RC_INVALID; return false;
        }
        up_.store(true, std::memory_order_release);
        th_ = jthread([this](stop_token s) { serve(s); });
        // LSL announcement so clients DISCOVER the control endpoint without knowing
        // host:port — they resolve type "ViewerControl"; LSL supplies the hostname, and
        // the TCP port is the last field of source_id ("lsl-viewer-rc:<host>:<pid>:<port>")
        // so it's readable from the resolve result alone (desc isn't, without opening an
        // inlet). The outlet stays resolvable while we're up.
        try {
            const std::string sid = rc_beacon_source_id(port_);
            lsl::stream_info ai("LSLViewerControl", "ViewerControl", 1,
                                lsl::IRREGULAR_RATE, lsl::cf_string, sid);
            ai.desc().append_child_value("port", std::to_string(port_));
            ai.desc().append_child_value("pid", std::to_string(rc_pid()));
            ai.desc().append_child_value("protocol", "tcp-text-lines");
            // Resolving gives a client info.hostname(), which does NOT connect while we
            // are on loopback — it has to use 127.0.0.1 instead, and only if it is on
            // this machine at all. Say which, so a client can tell "not exposed" from
            // "wrong address". (In desc, so it needs an inlet: it is a hint, not a
            // credential, and the cheap resolve-only path doesn't need it.)
            ai.desc().append_child_value("bind", bindAll ? "all" : "loopback");
            announce_ = std::make_unique<lsl::stream_outlet>(ai);
        } catch (...) { /* discovery is best-effort */ }
        return true;
    }

    void stop() {
        if (!up_.exchange(false)) return;
        announce_.reset();
        if (listenfd_ != RC_INVALID) { ::shutdown(listenfd_, RC_SHUT_RDWR); rc_close(listenfd_); listenfd_ = RC_INVALID; }
        th_.request_stop();
        // Unblock every session parked in recv() on an idle client, else the accept
        // thread's wait for them (and this join) hangs until they happen to send.
        // Under cmtx_: a session erases itself and closes its own descriptor while
        // holding it, so we can't shutdown() a number the OS has already recycled.
        { std::lock_guard<std::mutex> lk(cmtx_);
          for (const auto& c : clients_) ::shutdown(c.fd, RC_SHUT_RDWR); }
        if (th_.joinable()) th_.join();
    }

private:
    void setError(const std::string& e) { std::lock_guard<std::mutex> lk(emtx_); error_ = e; }

    void serve(stop_token stoke) {
        while (!stoke.stop_requested() && up_) {
            sockaddr_in peer{};
            rc_socklen_t plen = (rc_socklen_t)sizeof(peer);
            rc_socket_t fd = ::accept(listenfd_, (sockaddr*)&peer, &plen);
            if (fd == RC_INVALID) {
                // stop() closed the listen socket -> exit; a transient accept error
                // (client RST before accept, e.g. a port scanner) must not kill the
                // server, so keep listening.
                if (stoke.stop_requested() || !up_) break;
                continue;
            }
            bool busy = false;
            { std::lock_guard<std::mutex> lk(cmtx_);
              if (clients_.size() >= kMaxClients) busy = true;
              else clients_.push_back(RcClient{fd, rc_peer_string(peer)}); }
            if (busy) { ::send(fd, kBusy, (int)(sizeof(kBusy) - 1), 0); rc_close(fd); continue; }
            std::thread(&RemoteControl::session, this, fd).detach();
        }
        // The sessions are detached, so the accept thread waits them out here rather
        // than leaving them to touch a destroyed RemoteControl. Bounded: stop() has
        // already shut their sockets down, and a session blocked on the main loop
        // gives up as soon as it sees up_ go false.
        std::unique_lock<std::mutex> lk(cmtx_);
        ccv_.wait(lk, [this] { return clients_.empty(); });
    }

    void session(rc_socket_t fd) {
        ::send(fd, kHello, (int)(sizeof(kHello) - 1), 0);
        std::string buf;
        char tmp[1024];
        bool open = true;
        while (open && up_) {
            const int n = (int)::recv(fd, tmp, (int)sizeof(tmp), 0);
            if (n <= 0) break;
            buf.append(tmp, (std::size_t)n);
            // Cap the unterminated-line buffer so a client that never sends '\n'
            // (garbage/binary traffic) can't grow it without bound.
            if (buf.size() > kMaxLine) { reply(fd, "error: line too long\n"); break; }
            std::size_t nl;
            while ((nl = buf.find('\n')) != std::string::npos) {
                std::string line = buf.substr(0, nl);
                buf.erase(0, nl + 1);
                while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
                if (!dispatch(fd, line)) { open = false; break; }
            }
        }
        { std::lock_guard<std::mutex> lk(cmtx_);
          clients_.erase(std::remove_if(clients_.begin(), clients_.end(),
                                        [fd](const RcClient& c) { return c.fd == fd; }),
                         clients_.end());
          rc_close(fd); }
        ccv_.notify_all();
    }

    static std::string verb(const std::string& s, std::string& rest) {
        std::size_t sp = s.find(' ');
        if (sp == std::string::npos) { rest.clear(); return s; }
        rest = s.substr(sp + 1);
        while (!rest.empty() && rest.front() == ' ') rest.erase(0, 1);
        return s.substr(0, sp);
    }

    void reply(rc_socket_t fd, const std::string& s) { ::send(fd, s.data(), (int)s.size(), 0); }

    // Hand a request to the main loop and wait for it to be applied; returns the reply
    // line it produced. Waiting in slices (rather than one long wait) is what lets a
    // client parked here notice stop() and let go. On timeout the request is pulled back
    // out of the queue, so it can't fire minutes later for a client that's long gone.
    std::string submit(std::unique_lock<std::mutex>& lk, const std::shared_ptr<RcRequest>& req) {
        st_->queue.push_back(req);
        for (int i = 0; i < kApplySlices && !req->done && up_; ++i)
            st_->cv.wait_for(lk, std::chrono::milliseconds(kSliceMs), [&] { return req->done; });
        if (!req->done) {
            auto& q = st_->queue;
            q.erase(std::remove(q.begin(), q.end(), req), q.end());
            return "error: the viewer did not respond (is its window running?)\n";
        }
        return req->msg + "\n";
    }

    // Stream the last completed recording to the client: a header line
    // "OK <bytes> <filename>\n" followed by exactly <bytes> of raw file data. Blocking, on a
    // session thread (not the recording/UI thread); only the state mutex is held, briefly, to
    // read the path. The client reads the header, then reads <bytes> and saves them.
    // The path is always the viewer's own lastFile, never client-supplied, so this cannot
    // read an arbitrary file off the host.
    void sendFile(rc_socket_t fd) {
        std::string path;
        // A `stop` a moment ago leaves the writer flushing on the recorder's closer
        // thread. Wait that out instead of telling a stop-then-get script that there is
        // no recording; the file only becomes readable once the close lands.
        for (int i = 0; i < kFlushSlices; ++i) {
            bool pending;
            { std::lock_guard<std::mutex> lk(st_->mtx);
              if (st_->recording) { reply(fd, "error: stop the recording before `get`\n"); return; }
              path    = st_->lastFile;
              pending = st_->closePending; }
            if (!path.empty() || !pending || !up_) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(kSliceMs));
        }
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
        if (v == "get") { sendFile(fd); return true; }   // binary transfer; locks only briefly
        std::unique_lock<std::mutex> lk(st_->mtx);
        if (v == "help") {
            reply(fd, "commands: help status streams selected select filename set start stop get quit\n"
                      "  filename <template>   e.g. sub-{subject}_task-{task}_run-{run}_eeg.xdf\n"
                      "  set <field> <value>   subject|session|task|run  (also {datetime}/{date}/{time})\n"
                      "  get                   stream the last recording: 'OK <bytes> <name>' + raw data\n"
                      "select/start/stop reply only once the viewer has applied them.\n");
        } else if (v == "status") {
            reply(fd, st_->statusText + "\n");
        } else if (v == "streams") {
            // Never an empty reply: a client reading a line would block until timeout.
            reply(fd, st_->streamsText.empty() ? "(none)\n" : st_->streamsText);
        } else if (v == "selected") {
            reply(fd, st_->selectedText + "\n");
        } else if (v == "select") {                 // connect/disconnect (recording = connected set)
            auto req = std::make_shared<RcRequest>();
            req->kind = RcRequest::Kind::Select;
            if (arg == "all")       req->keys = {"*"};
            else if (arg == "none") req->keys = {};
            else {
                req->keys = splitCsv(arg);
                // The main loop validates the keys against the streams it can actually
                // see, so a typo is rejected there rather than half-applied here.
                if (req->keys.empty()) { reply(fd, "error: select needs all|none|<key,key,...>\n"); return true; }
            }
            reply(fd, submit(lk, req));
        } else if (v == "filename") {
            if (arg.empty()) reply(fd, "error: filename requires a path/template\n");
            else { st_->setFilename = arg; reply(fd, "ok\n"); }
        } else if (v == "set") {                 // set <field> <value> (subject/session/task/run)
            std::string val; const std::string key = verb(arg, val);
            if (key.empty()) reply(fd, "error: set requires <field> <value>\n");
            else { st_->setVars.emplace_back(key, val); reply(fd, "ok\n"); }
        } else if (v == "start") {
            // The path lands in setFilename, which the main loop applies before it drains
            // the queue — so this request already sees it.
            if (!arg.empty()) st_->setFilename = arg;
            auto req = std::make_shared<RcRequest>();
            req->kind = RcRequest::Kind::Start;
            reply(fd, submit(lk, req));
        } else if (v == "stop") {
            auto req = std::make_shared<RcRequest>();
            req->kind = RcRequest::Kind::Stop;
            reply(fd, submit(lk, req));
        } else if (v == "quit" || v == "exit") {
            reply(fd, "bye\n");
            return false;
        } else {
            reply(fd, "error: unknown command (try `help`)\n");
        }
        return true;
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
    static constexpr char kBusy[]  = "error: too many control clients\n";
    static constexpr std::size_t kMaxLine = 64 * 1024;   // reject an unterminated line past this
    static constexpr std::size_t kMaxClients = 4;        // concurrent control connections
    static constexpr int kSliceMs = 100;                 // wait granularity (so stop() is prompt)
    static constexpr int kApplySlices = 20;              // 2 s for the main loop to apply a request
    static constexpr int kFlushSlices = 30;              // 3 s for a just-stopped file to close
    struct RcClient { rc_socket_t fd; std::string peer; };
    rc_socket_t       listenfd_ = RC_INVALID;
    mutable std::mutex       cmtx_;      // guards clients_ (and serializes close vs shutdown)
    std::condition_variable  ccv_;       // a session left clients_
    std::vector<RcClient>    clients_;   // live sessions: stop() unblocks their recv(), the GUI lists them
    jthread      th_;
    RemoteState*      st_ = nullptr;
    int               port_ = 0;
    std::atomic<bool> up_{false};
    std::unique_ptr<lsl::stream_outlet> announce_;   // LSL discovery beacon
    mutable std::mutex emtx_;
    std::string        error_;
};
