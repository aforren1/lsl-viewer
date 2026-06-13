#pragma once
// std::jthread / std::stop_token backport.
//
// libstdc++, the MSVC STL, and recent libc++ provide <stop_token> + std::jthread; Apple's
// libc++ (Xcode clang) still ships neither, so the macOS build fails to find them. When the
// feature-test macro says they're present we just alias the standard types; otherwise we
// drop in a small, behaviour-compatible polyfill. Either way the names are pulled into the
// global namespace so call sites read as plain `jthread` / `stop_token`.

#include <version>

// Define LSL_JTHREAD_POLYFILL to force the polyfill even where the stdlib has jthread —
// used to exercise the macOS code path (this polyfill) on a Linux/Windows CI runner.
#if defined(__cpp_lib_jthread) && __cpp_lib_jthread >= 201911L && !defined(LSL_JTHREAD_POLYFILL)

#include <stop_token>
#include <thread>
using std::jthread;
using std::stop_source;
using std::stop_token;

#else

#include <atomic>
#include <memory>
#include <thread>
#include <type_traits>
#include <utility>

namespace compat_jthread {

// Cooperative-cancellation token: a shared atomic flag set by the owning stop_source.
class stop_token {
public:
    stop_token() = default;
    explicit stop_token(std::shared_ptr<std::atomic_bool> s) : state_(std::move(s)) {}
    bool stop_requested() const noexcept { return state_ && state_->load(std::memory_order_acquire); }
    bool stop_possible()  const noexcept { return static_cast<bool>(state_); }

private:
    std::shared_ptr<std::atomic_bool> state_;
};

class stop_source {
public:
    stop_source() : state_(std::make_shared<std::atomic_bool>(false)) {}
    stop_token get_token() const noexcept { return stop_token(state_); }
    bool request_stop() noexcept { return state_ && !state_->exchange(true, std::memory_order_acq_rel); }
    bool stop_requested() const noexcept { return state_ && state_->load(std::memory_order_acquire); }

private:
    std::shared_ptr<std::atomic_bool> state_;
};

// std::thread that carries a stop_source, passes its token to a callable that accepts one,
// and on destruction/move-assignment requests stop + joins (matching std::jthread).
class jthread {
public:
    jthread() noexcept = default;

    template <class F, class... Args,
              std::enable_if_t<!std::is_same_v<std::decay_t<F>, jthread>, int> = 0>
    explicit jthread(F&& f, Args&&... args) {
        if constexpr (std::is_invocable_v<std::decay_t<F>, stop_token, std::decay_t<Args>...>)
            t_ = std::thread(std::forward<F>(f), ssource_.get_token(), std::forward<Args>(args)...);
        else
            t_ = std::thread(std::forward<F>(f), std::forward<Args>(args)...);
    }

    jthread(const jthread&)            = delete;
    jthread& operator=(const jthread&) = delete;
    jthread(jthread&&) noexcept        = default;
    jthread& operator=(jthread&& other) noexcept {
        if (this != &other) {
            if (joinable()) { request_stop(); join(); }
            ssource_ = std::move(other.ssource_);
            t_       = std::move(other.t_);
        }
        return *this;
    }
    ~jthread() { if (joinable()) { request_stop(); join(); } }

    bool joinable() const noexcept { return t_.joinable(); }
    void join() { t_.join(); }
    void detach() { t_.detach(); }
    bool request_stop() noexcept { return ssource_.request_stop(); }
    stop_source get_stop_source() noexcept { return ssource_; }
    stop_token  get_stop_token() const noexcept { return ssource_.get_token(); }

private:
    stop_source ssource_;
    std::thread t_;
};

}  // namespace compat_jthread

using compat_jthread::jthread;
using compat_jthread::stop_source;
using compat_jthread::stop_token;

#endif
