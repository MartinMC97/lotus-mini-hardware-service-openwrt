// SPDX-License-Identifier: GPL-3.0-only
#include "application_client.h"
#include <cassert>
#include <iostream>
int main(int argc,char** argv) {
    if (argc!=2) return 2;
    hw::ApplicationClient client; std::string error; hw::Value result;
    assert(client.start(argv[1],&error)); assert(client.ready()); assert(client.muted());
    assert(client.request("privacy.set",hw::object({{"muted",hw::Value(false)}}),&result,&error));
    for (unsigned i=0;i<100 && client.muted();++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    assert(!client.muted());
    assert(client.request("volume.adjust",hw::object({{"delta",hw::num(5)}}),&result,&error));
    assert(hw::field(client.state(),"volume_percent")==hw::num(40));
    const auto idle=hw::object({{"effect",hw::str("breath")},{"color",hw::num(0x18bcf2)},
        {"brightness_percent",hw::num(25)},{"period_ms",hw::num(100)},
        {"duration_ms",hw::num(150)}});
    bool saw_pulse=false;
    for (unsigned i=0;i<12;++i) {
        // Repeated identical idle reports must not keep postponing heartbeat.
        assert(client.led(idle,200,&error));
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        assert(client.request("state.get",hw::object({}),&result,&error));
        saw_pulse=saw_pulse || hw::field(hw::field(result,"led"),"effect")==hw::str("breath");
    }
    assert(saw_pulse);
    assert(client.led(hw::object({{"effect",hw::str("breath")},{"color",hw::num(0x18bcf2)},
        {"brightness_percent",hw::num(25)},{"period_ms",hw::num(100)},
        {"duration_ms",hw::num(100)}}),0,&error));
    std::cout<<"ready"<<std::endl;
    char input; std::cin.get(input); // harness terminates daemon, then releases us
    for (unsigned i=0;i<100 && client.ready();++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    assert(!client.ready() && client.muted());
    assert(!client.request("volume.adjust",hw::object({{"delta",hw::num(5)}}),&result,&error));
    client.stop(); std::cout<<"application client tests passed\n";
}
