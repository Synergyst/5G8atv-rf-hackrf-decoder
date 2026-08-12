#pragma once

#include <atomic>

namespace famidec {

class RuntimeLifecycle {
public:
    enum class Request : int { None = 0, Restart = 1, Quit = 2 };

    void start() {
        running_.store(true, std::memory_order_release);
        clear();
    }
    void request_restart() { request_.store(Request::Restart, std::memory_order_release); }
    void request_quit() {
        running_.store(false, std::memory_order_release);
        request_.store(Request::Quit, std::memory_order_release);
    }
    Request request() const { return request_.load(std::memory_order_acquire); }
    bool restart_requested() const { return request() == Request::Restart; }
    bool quit_requested() const { return request() == Request::Quit; }
    void clear() { request_.store(Request::None, std::memory_order_release); }
    bool running() const { return running_.load(std::memory_order_acquire); }
    std::atomic<bool>& running_ref() { return running_; }

private:
    std::atomic<Request> request_{Request::None};
    std::atomic<bool> running_{true};
};

} // namespace famidec
