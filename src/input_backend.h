// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "controls.h"
#include <cerrno>
#include <dirent.h>
#include <fcntl.h>
#include <linux/input.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

namespace hw {
class EvdevButtons {
public:
    EvdevButtons():recognizer_({114,115,164,248}) {}
    ~EvdevButtons() { clear(); }
    EvdevButtons(const EvdevButtons&)=delete;
    EvdevButtons& operator=(const EvdevButtons&)=delete;
    void immediate_globals(bool enabled) { immediate_globals_=enabled; }
    void cancel_gestures() {
        recognizer_.reset();
        global_presses_.reset();
        timespec now{};
        clock_gettime(CLOCK_MONOTONIC,&now);
        cutoff_us_=static_cast<uint64_t>(now.tv_sec)*1000000+now.tv_nsec/1000;
    }
    bool available() const {
        for (const auto& device:devices_) if (device.fd>=0) return true;
        return false;
    }
    std::vector<unsigned> codes() const {
        std::vector<unsigned> result;
        for (const auto& device:devices_) if (device.fd>=0) {
            if (device.name=="gpio-keys") result.push_back(164);
            else result.insert(result.end(),{114,115,248});
        }
        return result;
    }
    // No EVIOCGRAB: caller must stop previous action handlers before activation.
    bool discover(const std::string& root="/dev/input") {
        clear();
        DIR* directory=opendir(root.c_str());
        if (!directory) return false;
        unsigned inspected=0;
        while (dirent* entry=readdir(directory)) {
            if (++inspected>128) break;
            std::string name=entry->d_name;
            if (name.size()<=5 || name.compare(0,5,"event") ||
                name.find_first_not_of("0123456789",5)!=std::string::npos) continue;
            int fd=open((root+"/"+name).c_str(),O_RDONLY|O_NONBLOCK|O_CLOEXEC);
            if (fd<0) continue;
            char identity[128]{};
            std::array<unsigned long,(KEY_MAX+8*sizeof(unsigned long))/(8*sizeof(unsigned long))> bits{};
            // musl uses int for ioctl requests, glibc uses unsigned long.
            if (ioctl(fd,static_cast<int>(EVIOCGNAME(sizeof(identity)-1)),identity)<0 ||
                ioctl(fd,static_cast<int>(EVIOCGBIT(EV_KEY,sizeof(bits))),bits.data())<0) { close(fd); continue; }
            const std::string id=identity;
            auto has=[&](unsigned key){ return (bits[key/(8*sizeof(unsigned long))]>>
                (key%(8*sizeof(unsigned long))))&1ul; };
            const bool match=(id=="gpio-keys" && has(164)) ||
                (id=="adc-keys" && has(114) && has(115) && has(248));
            if (!match) { close(fd); continue; }
            int clock_id=CLOCK_MONOTONIC;
            if (ioctl(fd,static_cast<int>(EVIOCSCLOCKID),&clock_id)<0) { close(fd); continue; }
            bool duplicate=false;
            for (const auto& device:devices_) if (device.name==id) duplicate=true;
            if (duplicate) { close(fd); closedir(directory); clear(); return false; }
            devices_.push_back({fd,id,false});
        }
        closedir(directory);
        return !devices_.empty();
    }
    struct Batch { std::vector<ButtonEvent> events; bool input_lost=false; };
    Batch poll(uint64_t now) {
        Batch result;
        for (auto& device:devices_) {
            if (device.fd<0) continue;
            for (unsigned n=0;n<64;++n) {
                input_event event{};
                ssize_t bytes=read(device.fd,&event,sizeof(event));
                if (bytes<0 && (errno==EAGAIN || errno==EINTR)) break;
                if (bytes!=sizeof(event)) {
                    close(device.fd); device.fd=-1; recognizer_.reset();
                    global_presses_.reset();
                    result.events.clear(); result.input_lost=true; break;
                }
                if (event.type==EV_SYN && event.code==SYN_DROPPED) {
                    recognizer_.reset(); device.dropped=true;
                    global_presses_.reset();
                    result.events.clear(); result.input_lost=true;
                } else if (device.dropped) {
                    if (event.type==EV_SYN && event.code==SYN_REPORT) device.dropped=false;
                } else if (event.type==EV_KEY) {
                    // Do not hand buffered input from an old lease to a new owner.
                    const uint64_t stamp=static_cast<uint64_t>(event.time.tv_sec)*1000000+
                        static_cast<uint64_t>(event.time.tv_usec);
                    if (stamp<=cutoff_us_) continue;
                    const bool mapped=(device.name=="gpio-keys" && event.code==164) ||
                        (device.name=="adc-keys" && (event.code==114 || event.code==115 || event.code==248));
                    if (mapped) {
                        if (immediate_globals_ && event.code!=164) {
                            if (global_presses_.edge(event.code,event.value,stamp/1000))
                                result.events.push_back({event.code,"press",now});
                            continue;
                        }
                        auto events=recognizer_.edge(event.code,event.value,now);
                        result.events.insert(result.events.end(),events.begin(),events.end());
                    }
                }
            }
        }
        auto timed=recognizer_.tick(now);
        result.events.insert(result.events.end(),timed.begin(),timed.end());
        return result;
    }
private:
    struct Device { int fd; std::string name; bool dropped; };
    void clear() { for (auto& device:devices_) if (device.fd>=0) close(device.fd);
        devices_.clear(); recognizer_.reset(); global_presses_.reset(); }
    std::vector<Device> devices_;
    ButtonRecognizer recognizer_;
    uint64_t cutoff_us_=0;
    bool immediate_globals_=false;
    GlobalPresses global_presses_;
};
} // namespace hw
