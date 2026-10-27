// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace hw {
struct LedEffect {
    enum Kind { Off, Solid, Breath, Gradient } kind = Off;
    uint32_t color = 0, color2 = 0;
    unsigned brightness = 100, period_ms = 1000;
};

// Pure, monotonic-time evaluator. Scheduling/idle policy belongs to clients.
inline std::array<unsigned,3> led_duties(const LedEffect& e, uint64_t elapsed_ms) {
    std::array<unsigned,3> result{};
    if (e.kind == LedEffect::Off) return result;
    const double phase = static_cast<double>(elapsed_ms % std::max(1u,e.period_ms)) /
                         std::max(1u,e.period_ms);
    const double blend = (1.0-std::cos(phase*6.283185307179586))/2.0;
    for (unsigned i=0; i<3; ++i) {
        const unsigned shift=(2-i)*8;
        double value=(e.color>>shift)&255;
        if (e.kind==LedEffect::Breath) value*=blend;
        if (e.kind==LedEffect::Gradient)
            value=value*(1-blend)+((e.color2>>shift)&255)*blend;
        result[i]=static_cast<unsigned>(std::lround(value*std::min(100u,e.brightness)*100/255));
    }
    return result; // 0..10000, a 10ms PWM carrier gives 100Hz.
}

struct ButtonEvent { unsigned code; std::string gesture; uint64_t time_ms; };
class GlobalPresses {
public:
    bool edge(unsigned code,int value,uint64_t now) {
        const int index=code==114?0:code==115?1:code==248?2:-1;
        if (index<0 || (value!=0 && value!=1)) return false;
        if (!value) {
            if (down_[index]) { released_[index]=now; seen_release_[index]=true; }
            down_[index]=false; return false;
        }
        if (down_[index] || (seen_release_[index] && now-released_[index]<30)) return false;
        down_[index]=true; return true;
    }
    void reset() { down_={}; seen_release_={}; released_={}; }
private:
    std::array<bool,3> down_{},seen_release_{};
    std::array<uint64_t,3> released_{};
};
class ButtonRecognizer {
public:
    // Bound to a fixed profile: unknown keycodes cannot grow state indefinitely.
    explicit ButtonRecognizer(std::vector<unsigned> codes) {
        for (unsigned code:codes) states_.emplace(code,State{});
    }
    std::vector<ButtonEvent> edge(unsigned code, int value, uint64_t now) {
        std::vector<ButtonEvent> out;
        auto it=states_.find(code);
        if (it==states_.end() || (value!=0 && value!=1)) return out; // ignore repeat=2
        auto& s=it->second;
        if (s.down==(value==1)) return out;
        if (value==1) {
            // Ignore bounce after release; no phantom second click.
            if (s.seen_release && now-s.released<30) return out;
            flush(code,s,now,out);
            s.down=true; s.pressed=now; s.long_sent=false;
        } else {
            s.down=false; s.released=now; s.seen_release=true;
            if (now-s.pressed<30) return out;
            if (!s.long_sent && now-s.pressed>=800) {
                s.clicks=0; out.push_back({code,"long",now});
            } else if (!s.long_sent) {
                if (++s.clicks==2) { out.push_back({code,"double",now}); s.clicks=0; }
            }
        }
        return out;
    }
    std::vector<ButtonEvent> tick(uint64_t now) {
        std::vector<ButtonEvent> out;
        for (auto& pair:states_) {
            auto& s=pair.second;
            if (s.down && !s.long_sent && now-s.pressed>=800) {
                s.long_sent=true; s.clicks=0; out.push_back({pair.first,"long",now});
            }
            flush(pair.first,s,now,out);
        }
        return out;
    }
    // Input loss/SYN_DROPPED cancels pending gestures; never fabricate releases.
    void reset() { for (auto& pair:states_) pair.second=State{}; }
private:
    struct State {
        bool down=false,long_sent=false,seen_release=false;
        unsigned clicks=0;
        uint64_t pressed=0,released=0;
    };
    static void flush(unsigned code, State& s, uint64_t now,std::vector<ButtonEvent>& out) {
        if (!s.down && s.clicks && now-s.released>=300) {
            out.push_back({code,"short",now}); s.clicks=0;
        }
    }
    std::map<unsigned,State> states_;
};
} // namespace hw
