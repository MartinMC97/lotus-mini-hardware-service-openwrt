// SPDX-License-Identifier: GPL-3.0-only
#include "connection.h"
#include <iostream>
using namespace hw;
int main(int argc,char** argv) {
    if (argc!=2) return 2;
    Connection c; std::string error; Value event,response;
    if (!c.connect_to(argv[1],&error) || !c.next_event(&event,&error) ||
        field(event,"event").get<std::string>()!="snapshot") return 1;
    if (!c.request("events.subscribe",object({}),&response,&error)) return 1;
    if (!c.request("volume.set",object({{"percent",num(48)}}),&response,&error)) return 1;
    if (!c.next_event(&event,&error) || field(event,"event").get<std::string>()!="volume.changed") return 1;
    if (c.next_event(&event,&error,60) || error!="timeout") return 1;
    // An idle event poll timeout must not disconnect or release an owned lease.
    if (!c.request("state.get",object({}),&response,&error)) return 1;
    std::cout << "connection tests passed\n"; return 0;
}
