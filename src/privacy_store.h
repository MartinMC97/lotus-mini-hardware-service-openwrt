// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "protocol.h"
#include <cerrno>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cstdio>

namespace hw {
// Synchronous durable primitive: call only from a persistence worker, never
// from the socket loop. No default file creation, debounce or automatic retry.
class PrivacyStore {
public:
    explicit PrivacyStore(const std::string& directory) {
        struct stat before{},after{};
        if (lstat(directory.c_str(),&before) || !safe_dir(before)) return;
        dir_=open(directory.c_str(),O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
        if (dir_<0 || fstat(dir_,&after) || !safe_dir(after) ||
            before.st_ino!=after.st_ino || before.st_dev!=after.st_dev) return;
        lock_=openat(dir_,"privacy.lock",O_RDWR|O_CREAT|O_NONBLOCK|O_NOFOLLOW|O_CLOEXEC,0600);
        struct stat lock_stat{};
        if (lock_<0 || fstat(lock_,&lock_stat) || !safe_file(lock_stat) || flock(lock_,LOCK_EX|LOCK_NB)) return;
        valid_=true;
    }
    ~PrivacyStore() { if (lock_>=0) close(lock_); if (dir_>=0) close(dir_); }
    PrivacyStore(const PrivacyStore&)=delete;
    PrivacyStore& operator=(const PrivacyStore&)=delete;
    bool valid() const { return valid_; }
    bool load(bool* muted,std::string* reason) {
        *muted=true;
        if (!valid_) { *reason="unsafe_or_locked_directory"; return false; }
        int fd=openat(dir_,"privacy.json",O_RDONLY|O_NONBLOCK|O_NOFOLLOW|O_CLOEXEC);
        if (fd<0) { *reason=errno==ENOENT?"missing":"open_failed"; return false; }
        struct stat st{};
        if (fstat(fd,&st) || !safe_file(st) || st.st_size>256) {
            close(fd); *reason="unsafe_state_file"; return false;
        }
        char buffer[257]; ssize_t n;
        do { n=read(fd,buffer,sizeof(buffer)); } while (n<0 && errno==EINTR);
        close(fd);
        if (n<=0 || n>256) { *reason="invalid_state"; return false; }
        std::string data(buffer,static_cast<size_t>(n));
        while (!data.empty() && (data.back()=='\n'||data.back()=='\r'||data.back()=='\t'||data.back()==' ')) data.pop_back();
        Value value;
        if (!hw::parse(value,data) || !value.is<Object>() || value.get<Object>().size()!=2 ||
            !integer(field(value,"version"),1,1) || !field(value,"muted").is<bool>()) {
            *reason="invalid_state"; return false;
        }
        *muted=field(value,"muted").get<bool>(); *reason="loaded"; return true;
    }
    std::string save(bool muted) {
        if (!valid_) return "unsafe_or_locked_directory";
        struct stat target{};
        if (fstatat(dir_,"privacy.json",&target,AT_SYMLINK_NOFOLLOW)==0) {
            if (!safe_file(target)) return "unsafe_target";
        } else if (errno!=ENOENT) return "target_stat_failed";
        const std::string temporary="privacy.tmp."+std::to_string(getpid())+"."+std::to_string(++serial_);
        int fd=openat(dir_,temporary.c_str(),O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600);
        if (fd<0) return "create_failed";
        const std::string bytes=object({{"version",num(1)},{"muted",Value(muted)}}).serialize();
        bool ok=true; size_t offset=0;
        while (offset<bytes.size()) {
            ssize_t n=write(fd,bytes.data()+offset,bytes.size()-offset);
            if (n<0 && errno==EINTR) continue;
            if (n<=0) { ok=false; break; }
            offset+=static_cast<size_t>(n);
        }
        if (ok && fsync(fd)) ok=false;
        if (close(fd)) ok=false;
        if (!ok) { unlinkat(dir_,temporary.c_str(),0); return "write_or_sync_failed"; }
        if (renameat(dir_,temporary.c_str(),dir_,"privacy.json")) {
            unlinkat(dir_,temporary.c_str(),0); return "rename_failed";
        }
        if (fsync(dir_)) return "directory_sync_uncertain";
        return {};
    }
private:
    static bool safe_dir(const struct stat& s) {
        return S_ISDIR(s.st_mode) && s.st_uid==geteuid() && !(s.st_mode&0077);
    }
    static bool safe_file(const struct stat& s) {
        return S_ISREG(s.st_mode) && s.st_uid==geteuid() && !(s.st_mode&0077) && s.st_nlink==1;
    }
    int dir_=-1,lock_=-1;
    bool valid_=false;
    uint64_t serial_=0;
};
}
