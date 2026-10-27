// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "privacy.h"
#include "privacy_store.h"
#include <condition_variable>
#include <mutex>
#include <thread>

namespace hw {
// One in-flight write, one latest pending target, one completion mailbox.
// Serialized writes preserve disk ordering; no debounce of privacy requests.
class PrivacyWorker {
public:
    struct Completion { PrivacyState::Save save; std::string error; unsigned failures; };
    explicit PrivacyWorker(const std::string& directory):store_(directory) {}
    ~PrivacyWorker() { finish(); }
    bool valid() const { return store_.valid(); }
    bool load(bool* muted,std::string* reason) { return store_.load(muted,reason); }
    void start() { thread_=std::thread([this]{run();}); }
    void schedule(PrivacyState::Save save) {
        std::lock_guard<std::mutex> guard(mutex_);
        target_=save; pending_=true; cv_.notify_all();
    }
    bool take(Completion* result) {
        std::lock_guard<std::mutex> guard(mutex_);
        if (!completed_) return false;
        *result=result_; completed_=false; return true;
    }
    void finish() {
        { std::lock_guard<std::mutex> guard(mutex_); stop_=true; }
        cv_.notify_all(); if (thread_.joinable()) thread_.join();
    }
private:
    void run() {
        std::unique_lock<std::mutex> guard(mutex_);
        for (;;) {
            cv_.wait(guard,[this]{return pending_ || stop_;});
            if (!pending_) return;
            const auto save=target_; pending_=false;
            guard.unlock(); const std::string error=store_.save(save.muted); guard.lock();
            if (!error.empty() && failures_<1000000000) ++failures_;
            result_={save,error,failures_}; completed_=true;
        }
    }
    PrivacyStore store_;
    PrivacyState::Save target_{};
    Completion result_{};
    bool pending_=false,completed_=false,stop_=false;
    unsigned failures_=0;
    std::mutex mutex_; std::condition_variable cv_; std::thread thread_;
};
}
