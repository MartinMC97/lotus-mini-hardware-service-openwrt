// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "controls.h"
#include <chrono>
#include <condition_variable>
#include <fcntl.h>
#include <mutex>
#include <thread>
#include <unistd.h>

namespace hw {
// One worker owns all output writes. set/off waits for the old generation to
// stop writing; a delayed old PWM edge cannot relight an explicitly closed LED.
class LedRunner {
public:
    using Sink=std::function<bool(unsigned)>; // RGB bitmask: R=4 G=2 B=1
    explicit LedRunner(Sink sink):sink_(std::move(sink)),worker_([this]{run();}) {}
    ~LedRunner() {
        { std::lock_guard<std::mutex> lock(mutex_); stop_=true; ++generation_; }
        cv_.notify_all(); worker_.join();
    }
    bool set(LedEffect effect) {
        std::lock_guard<std::mutex> lock(mutex_);
        ++generation_; effect_=effect; started_=Clock::now();
        if (!write(0)) { effect_={}; cv_.notify_all(); return false; }
        cv_.notify_all(); return true;
    }
    bool healthy() { std::lock_guard<std::mutex> lock(mutex_); return healthy_; }
private:
    using Clock=std::chrono::steady_clock;
    bool write(unsigned mask) {
        healthy_=sink_(mask);
        if (!healthy_) effect_={};
        return healthy_;
    }
    void run() {
        std::unique_lock<std::mutex> lock(mutex_);
        while (!stop_) {
            if (effect_.kind==LedEffect::Off) {
                cv_.wait(lock,[this]{return stop_ || effect_.kind!=LedEffect::Off;});
                continue;
            }
            const auto generation=generation_;
            const auto frame=Clock::now();
            const auto elapsed=std::chrono::duration_cast<std::chrono::milliseconds>(frame-started_).count();
            const auto duties=led_duties(effect_,static_cast<uint64_t>(elapsed));
            std::array<unsigned,5> edges{{0,duties[0],duties[1],duties[2],10000}};
            std::sort(edges.begin(),edges.end());
            for (size_t i=0;i+1<edges.size();++i) {
                if (edges[i]==edges[i+1]) continue;
                unsigned mask=0;
                for (unsigned c=0;c<3;++c) if (duties[c]>edges[i]) mask|=1u<<(2-c);
                if (!write(mask)) { sink_(0); break; }
                if (cv_.wait_until(lock,frame+std::chrono::microseconds(edges[i+1]),
                    [this,generation]{return stop_ || generation_!=generation;})) break;
            }
        }
        sink_(0);
    }
    Sink sink_;
    std::mutex mutex_;
    std::condition_variable cv_;
    bool stop_=false,healthy_=true;
    uint64_t generation_=0;
    LedEffect effect_{};
    Clock::time_point started_=Clock::now();
    std::thread worker_;
};

// Explicit profile paths only; construction does not touch hardware.
class SysfsRgb {
public:
    explicit SysfsRgb(std::string root):root_(std::move(root)) {}
    ~SysfsRgb() { for (int fd:fds_) if (fd>=0) close(fd); }
    SysfsRgb(const SysfsRgb&)=delete;
    SysfsRgb& operator=(const SysfsRgb&)=delete;
    bool write(unsigned mask) {
        static const char* names[]={"redled","greenled","blueled"};
        bool ok=true;
        for (unsigned i=0;i<3;++i) {
            if (fds_[i]<0) fds_[i]=open((root_+"/class/leds/"+names[i]+"/brightness").c_str(),
                O_WRONLY|O_NONBLOCK|O_CLOEXEC);
            const char value=(mask&(1u<<(2-i)))?'1':'0';
            if (fds_[i]<0 || pwrite(fds_[i],&value,1,0)!=1) {
                if (fds_[i]>=0) close(fds_[i]);
                fds_[i]=-1; ok=false;
            }
        }
        return ok;
    }
private:
    std::string root_;
    std::array<int,3> fds_{{-1,-1,-1}};
};
} // namespace hw
