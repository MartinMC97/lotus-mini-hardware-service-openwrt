// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "protocol.h"
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <fcntl.h>
#include <mutex>
#include <sys/file.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <cerrno>

namespace hw {
class VolumeStore {
public:
    explicit VolumeStore(const std::string& directory) {
        struct stat before{},after{};
        if (lstat(directory.c_str(),&before) || !safe_dir(before)) return;
        dir_=open(directory.c_str(),O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
        if (dir_<0 || fstat(dir_,&after) || !safe_dir(after) ||
            before.st_ino!=after.st_ino || before.st_dev!=after.st_dev) return;
        lock_=openat(dir_,"volume.lock",O_RDWR|O_CREAT|O_NONBLOCK|O_NOFOLLOW|O_CLOEXEC,0600);
        struct stat lock_stat{};
        if (lock_<0 || fstat(lock_,&lock_stat) || !safe_file(lock_stat) || flock(lock_,LOCK_EX|LOCK_NB)) return;
        valid_=true;
    }
    ~VolumeStore() {
        finish();
        if (lock_>=0) close(lock_);
        if (dir_>=0) close(dir_);
    }
    void finish() {
        { std::lock_guard<std::mutex> guard(mutex_); stop_=true; }
        cv_.notify_all();
        if (worker_.joinable()) worker_.join();
    }
    VolumeStore(const VolumeStore&)=delete;
    VolumeStore& operator=(const VolumeStore&)=delete;
    bool valid() const { return valid_; }
    // Returns missing without creating a default or overwriting damaged state.
    bool load(int* percent,std::string* reason) {
        if (!valid_) { *reason="unsafe_or_locked_directory"; return false; }
        int fd=openat(dir_,"volume.json",O_RDONLY|O_NONBLOCK|O_NOFOLLOW|O_CLOEXEC);
        if (fd<0) { *reason=errno==ENOENT?"missing":"open_failed"; return false; }
        struct stat stat{};
        if (fstat(fd,&stat) || !safe_file(stat) || stat.st_size>256) {
            close(fd); *reason="unsafe_state_file"; return false;
        }
        char buffer[257];
        ssize_t bytes;
        do { bytes=read(fd,buffer,sizeof(buffer)); } while (bytes<0 && errno==EINTR);
        close(fd);
        if (bytes<=0 || bytes>256) { *reason="invalid_state"; return false; }
        std::string data(buffer,static_cast<size_t>(bytes));
        while (!data.empty() && (data.back()=='\n'||data.back()=='\r'||data.back()=='\t'||data.back()==' ')) data.pop_back();
        Value value;
        if (!hw::parse(value,data) || !value.is<Object>() || value.get<Object>().size()!=2 ||
            !integer(field(value,"version"),1,1) || !integer(field(value,"percent"),0,100)) {
            *reason="invalid_state"; return false;
        }
        *percent=static_cast<int>(field(value,"percent").get<double>());
        { std::lock_guard<std::mutex> guard(mutex_); saved_=*percent; status_="saved"; }
        *reason="loaded"; return true;
    }
    void start() { worker_=std::thread([this]{run();}); }
    void load_failure(const std::string& reason) {
        std::lock_guard<std::mutex> guard(mutex_);
        if (reason!="missing") { status_="error"; error_=reason; }
    }
    void schedule(int percent) {
        std::lock_guard<std::mutex> guard(mutex_);
        if (!valid_ || percent<0 || percent>100) return;
        if (pending_<0 && saved_==percent && status_=="saved") return;
        if (pending_<0) due_=Clock::now()+std::chrono::seconds(2);
        pending_=percent; ++generation_; status_="pending"; cv_.notify_all();
    }
    Value status() {
        std::lock_guard<std::mutex> guard(mutex_);
        return object({{"enabled",Value(valid_)},{"status",str(status_)},
            {"saved_percent",saved_<0?Value():num(saved_)},{"error",error_.empty()?Value():str(error_)},
            {"failure_count",num(failures_)},{"last_failure",last_failure_.empty()?Value():str(last_failure_)}});
    }
private:
    using Clock=std::chrono::steady_clock;
    static bool safe_dir(const struct stat& stat) {
        return S_ISDIR(stat.st_mode) && stat.st_uid==geteuid() && !(stat.st_mode&0077);
    }
    static bool safe_file(const struct stat& stat) {
        return S_ISREG(stat.st_mode) && stat.st_uid==geteuid() && !(stat.st_mode&0077) && stat.st_nlink==1;
    }
    std::string save(int percent) {
        struct stat target{};
        if (fstatat(dir_,"volume.json",&target,AT_SYMLINK_NOFOLLOW)==0) {
            if (!safe_file(target)) return "unsafe_target";
        } else if (errno!=ENOENT) return "target_stat_failed";
        const std::string temporary="volume.tmp."+std::to_string(getpid())+"."+std::to_string(++serial_);
        int fd=openat(dir_,temporary.c_str(),O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600);
        if (fd<0) return "create_failed";
        const std::string bytes=object({{"version",num(1)},{"percent",num(percent)}}).serialize();
        bool ok=true; size_t offset=0;
        while (offset<bytes.size()) {
            ssize_t written=write(fd,bytes.data()+offset,bytes.size()-offset);
            if (written<0 && errno==EINTR) continue;
            if (written<=0) { ok=false; break; }
            offset+=static_cast<size_t>(written);
        }
        if (ok && fsync(fd)) ok=false;
        if (close(fd)) ok=false;
        if (!ok) { unlinkat(dir_,temporary.c_str(),0); return "write_or_sync_failed"; }
        if (renameat(dir_,temporary.c_str(),dir_,"volume.json")) {
            unlinkat(dir_,temporary.c_str(),0); return "rename_failed";
        }
        if (fsync(dir_)) return "directory_sync_uncertain";
        return {};
    }
    void run() {
        std::unique_lock<std::mutex> guard(mutex_);
        for (;;) {
            cv_.wait(guard,[this]{return stop_ || pending_>=0;});
            if (pending_<0 && stop_) return;
            if (!stop_ && Clock::now()<due_) {
                cv_.wait_until(guard,due_,[this]{return stop_;});
            }
            const int percent=pending_;
            const uint64_t generation=generation_;
            pending_=-1;
            guard.unlock();
            const std::string error=save(percent);
            guard.lock();
            if (error.empty()) saved_=percent;
            else { if (failures_<1000000000) ++failures_; last_failure_=error; }
            error_=error;
            status_=generation==generation_?(error.empty()?"saved":"error"):"pending";
            // No uncontrolled retries on failed flash; a new explicit command retries.
        }
    }
    int dir_=-1,lock_=-1,pending_=-1,saved_=-1;
    bool valid_=false,stop_=false;
    uint64_t serial_=0,generation_=0;
    unsigned failures_=0;
    std::string last_failure_;
    std::string status_="unsaved",error_;
    Clock::time_point due_{};
    std::mutex mutex_;
    std::condition_variable cv_;
    std::thread worker_;
};
}
