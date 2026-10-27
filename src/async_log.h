// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <chrono>
#include <condition_variable>
#include <deque>
#include <fcntl.h>
#include <mutex>
#include <thread>
#include <unistd.h>
#include <cerrno>
#include <poll.h>
#include <string>
namespace hw {
class AsyncLog {
public:
    explicit AsyncLog(std::string instance) : instance_(std::move(instance)) {
        // Reopen instead of dup: do not alter the caller's stderr flags.
        fd_ = open("/proc/self/fd/2", O_WRONLY | O_APPEND | O_NONBLOCK | O_CLOEXEC);
        if (fd_ >= 0) worker_ = std::thread([this] { run(); });
    }
    ~AsyncLog() {
        {
            std::lock_guard<std::mutex> guard(mutex_);
            stopping_ = true;
            deadline_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(200);
        }
        changed_.notify_all();
        if (worker_.joinable()) worker_.join();
        if (fd_ >= 0) close(fd_);
    }
    bool available() const { return fd_ >= 0; }
    void push(std::string line) {
        if (line.size() > 2047) return; // Within Linux PIPE_BUF.
        line += '\n';
        std::lock_guard<std::mutex> guard(mutex_);
        if (queue_.size() >= 64) { if (dropped_ < 1000000000) ++dropped_; return; }
        if (dropped_ && queue_.size() < 63) {
            queue_.push_back("{\"tag\":\"lotus-hardwared\",\"level\":\"WARN\","
                "\"component\":\"logger\",\"event\":\"log_queue_overflow\","
                "\"result\":\"dropped\",\"instance\":\"" + instance_ +
                "\",\"uptime_ms\":" + std::to_string(std::chrono::duration_cast<
                    std::chrono::milliseconds>(std::chrono::steady_clock::now()-started_).count()) +
                ",\"count\":" + std::to_string(dropped_) + "}\n");
            dropped_ = 0;
        }
        queue_.push_back(std::move(line)); changed_.notify_one();
    }
private:
    void run() {
        for (;;) {
            std::string line;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                changed_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
                if (queue_.empty()) return;
                line = std::move(queue_.front()); queue_.pop_front();
            }
            size_t offset = 0;
            while (offset < line.size()) {
                {
                    std::lock_guard<std::mutex> guard(mutex_);
                    if (stopping_ && std::chrono::steady_clock::now() >= deadline_) return;
                }
                const ssize_t n = write(fd_, line.data()+offset, line.size()-offset);
                if (n > 0) offset += static_cast<size_t>(n);
                else if (n < 0 && errno == EINTR) continue;
                else if (n < 0 && errno == EAGAIN) {
                    pollfd p{fd_, POLLOUT, 0}; (void)poll(&p, 1, 20);
                } else return;
            }
        }
    }
    int fd_ = -1;
    std::string instance_;
    std::mutex mutex_;
    std::condition_variable changed_;
    std::deque<std::string> queue_;
    unsigned dropped_ = 0;
    bool stopping_ = false;
    std::chrono::steady_clock::time_point deadline_{};
    std::chrono::steady_clock::time_point started_ = std::chrono::steady_clock::now();
    std::thread worker_;
};
}
