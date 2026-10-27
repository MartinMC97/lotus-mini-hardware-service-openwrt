// SPDX-License-Identifier: GPL-3.0-only
#include "controls.h"
#include "led_backend.h"
#include "input_backend.h"
#include "volume.h"
#include <cassert>
#include <mutex>
#include <iostream>

int main() {
    hw::GlobalPresses presses;
    assert(presses.edge(115,1,100));
    assert(!presses.edge(115,1,101) && !presses.edge(115,2,102));
    assert(!presses.edge(115,0,120) && !presses.edge(115,1,121));
    assert(!presses.edge(115,0,125) && presses.edge(115,1,150));
    assert(presses.edge(248,1,100) && !presses.edge(248,0,150));
    assert(!presses.edge(164,1,100) && !presses.edge(9999,1,100));
    presses.reset(); assert(presses.edge(115,1,100));
    using namespace hw;
    for (int percent=0;percent<=100;++percent) {
        assert(volume_raw(percent)>=0 && volume_raw(percent)<=255);
        assert(volume_percent(volume_raw(percent))==percent);
    }
    LedEffect effect{LedEffect::Solid,0xffffff,0,100,1000};
    assert((led_duties(effect,0)==std::array<unsigned,3>{{10000,10000,10000}}));
    effect.kind=LedEffect::Breath;
    assert(led_duties(effect,0)[0]==0);
    assert(led_duties(effect,500)[0]==10000);
    assert(led_duties(effect,1000)[0]==0);
    effect={LedEffect::Gradient,0x0000ff,0x00ffff,100,1000};
    assert((led_duties(effect,0)==std::array<unsigned,3>{{0,0,10000}}));
    assert((led_duties(effect,500)==std::array<unsigned,3>{{0,10000,10000}}));
    assert(led_duties(effect,250)[1]==5000);
    std::mutex mutex;
    std::vector<unsigned> writes;
    {
        LedRunner runner([&](unsigned mask){std::lock_guard<std::mutex> lock(mutex);
            writes.push_back(mask); return true;});
        for (unsigned i=0;i<50;++i) {
            assert(runner.set(effect));
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            assert(runner.set({}));
            size_t boundary;
            { std::lock_guard<std::mutex> lock(mutex); boundary=writes.size(); }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            { std::lock_guard<std::mutex> lock(mutex);
              for (size_t j=boundary;j<writes.size();++j) assert(writes[j]==0); }
        }
    }
    assert(writes.back()==0);
    { LedRunner failing([](unsigned){return false;}); assert(!failing.set(effect));
      assert(!failing.healthy()); }
    ButtonRecognizer buttons({114,115,164,248});
    assert(buttons.edge(999,1,0).empty());
    assert(buttons.edge(164,1,0).empty());
    assert(buttons.edge(164,2,50).empty());
    assert(buttons.edge(164,0,100).empty());
    assert(buttons.tick(399).empty());
    auto events=buttons.tick(400);
    assert(events.size()==1 && events[0].gesture=="short");
    assert(buttons.tick(500).empty());
    buttons.edge(164,1,600); buttons.edge(164,0,700);
    buttons.edge(164,1,800); events=buttons.edge(164,0,900);
    assert(events.size()==1 && events[0].gesture=="double");
    assert(buttons.tick(1300).empty());
    buttons.edge(248,1,1400); events=buttons.tick(2200);
    assert(events.size()==1 && events[0].gesture=="long");
    assert(buttons.tick(2300).empty()); assert(buttons.edge(248,0,2400).empty());
    assert(buttons.tick(2800).empty());
    buttons.edge(114,1,3000); buttons.edge(114,0,3005);
    assert(buttons.tick(3400).empty());
    buttons.edge(115,1,3500); buttons.reset();
    assert(buttons.edge(115,0,4500).empty());
    assert(buttons.tick(5000).empty());
    buttons.edge(164,1,5100);
    events=buttons.edge(164,0,6000); // missed timer tick still emits exactly once
    assert(events.size()==1 && events[0].gesture=="long");
    assert(buttons.tick(6400).empty());
    buttons.edge(164,1,6500); buttons.edge(164,0,6600);
    buttons.edge(164,1,6610); buttons.edge(164,0,6620); // contact bounce
    events=buttons.tick(6900);
    assert(events.size()==1 && events[0].gesture=="short");
    SysfsRgb missing("/nonexistent-lotus-hw-test"); assert(!missing.write(7));
    EvdevButtons input; assert(!input.discover("/nonexistent-lotus-hw-test"));
    assert(input.poll(0).events.empty());
    std::cout << "controls tests passed\n";
}
