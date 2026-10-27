// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <algorithm>
#include <string>
#ifdef HW_WITH_ALSA
#include <alsa/asoundlib.h>
#endif
namespace hw {
inline int volume_percent(long raw) { return static_cast<int>((raw*100+127)/255); }
inline long volume_raw(int percent) { return (static_cast<long>(percent)*255+50)/100; }
class AlsaVolume {
public:
    AlsaVolume()=default;
    AlsaVolume(const AlsaVolume&)=delete;
    AlsaVolume& operator=(const AlsaVolume&)=delete;
    ~AlsaVolume() {
#ifdef HW_WITH_ALSA
        if (handle_) snd_ctl_close(handle_);
#endif
    }
    // Explicit cold-start provisioning, CTL only (never opens PCM). Existing
    // controls are validated, not overwritten. New controls start quiet at 35%.
    bool initialize(bool* created,std::string* error) {
        *created=false;
#ifdef HW_WITH_ALSA
        if (!handle_ && snd_ctl_open(&handle_,"hw:LotusMini",SND_CTL_NONBLOCK)<0)
            return fail("open_failed",error);
        snd_ctl_elem_info_t* info; snd_ctl_elem_info_alloca(&info);
        snd_ctl_elem_info_set_interface(info,SND_CTL_ELEM_IFACE_MIXER);
        snd_ctl_elem_info_set_name(info,"HA Volume");
        const int result=snd_ctl_elem_info(handle_,info);
        if (result>=0) return open(error);
        if (result!=-ENOENT) return fail("control_info_failed",error);
        if (snd_ctl_add_integer_elem_set(handle_,info,1,2,0,255,0)<0)
            return fail("control_create_failed",error);
        snd_ctl_elem_id_t* id; snd_ctl_elem_id_alloca(&id); snd_ctl_elem_info_get_id(info,id);
        const unsigned tlv[]={1,8,static_cast<unsigned>(-4000),15};
        if (snd_ctl_elem_tlv_write(handle_,id,tlv)<0) return fail("control_tlv_failed",error);
        int actual=0;
        if (!set(35,&actual,error)) return false;
        *created=true; return true;
#else
        return fail("alsa_not_built",error);
#endif
    }
    bool read(int* percent,std::string* error) {
#ifdef HW_WITH_ALSA
        if (!open(error)) return false;
        snd_ctl_elem_value_t* value;
        snd_ctl_elem_value_alloca(&value);
        identify(value);
        const int result=snd_ctl_elem_read(handle_,value);
        if (result<0) return fail("read_failed",error);
        long left=snd_ctl_elem_value_get_integer(value,0),right=snd_ctl_elem_value_get_integer(value,1);
        if (left<0 || left>255 || right<0 || right>255 || left!=right)
            return fail("asymmetric_or_invalid",error);
        *percent=volume_percent(left); return true;
#else
        (void)percent; return fail("alsa_not_built",error);
#endif
    }
    bool set(int percent,int* actual,std::string* error) {
        if (percent<0 || percent>100) return fail("invalid_percent",error);
#ifdef HW_WITH_ALSA
        if (!open(error)) return false;
        snd_ctl_elem_value_t* value;
        snd_ctl_elem_value_alloca(&value);
        identify(value);
        snd_ctl_elem_value_set_integer(value,0,volume_raw(percent));
        snd_ctl_elem_value_set_integer(value,1,volume_raw(percent));
        if (snd_ctl_elem_write(handle_,value)<0) return fail("write_failed",error);
        return read(actual,error);
#else
        (void)actual; return fail("alsa_not_built",error);
#endif
    }
private:
    bool fail(const char* reason,std::string* error) {
        *error=reason;
#ifdef HW_WITH_ALSA
        if (handle_) { snd_ctl_close(handle_); handle_=nullptr; }
#endif
        return false;
    }
#ifdef HW_WITH_ALSA
    static void identify(snd_ctl_elem_value_t* value) {
        snd_ctl_elem_value_set_interface(value,SND_CTL_ELEM_IFACE_MIXER);
        snd_ctl_elem_value_set_name(value,"HA Volume");
        snd_ctl_elem_value_set_index(value,0);
    }
    bool open(std::string* error) {
        if (!handle_ && snd_ctl_open(&handle_,"hw:LotusMini",SND_CTL_NONBLOCK)<0)
            return fail("open_failed",error);
        snd_ctl_elem_info_t* info;
        snd_ctl_elem_info_alloca(&info);
        snd_ctl_elem_info_set_interface(info,SND_CTL_ELEM_IFACE_MIXER);
        snd_ctl_elem_info_set_name(info,"HA Volume");
        if (snd_ctl_elem_info(handle_,info)<0 || snd_ctl_elem_info_get_type(info)!=SND_CTL_ELEM_TYPE_INTEGER ||
            snd_ctl_elem_info_get_count(info)!=2 || snd_ctl_elem_info_get_min(info)!=0 ||
            snd_ctl_elem_info_get_max(info)!=255 || !snd_ctl_elem_info_is_writable(info) ||
            !snd_ctl_elem_info_is_user(info) || !snd_ctl_elem_info_is_tlv_readable(info))
            return fail("unexpected_control_profile",error);
        return true;
    }
    snd_ctl_t* handle_=nullptr;
#endif
};
}
