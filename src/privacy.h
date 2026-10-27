// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "protocol.h"
#include <cstdint>

namespace hw {
// Service-side policy. Persistence runs outside the socket/event loop. Completion
// tickets prevent an old successful unmute write from undoing a newer mute.
class PrivacyState {
public:
    struct Save { uint64_t generation; bool muted; };
    void restore(bool valid, bool muted) {
        muted_=!valid || muted; requested_=muted_; pending_=false;
        error_=valid?"":"missing_or_invalid"; ++generation_;
    }
    Save request(bool muted) {
        requested_=muted; pending_=true; error_.clear(); ++generation_;
        // Mute immediately. Unmute only after the matching durable commit.
        muted_=true;
        return {generation_,muted};
    }
    bool complete(Save save, const std::string& error) {
        if (!pending_ || save.generation!=generation_ || save.muted!=requested_) return false;
        pending_=false; error_=error;
        muted_=!error.empty() || requested_;
        return true;
    }
    bool muted() const { return muted_; }
    Value status() const {
        return object({{"muted",Value(muted_)},{"requested_muted",Value(requested_)},
            {"pending",Value(pending_)},{"error",error_.empty()?Value():str(error_)}});
    }
private:
    bool muted_=true,requested_=true,pending_=false;
    uint64_t generation_=0;
    std::string error_;
};

// App-side safety policy, not an audio driver. The app must stop capture/KWS/
// uplink when allowed() becomes false and call lost() on transport/protocol loss.
// No implicit permission is granted by reconnect or by a global unmute event.
class PrivacyGate {
public:
    void lost() { known_=false; muted_=true; consent_=false; }
    void consent(bool allowed) { consent_=allowed; }
    bool snapshot(const Value& state) {
        lost();
        if (!field(state,"microphone_muted").is<bool>() ||
            field(state,"privacy_enforcement")!=str("cooperative")) return false;
        known_=true; muted_=field(state,"microphone_muted").get<bool>();
        return true;
    }
    bool changed(const Value& data) {
        if (!known_ || !field(data,"muted").is<bool>()) { lost(); return false; }
        muted_=field(data,"muted").get<bool>(); return true;
    }
    bool allowed() const { return known_ && !muted_ && consent_; }
private:
    bool known_=false,muted_=true,consent_=false;
};
}
