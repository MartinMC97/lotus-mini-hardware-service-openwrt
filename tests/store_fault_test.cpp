// SPDX-License-Identifier: GPL-3.0-only
// Linker-injected fsync faults; never exposes failure switches in the daemon API.
#include "volume_store.h"
#include <atomic>
#include <cassert>
#include <fstream>
#include <iostream>
static std::atomic<int> fail_after{-1};
extern "C" int __real_fsync(int);
extern "C" int __wrap_fsync(int fd) {
    if (fail_after.load()>=0 && fail_after.fetch_sub(1)==0) { errno=EIO; return -1; }
    return __real_fsync(fd);
}
static hw::Value wait_saved(hw::VolumeStore& store) {
    for (unsigned i=0;i<160;++i) {
        const auto value=store.status();
        if (hw::field(value,"status")!=hw::str("pending")) return value;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    assert(false); return {};
}
static int disk_percent(const std::string& path) {
    std::ifstream stream(path); std::string data;
    std::getline(stream,data);
    hw::Value value; assert(hw::parse(value,data));
    return static_cast<int>(hw::field(value,"percent").get<double>());
}
int main() {
    char pattern[]="/tmp/lotus-volume-fault.XXXXXX";
    const char* directory=mkdtemp(pattern); assert(directory);
    const std::string target=std::string(directory)+"/volume.json";
    int fd=open(target.c_str(),O_WRONLY|O_CREAT|O_EXCL,0600); assert(fd>=0);
    const std::string data="{\"version\":1,\"percent\":35}";
    assert(write(fd,data.data(),data.size())==static_cast<ssize_t>(data.size())); close(fd);
    {
        hw::VolumeStore store(directory); assert(store.valid());
        int initial=0; std::string reason;
        assert(store.load(&initial,&reason) && initial==35);
        store.start();
        fail_after=0; store.schedule(50);
        auto status=wait_saved(store);
        assert(hw::field(status,"error")==hw::str("write_or_sync_failed"));
        assert(hw::field(status,"failure_count")==hw::num(1));
        assert(disk_percent(target)==35); // old file survives failure before rename
        store.schedule(60); status=wait_saved(store);
        assert(hw::field(status,"status")==hw::str("saved"));
        assert(disk_percent(target)==60);
        fail_after=1; store.schedule(70); status=wait_saved(store);
        assert(hw::field(status,"error")==hw::str("directory_sync_uncertain"));
        assert(hw::field(status,"saved_percent")==hw::num(60));
        assert(hw::field(status,"failure_count")==hw::num(2));
        assert(disk_percent(target)==70); // rename visible, durability uncertain
        store.schedule(70); store.finish();
        assert(hw::field(store.status(),"status")==hw::str("saved"));
        assert(hw::field(store.status(),"last_failure")==hw::str("directory_sync_uncertain"));
    }
    assert(unlink(target.c_str())==0);
    assert(unlink((std::string(directory)+"/volume.lock").c_str())==0);
    assert(rmdir(directory)==0); // also detects leaked temporary files
    std::cout<<"volume store fault tests passed\n";
}
