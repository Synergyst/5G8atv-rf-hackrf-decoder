#pragma once

#include <atomic>

namespace famidec {

// Run-level lifecycle requests. The UI/control side submits requests; the
// application owner consumes them at a safe boundary and performs teardown /
// reconstruction. DSP code never destroys or resizes UI state itself.
class RuntimeLifecycle {
public:
    enum class Request : int { None = 0, Restart = 1, Quit = 2 };

    void request_restart() { request_.store(Request::Restart, std::memory_order_release); }
    void request_quit() { request_.store(Request::Quit, std::memory_order_release); }
    Request request() const { return request_.load(std::memory_order_acquire); }
    bool restart_requested() const { return request() == Request::Restart; }
    bool quit_requested() const { return request() == Request::Quit; }
    void clear() { request_.store(Request::None, std::memory_order_release); }


private:
    std::atomic<Request> request_{Request::None};
};

} // namespace famidec
