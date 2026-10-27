// SPDX-License-Identifier: GPL-3.0-only
#include "connection.h"
#include <iostream>
#include <csignal>
using namespace hw;
static volatile sig_atomic_t interrupted = 0;
static void stop_watch(int) { interrupted = 1; }
int main(int argc, char** argv) {
    if (argc < 4 || argc > 5 || std::string(argv[1]) != "--socket") {
        std::cerr << "usage: lotus-hwctl --socket PATH METHOD [JSON_PARAMS]\n"
                     "       lotus-hwctl --socket PATH session < commands.jsonl\n"
                     "       lotus-hwctl --socket PATH watch\n"; return 2;
    }
    Connection client; std::string error;
    if (!client.connect_to(argv[2], &error)) { std::cerr << error << '\n'; return 1; }
    auto run = [&](const std::string& method, const Value& params) {
        Value response;
        if (!client.request(method,params,&response,&error)) { std::cerr << error << '\n'; return 1; }
        std::cout << response.serialize() << std::endl;
        return field(response,"ok").is<bool>() && field(response,"ok").get<bool>() ? 0 : 1;
    };
    if (std::string(argv[3]) == "watch") {
        if (argc != 4) return 2;
        if (run("events.subscribe",object({}))) return 1;
        std::signal(SIGINT,stop_watch); std::signal(SIGTERM,stop_watch);
        while (!interrupted) {
            Value event;
            if (client.next_event(&event,&error,500)) std::cout << event.serialize() << std::endl;
            else if (error != "timeout") { std::cerr << error << '\n'; return 1; }
        }
        return 0;
    }
    if (std::string(argv[3]) == "session") {
        if (argc != 4) return 2;
        std::string line;
        while (std::getline(std::cin,line)) {
            Value command;
            if (!hw::parse(command,line) || !field(command,"method").is<std::string>() ||
                !field(command,"params").is<Object>()) { std::cerr << "invalid_command\n"; return 2; }
            const int result = run(field(command,"method").get<std::string>(),field(command,"params"));
            if (result) return result;
        }
        return 0;
    }
    Value params = object({});
    if (argc == 5 && (!hw::parse(params,argv[4]) || !params.is<Object>())) {
        std::cerr << "params must be a JSON object\n"; return 2;
    }
    return run(argv[3],params);
}
