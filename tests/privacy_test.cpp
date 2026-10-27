// SPDX-License-Identifier: GPL-3.0-only
#include "privacy.h"
#include "privacy_store.h"
#include <atomic>
#include <cassert>
#include <iostream>

static std::atomic<int> fail_after{-1};
extern "C" int __real_fsync(int);
extern "C" int __wrap_fsync(int fd) {
    if (fail_after.load()>=0 && fail_after.fetch_sub(1)==0) { errno=EIO; return -1; }
    return __real_fsync(fd);
}
static void write_file(const std::string& path,const std::string& bytes,mode_t mode=0600) {
    int fd=open(path.c_str(),O_WRONLY|O_CREAT|O_EXCL,mode); assert(fd>=0);
    assert(write(fd,bytes.data(),bytes.size())==static_cast<ssize_t>(bytes.size()));
    assert(close(fd)==0);
}
int main() {
    hw::PrivacyState state;
    assert(state.muted());
    state.restore(false,false); assert(state.muted());
    state.restore(true,false); assert(!state.muted());
    const auto unmute=state.request(false); assert(state.muted());
    const auto mute=state.request(true);
    assert(!state.complete(unmute,"") && state.muted()); // stale success cannot unmute
    assert(state.complete(mute,"write_or_sync_failed") && state.muted());
    const auto retry=state.request(false);
    assert(state.complete(retry,"directory_sync_uncertain") && state.muted());
    const auto success=state.request(false);
    assert(state.complete(success,"") && !state.muted());
    assert(!state.complete(success,"late_failure")); // duplicate completion ignored
    state.request(true); assert(state.muted());

    hw::PrivacyGate gate;
    const auto unmuted=hw::object({{"microphone_muted",hw::Value(false)},
        {"privacy_enforcement",hw::str("cooperative")}});
    gate.consent(true); assert(!gate.allowed());
    assert(gate.snapshot(unmuted) && !gate.allowed());
    gate.consent(true); assert(gate.allowed());
    assert(gate.changed(hw::object({{"muted",hw::Value(true)}})) && !gate.allowed());
    gate.consent(false);
    assert(gate.changed(hw::object({{"muted",hw::Value(false)}})) && !gate.allowed());
    gate.consent(true); assert(gate.allowed());
    gate.lost(); assert(!gate.allowed());
    assert(!gate.changed(hw::object({{"muted",hw::Value(false)}})));
    assert(gate.snapshot(unmuted) && !gate.allowed()); // no reconnect consent reuse
    gate.consent(true);
    assert(!gate.changed(hw::object({{"muted",hw::num(0)}})) && !gate.allowed());
    assert(!gate.snapshot(hw::object({{"microphone_muted",hw::Value(false)},
        {"privacy_enforcement",hw::str("unavailable")}})));

    char pattern[]="/tmp/lotus-privacy-test.XXXXXX";
    const char* directory=mkdtemp(pattern); assert(directory);
    const std::string root=directory,target=root+"/privacy.json";
    {
        hw::PrivacyStore store(root); assert(store.valid());
        hw::PrivacyStore second(root); assert(!second.valid());
        bool muted=false; std::string reason;
        assert(!store.load(&muted,&reason) && muted && reason=="missing");
        assert(access(target.c_str(),F_OK)!=0); // no default write
        assert(store.save(false).empty());
        assert(store.load(&muted,&reason) && !muted);
        auto ticket=state.request(true);
        fail_after=0;
        const auto error=store.save(true);
        assert(error=="write_or_sync_failed");
        assert(state.complete(ticket,error) && state.muted());
        assert(store.load(&muted,&reason) && !muted); // failed mute is not durable!
        assert(store.save(true).empty());
        assert(store.load(&muted,&reason) && muted);
        ticket=state.request(false); fail_after=1;
        const auto uncertain=store.save(false);
        assert(uncertain=="directory_sync_uncertain");
        assert(state.complete(ticket,uncertain) && state.muted());
        ticket=state.request(false);
        assert(state.complete(ticket,store.save(false)) && !state.muted());
    }
    {
        hw::PrivacyStore store(root); bool muted=true; std::string reason;
        assert(store.load(&muted,&reason) && !muted); // service restart
        const std::string invalid[]={"{}","{\"version\":1,\"muted\":0}",
            "{\"version\":1,\"muted\":false,\"extra\":1}",
            "{\"version\":1,\"muted\":false,\"muted\":true}",
            "{\"version\":2,\"muted\":false}",std::string(257,'x')};
        for (const auto& bytes:invalid) {
            assert(unlink(target.c_str())==0); write_file(target,bytes);
            muted=false; assert(!store.load(&muted,&reason) && muted);
        }
        assert(unlink(target.c_str())==0);
        write_file(target,"{\"version\":1,\"muted\":false}",0644);
        assert(chmod(target.c_str(),0644)==0);
        muted=false; assert(!store.load(&muted,&reason) && muted);
        assert(store.save(true)=="unsafe_target");
        assert(unlink(target.c_str())==0);
        assert(symlink("nonexistent",target.c_str())==0);
        assert(!store.load(&muted,&reason) && muted);
        assert(store.save(true)=="unsafe_target");
        assert(unlink(target.c_str())==0);
        assert(mkfifo(target.c_str(),0600)==0);
        assert(!store.load(&muted,&reason) && muted);
        assert(store.save(true)=="unsafe_target");
        assert(unlink(target.c_str())==0);
        assert(store.save(true).empty()); // explicit repair only
        assert(store.load(&muted,&reason) && muted);
    }
    assert(chmod(root.c_str(),0755)==0);
    { hw::PrivacyStore unsafe(root); assert(!unsafe.valid()); }
    assert(chmod(root.c_str(),0700)==0);
    assert(unlink(target.c_str())==0);
    assert(unlink((root+"/privacy.lock").c_str())==0);
    assert(rmdir(root.c_str())==0); // no leaked temporary files
    std::cout<<"privacy policy, gate, store and fsync fault tests passed\n";
}
