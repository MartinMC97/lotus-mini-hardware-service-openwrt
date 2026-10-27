// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "connection.h"
#include <atomic>
#include <cstdlib>
#include <mutex>
#include <thread>

namespace hw {
// Header-only, C++14 client; one I/O mutex, bounded event queue, no reconnect or
// direct-backend fallback. Business policies/themes stay in the application.
class ApplicationClient {
public:
    ~ApplicationClient() { stop(); }
    static std::string configured_socket() {
        const char* path=std::getenv("LOTUS_HARDWARE_SOCKET");
        return path?path:"";
    }
    bool start(const std::string& path,std::string* error,bool claim=true) {
        stop(); std::lock_guard<std::mutex> lock(mutex_);
        if (!connection_.connect_to(path,error)) return false;
        Value message;
        if (!connection_.next_event(&message,error)) return false;
        state_=field(message,"data");
        ready_=true; claim_=claim;
        if (!command("events.subscribe",object({}),&message,error) || (claim &&
            (!command("lease.acquire",object({{"ttl_ms",num(15000)}}),&message,error) ||
            !command("audio.acquire",object({{"capture",Value(!muted_locked())},{"playback",Value(true)}}),&message,error)))) {
            lost(); return false;
        }
        next_renew_=Clock::now()+std::chrono::seconds(5);
        next_state_=Clock::now(); stopping_=false;
        worker_=std::thread([this]{run();}); return true;
    }
    void stop() {
        stopping_=true;
        if (worker_.joinable()) worker_.join();
        std::lock_guard<std::mutex> lock(mutex_); lost();
    }
    bool ready() const { return ready_; }
    bool muted() { std::lock_guard<std::mutex> lock(mutex_); return muted_locked(); }
    Value state() { std::lock_guard<std::mutex> lock(mutex_); return state_; }
    bool request(const std::string& method,const Value& params,Value* result,std::string* error) {
        std::lock_guard<std::mutex> lock(mutex_);
        return command(method,params,result,error);
    }
    bool take(Value* event) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (events_.empty()) return false;
        *event=std::move(events_.front()); events_.pop_front(); return true;
    }
    // Idle heartbeat timing is selected by the app, not the daemon.
    bool led(const Value& effect,unsigned idle_interval_ms, std::string* error) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (ready_ && idle_interval_ms && idle_interval_==idle_interval_ms && idle_effect_==effect) {
            if (error) error->clear();
            return true;
        }
        idle_effect_=Value(); idle_interval_=0;
        Value result;
        if (!command("led.set",idle_interval_ms?object({{"effect",str("off")}}):effect,&result,error)) return false;
        idle_effect_=idle_interval_ms?effect:Value(); idle_interval_=idle_interval_ms;
        next_idle_=Clock::now()+std::chrono::milliseconds(idle_interval_ms);
        return true;
    }
private:
    using Clock=std::chrono::steady_clock;
    bool muted_locked() const {
        return !ready_ || field(state_,"privacy_enforcement")!=str("cooperative") ||
            !field(state_,"microphone_muted").is<bool>() || field(state_,"microphone_muted").get<bool>();
    }
    void lost() {
        ready_=false; connection_.disconnect(); events_.clear(); state_=Value(); idle_effect_=Value();
    }
    bool command(const std::string& method,const Value& params,Value* result,std::string* error) {
        if (!ready_) { if (error) *error="hardware_service_unavailable"; return false; }
        Value response; std::string reason;
        if (!connection_.request(method,params,&response,&reason,1000)) {
            lost(); if (error) *error=reason; return false;
        }
        if (field(response,"ok")==Value(true)) {
            if (method=="state.get") state_=field(response,"result");
            if (method=="events.subscribe") state_=field(field(response,"result"),"snapshot");
        }
        // Handle privacy events queued before this command response immediately.
        drain();
        if (field(response,"ok")!=Value(true)) {
            if (error) *error=field(field(response,"error"),"code").get<std::string>();
            return false;
        }
        *result=field(response,"result"); if (error) error->clear(); return ready_;
    }
    void drain() {
        Value event; std::string error;
        for (unsigned i=0;i<64 && ready_;++i) {
            if (!connection_.next_event(&event,&error,1)) {
                if (error!="timeout") lost();
                return;
            }
            const auto name=field(event,"event"); const auto data=field(event,"data");
            if (name==str("privacy.changed")) {
                if (!field(data,"muted").is<bool>() || !state_.is<Object>()) { lost(); return; }
                state_.get<Object>()["microphone_muted"]=field(data,"muted");
            } else if (name==str("volume.changed") && state_.is<Object>())
                state_.get<Object>()["volume_percent"]=field(data,"percent");
            else if (name==str("battery.changed") && state_.is<Object>())
                state_.get<Object>()["battery"]=data;
            else if (name==str("temperature.changed") && state_.is<Object>())
                state_.get<Object>()["temperature"]=data;
            else if (name==str("audio.changed") && state_.is<Object>())
                state_.get<Object>()["audio"]=data;
            else if (name==str("button.action") || name==str("button.feedback")) {
                if (events_.size()>=32) { lost(); return; }
                events_.push_back(event);
            }
        }
    }
    void run() {
        bool capture=false;
        while (!stopping_ && ready_) {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                drain(); Value result; std::string error;
                const auto now=Clock::now();
                if (ready_ && claim_ && now>=next_renew_) {
                    next_renew_=now+std::chrono::seconds(5);
                    if (!command("lease.renew",object({{"ttl_ms",num(15000)}}),&result,&error)) lost();
                }
                if (ready_ && now>=next_state_) {
                    next_state_=now+std::chrono::milliseconds(500);
                    command("state.get",object({}),&result,&error);
                }
                const bool want_capture=!muted_locked();
                if (ready_ && claim_ && want_capture!=capture) {
                    if (command("audio.acquire",object({{"capture",Value(want_capture)},
                        {"playback",Value(true)}}),&result,&error)) capture=want_capture;
                    else if (want_capture) lost();
                }
                if (ready_ && !muted_locked() && idle_interval_ && now>=next_idle_) {
                    next_idle_=now+std::chrono::milliseconds(idle_interval_);
                    command("led.set",idle_effect_,&result,&error);
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    }
    Connection connection_; std::mutex mutex_; std::thread worker_;
    std::atomic<bool> stopping_{true},ready_{false};
    Value state_,idle_effect_;
    unsigned idle_interval_=0;
    bool claim_=true;
    Clock::time_point next_state_{},next_renew_{},next_idle_{};
    std::deque<Value> events_;
};
}
