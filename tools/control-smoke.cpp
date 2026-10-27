// SPDX-License-Identifier: GPL-3.0-only
// No ALSA, audio assets, recording or assistant triggers in this test client.
#include "connection.h"
#include <iostream>
#include <thread>
using namespace hw;
int main(int argc,char** argv) {
    if (argc!=3 || std::string(argv[1])!="--socket") return 2;
    Connection owner,observer; std::string error;
    if (!owner.connect_to(argv[2],&error) || !observer.connect_to(argv[2],&error)) {
        std::cerr<<error<<'\n'; return 1;
    }
    auto request=[&](Connection& client,const std::string& method,Value params,bool expected=true) {
        Value response;
        if (!client.request(method,params,&response,&error)) { std::cerr<<error<<'\n'; return false; }
        std::cout<<response.serialize()<<std::endl;
        if (!expected && field(field(response,"error"),"code")!=str("busy")) return false;
        return field(response,"ok").is<bool>() && field(response,"ok").get<bool>()==expected;
    };
    if (!request(owner,"capabilities.get",object({})) || !request(owner,"state.get",object({})) ||
        !request(owner,"lease.acquire",object({{"ttl_ms",num(60000)}})) ||
        !request(observer,"lease.acquire",object({{"ttl_ms",num(1000)}}),false) ||
        !request(observer,"events.subscribe",object({}))) return 1;
    for (uint32_t color:{0xffffffu,0xff0000u,0x00ff00u,0x0000ffu}) {
        std::cout<<"stage: solid "<<color<<std::endl;
        if (!request(owner,"led.set",object({{"effect",str("solid")},{"color",num(color)},
            {"brightness_percent",num(25)}}))) return 1;
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    for (const char* effect:{"breath","gradient"}) {
        std::cout<<"stage: "<<effect<<std::endl;
        if (!request(owner,"led.set",object({{"effect",str(effect)},{"color",num(0x0000ff)},
            {"color_2",num(0x00ffff)},{"brightness_percent",num(25)},
            {"period_ms",num(2000)}}))) return 1;
        std::this_thread::sleep_for(std::chrono::seconds(2));
    }
    if (!request(owner,"led.set",object({{"effect",str("off")}}))) return 1;
    std::cout<<"stage: observe_buttons_30_seconds_no_actions_no_audio"<<std::endl;
    const auto end=std::chrono::steady_clock::now()+std::chrono::seconds(30);
    while (std::chrono::steady_clock::now()<end) {
        for (Connection* client:{&owner,&observer}) {
            Value event;
            if (client->next_event(&event,&error,100)) {
                std::cout<<(client==&owner?"owner: ":"observer: ")<<event.serialize()<<std::endl;
                if (client==&observer && field(event,"event")==str("button.action")) {
                    std::cerr<<"observer received an action\n"; return 1;
                }
            } else if (error!="timeout") { std::cerr<<error<<'\n'; return 1; }
        }
    }
    if (!request(owner,"lease.release",object({})) ||
        !request(observer,"lease.acquire",object({{"ttl_ms",num(1000)}})) ||
        !request(observer,"lease.release",object({}))) return 1;
    std::cout<<"smoke_passed; physical LED/button acceptance requires observation"<<std::endl;
}
