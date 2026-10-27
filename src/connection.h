// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "protocol.h"
#include <chrono>
#include <deque>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>
#include <cerrno>
namespace hw {
// Single-thread owner; no implicit reconnect, lease acquisition or unmute.
class Connection {
public:
    Connection() = default;
    ~Connection() { disconnect(); }
    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;
    void disconnect() {
        if (fd_ >= 0) close(fd_);
        fd_ = -1; buffer_.clear(); events_.clear();
    }
    bool connect_to(const std::string& path, std::string* error) {
        disconnect(); sockaddr_un addr{};
        if (!address(path,&addr)) return fail("invalid_socket_path",error);
        fd_ = socket(AF_UNIX,SOCK_STREAM|SOCK_NONBLOCK|SOCK_CLOEXEC,0);
        if (fd_ < 0 || connect(fd_,reinterpret_cast<sockaddr*>(&addr),sizeof(addr)))
            return fail("connect_failed",error);
        Value message;
        if (!read_frame(&message,Clock::now()+std::chrono::seconds(3),error)) return false;
        if (!field(message,"event").is<std::string>() ||
            field(message,"event").get<std::string>() != "snapshot" ||
            !integer(field(message,"version"),1,1)) return fail("invalid_snapshot",error);
        events_.push_back(std::move(message)); return true;
    }
    bool request(const std::string& method, const Value& params, Value* response,
                 std::string* error, int timeout_ms=3000) {
        if (fd_ < 0 || !params.is<Object>() || !response || timeout_ms <= 0)
            return fail("invalid_client_request",error);
        const auto deadline=Clock::now()+std::chrono::milliseconds(timeout_ms);
        if (++id_ > 9007199254740991ULL) return fail("request_id_exhausted",error);
        const auto message=object({{"version",num(1)}, {"id",num(id_)},
            {"method",str(method)}, {"params",params}}).serialize()+"\n";
        if (message.size()>16385) return fail("frame_limit",error);
        size_t sent=0;
        while (sent<message.size()) {
            if (!wait(POLLOUT,deadline,error)) return false;
            ssize_t n=send(fd_,message.data()+sent,message.size()-sent,MSG_NOSIGNAL);
            if (n<0 && (errno==EAGAIN || errno==EINTR)) continue;
            if (n<=0) return fail("send_failed",error);
            sent+=static_cast<size_t>(n);
        }
        for (;;) {
            Value incoming;
            if (!read_frame(&incoming,deadline,error)) return false;
            if (integer(field(incoming,"id"),id_,id_)) { *response=incoming; return true; }
            if (!field(incoming,"event").is<std::string>()) return fail("unexpected_response",error);
            if (events_.size()>=64) return fail("event_queue_overflow",error);
            events_.push_back(std::move(incoming));
        }
    }
    bool next_event(Value* event, std::string* error, int timeout_ms=1000) {
        if (!event || fd_<0 || timeout_ms<=0) return fail("invalid_client_request",error);
        if (!events_.empty()) { *event=std::move(events_.front()); events_.pop_front(); return true; }
        if (!read_frame(event,Clock::now()+std::chrono::milliseconds(timeout_ms),error,false)) return false;
        if (!field(*event,"event").is<std::string>()) return fail("unexpected_response",error);
        return true;
    }
private:
    using Clock=std::chrono::steady_clock;
    bool fail(const char* reason,std::string* error) {
        if (error) *error=reason;
        disconnect(); return false;
    }
    bool timeout(std::string* error, bool close_on_timeout) {
        if (error) *error="timeout";
        if (close_on_timeout) disconnect();
        return false;
    }
    bool wait(short flags,Clock::time_point deadline,std::string* error, bool close_on_timeout=true) {
        for (;;) {
            auto remaining=std::chrono::duration_cast<std::chrono::milliseconds>(deadline-Clock::now()).count();
            if (remaining<=0) return timeout(error,close_on_timeout);
            pollfd p{fd_,flags,0}; int n=poll(&p,1,static_cast<int>(remaining));
            if (n<0 && errno==EINTR) continue;
            if (n==0) return timeout(error,close_on_timeout);
            if (n<0) return fail("poll_failed",error);
            if (p.revents & flags) return true;
            return fail("disconnected",error);
        }
    }
    bool read_frame(Value* value,Clock::time_point deadline,std::string* error, bool close_on_timeout=true) {
        for (;;) {
            auto end=buffer_.find('\n');
            if (end!=std::string::npos) {
                if (end>16384 || !hw::parse(*value,buffer_.substr(0,end))) return fail("invalid_frame",error);
                buffer_.erase(0,end+1); return true;
            }
            if (buffer_.size()>16384) return fail("frame_limit",error);
            if (!wait(POLLIN,deadline,error,close_on_timeout)) return false;
            char chunk[4096]; ssize_t n=recv(fd_,chunk,sizeof(chunk),0);
            if (n<0 && (errno==EINTR || errno==EAGAIN)) continue;
            if (n<=0) return fail("disconnected",error);
            buffer_.append(chunk,static_cast<size_t>(n));
        }
    }
    int fd_=-1;
    uint64_t id_=0;
    std::string buffer_;
    std::deque<Value> events_;
};
}
