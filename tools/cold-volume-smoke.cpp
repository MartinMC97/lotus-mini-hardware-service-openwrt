// SPDX-License-Identifier: GPL-3.0-only
// Explicit test only: remove this known user control with both PCMs closed.
// Restart hardwared --initialize-volume afterwards; this never opens PCM.
#include <alsa/asoundlib.h>
#include <dirent.h>
#include <fstream>
#include <iostream>
#include <string>
static bool text_is(const char* path,const char* expected) {
    std::ifstream in(path); std::string value;
    return bool(in >> value) && value==expected;
}
int main(int argc,char** argv) {
    if (argc!=2 || std::string(argv[1])!="--remove-unused-ha-volume") return 2;
    DIR* processes=opendir("/proc");
    if (!processes) return 1;
    bool owner=false;
    while (dirent* entry=readdir(processes)) {
        const std::string name=entry->d_name;
        if (name.empty() || name.find_first_not_of("0123456789")!=std::string::npos) continue;
        const std::string comm="/proc/"+name+"/comm";
        if (text_is(comm.c_str(),"lotus-hardwared")) { owner=true; break; }
    }
    closedir(processes);
    if (owner) { std::cerr<<"Refused: hardware service still running\n"; return 1; }
    if (!text_is("/proc/asound/card0/id","LotusMini") ||
        !text_is("/proc/asound/card0/pcm0c/sub0/status","closed") ||
        !text_is("/proc/asound/card0/pcm0p/sub0/status","closed")) {
        std::cerr<<"Refused: exact card/closed PCM checks failed\n"; return 1;
    }
    snd_ctl_t* ctl=nullptr;
    if (snd_ctl_open(&ctl,"hw:LotusMini",SND_CTL_NONBLOCK)<0) return 1;
    snd_ctl_elem_info_t* info; snd_ctl_elem_info_alloca(&info);
    snd_ctl_elem_info_set_interface(info,SND_CTL_ELEM_IFACE_MIXER);
    snd_ctl_elem_info_set_name(info,"HA Volume");
    bool valid=snd_ctl_elem_info(ctl,info)>=0 && snd_ctl_elem_info_is_user(info) &&
        snd_ctl_elem_info_get_type(info)==SND_CTL_ELEM_TYPE_INTEGER &&
        snd_ctl_elem_info_get_count(info)==2 && snd_ctl_elem_info_get_min(info)==0 &&
        snd_ctl_elem_info_get_max(info)==255;
    snd_ctl_elem_id_t* id; snd_ctl_elem_id_alloca(&id); snd_ctl_elem_info_get_id(info,id);
    const bool removed=valid && snd_ctl_elem_remove(ctl,id)>=0;
    snd_ctl_close(ctl);
    std::cout<<(removed?"Removed only unused HA Volume; restart hardware service now\n":
        "Refused or removal failed\n");
    return removed?0:1;
}
