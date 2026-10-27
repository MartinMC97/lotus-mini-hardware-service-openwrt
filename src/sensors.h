// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "protocol.h"
#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <memory>
#include <vector>
namespace hw {
struct SensorSnapshot {
    Value battery = object({{"available",Value(false)}});
    Value temperature = object({{"available",Value(false)}});
};
class SensorBackend {
public:
    virtual ~SensorBackend() = default;
    virtual SensorSnapshot read() const = 0;
};
class MockSensors : public SensorBackend {
public:
    SensorSnapshot read() const override { return {}; }
};
class SysfsSensors : public SensorBackend {
public:
    explicit SysfsSensors(std::string root) : root_(std::move(root)) {}
    SensorSnapshot read() const override {
        SensorSnapshot snapshot;
        std::string capacity,status;
        int percent = 0;
        const bool valid = read_text(root_+"/class/power_supply/lotus-battery/capacity",&capacity) &&
            parse_integer(capacity,0,100,&percent) && percent%25==0;
        const bool status_read = read_text(root_+"/class/power_supply/lotus-battery/status",&status);
        const bool status_valid = status_read && (status=="Charging" || status=="Discharging" ||
            status=="Not charging" || status=="Full" || status=="Unknown");
        Value external;
        if (status_valid && status!="Unknown") external=Value(status!="Discharging");
        snapshot.battery=object({{"available",Value(valid)}, {"percent",valid?num(percent):Value()},
            {"resolution",str("five_step")}, {"status",status_valid?str(status):Value()},
            {"external_power_connected",external},
            {"error",valid?Value():str("capacity_unavailable_or_invalid")},
            {"status_error",status_valid?Value():str("status_unavailable_or_invalid")}});
        const std::string directory=root_+"/class/thermal";
        std::vector<std::string> zones;
        DIR* dir=opendir(directory.c_str());
        if (dir) {
            while (auto* entry=readdir(dir)) {
                const std::string name=entry->d_name;
                if (name.rfind("thermal_zone",0)==0 && name.size()>12 &&
                    name.find_first_not_of("0123456789",12)==std::string::npos && zones.size()<128)
                    zones.push_back(name);
            }
            closedir(dir);
        }
        std::sort(zones.begin(),zones.end());
        picojson::array readings;
        bool soc_available=false;
        for (const char* wanted : {"soc-thermal","gpu-thermal"}) {
            std::vector<std::string> matches;
            for (const auto& zone:zones) {
                std::string type;
                if (read_text(directory+"/"+zone+"/type",&type) && type==wanted) matches.push_back(zone);
            }
            std::string temp; int millidegrees=0;
            const bool available=matches.size()==1 &&
                read_text(directory+"/"+matches.front()+"/temp",&temp) &&
                parse_integer(temp,-40000,150000,&millidegrees);
            if (std::string(wanted)=="soc-thermal") soc_available=available;
            readings.push_back(object({{"type",str(wanted)}, {"available",Value(available)},
                {"millidegrees_celsius",available?num(millidegrees):Value()},
                {"error",available?Value():str(matches.size()>1?"ambiguous_type":"temperature_unavailable_or_invalid")}}));
        }
        snapshot.temperature=object({{"available",Value(soc_available)}, {"readings",Value(readings)}});
        return snapshot;
    }
private:
    static bool read_text(const std::string& path,std::string* value) {
        const int fd=open(path.c_str(),O_RDONLY|O_CLOEXEC|O_NONBLOCK);
        if (fd<0) return false;
        struct stat statbuf{};
        if (fstat(fd,&statbuf) || !S_ISREG(statbuf.st_mode)) { close(fd); return false; }
        value->clear(); char buffer[64]; bool success=true;
        for (;;) {
            const ssize_t n=::read(fd,buffer,sizeof(buffer));
            if (n<0 && errno==EINTR) continue;
            if (n<0 || (n>0 && value->size()+static_cast<size_t>(n)>128)) { success=false; break; }
            if (!n) break;
            value->append(buffer,static_cast<size_t>(n));
        }
        close(fd);
        if (!success || value->empty()) return false;
        while (!value->empty() && (value->back()=='\n' || value->back()=='\r')) value->pop_back();
        return !value->empty();
    }
    static bool parse_integer(const std::string& value,int low,int high,int* result) {
        if (value.empty() || value.find_first_not_of("-0123456789")!=std::string::npos) return false;
        char* end=nullptr; errno=0;
        const long n=std::strtol(value.c_str(),&end,10);
        if (errno || end==value.c_str() || *end || n<low || n>high) return false;
        *result=static_cast<int>(n); return true;
    }
    std::string root_;
};
}
