// SPDX-License-Identifier: GPL-3.0-only
#include "protocol.h"
#include "async_log.h"
#include "sensors.h"
#include "led_backend.h"
#include "input_backend.h"
#include "volume.h"
#include "volume_store.h"
#include "privacy_worker.h"
#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cerrno>
#include <fcntl.h>
#include <iostream>
#include <poll.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

using namespace hw;
using Clock = std::chrono::steady_clock;
static volatile sig_atomic_t stopping = 0;
static void stop_signal(int) { stopping = 1; }
static constexpr size_t MAX_FRAME = 16384, MAX_OUTPUT = 65536;
#ifdef HW_WITH_ALSA
// ALSA's default diagnostic is unstructured stderr; emit our bounded JSON errors.
static void quiet_alsa_error(const char*,int,const char*,int,const char*,...) {}
#endif

struct Client {
    int fd;
    unsigned id;
    std::string input, output;
    bool subscribed = false, closed = false;
    unsigned log_suppressed = 0;
    Clock::time_point next_warning{};
};

class Server {
public:
    Server(std::string path, bool debug, bool readonly, std::string root,
           bool controls=false, std::string input_root="/dev/input",bool alsa_volume=false,
           std::string state_directory={},bool privacy=false,bool global_keys=false,bool initialize_volume=false)
        : path_(std::move(path)), debug_(debug), readonly_(readonly),
          sensors_((readonly || controls) ? std::unique_ptr<SensorBackend>(new SysfsSensors(root)) :
                             std::unique_ptr<SensorBackend>(new MockSensors)),
          controls_(controls), sysfs_root_(std::move(root)), input_root_(std::move(input_root)),
          alsa_volume_(alsa_volume),state_directory_(std::move(state_directory)),
          privacy_enabled_(privacy),global_keys_(global_keys),initialize_volume_(initialize_volume) {}
    ~Server() {
        for (auto& c : clients_) close(c.fd);
        if (listener_ >= 0) close(listener_);
        struct stat s{};
        if (bound_ && lstat(path_.c_str(), &s) == 0 && s.st_ino == inode_ &&
            s.st_dev == device_) unlink(path_.c_str());
    }
    int run() {
        if (!logger_.available()) { std::cerr << "logger_initialization_failed\n"; return 1; }
        const auto slash = path_.find_last_of('/');
        if (slash == std::string::npos || slash == 0) return fail("private_parent_required");
        struct stat parent{}, existing{};
        const std::string directory = path_.substr(0, slash);
        if (lstat(directory.c_str(), &parent) || !S_ISDIR(parent.st_mode) ||
            parent.st_uid != geteuid() || (parent.st_mode & 0022))
            return fail("unsafe_socket_directory");
        // Never unlink a pre-existing socket: it may belong to a live service.
        if (lstat(path_.c_str(), &existing) == 0 || errno != ENOENT)
            return fail("socket_path_exists");
        sockaddr_un addr{};
        if (!address(path_, &addr)) return fail("invalid_socket_path");
        listener_ = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        if (listener_ < 0) return fail("socket_failed");
        const mode_t old = umask(0077);
        const int result = bind(listener_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
        umask(old);
        if (result) return fail("bind_failed");
        if (lstat(path_.c_str(), &existing)) return fail("socket_stat_failed");
        bound_ = true; inode_ = existing.st_ino; device_ = existing.st_dev;
        if (chmod(path_.c_str(), 0600) || listen(listener_, 16)) return fail("listen_failed");
        if (!state_directory_.empty()) {
            store_.reset(new VolumeStore(state_directory_));
            if (!store_->valid()) return fail("unsafe_or_locked_state_directory");
        }
        if (privacy_enabled_) {
            privacy_worker_.reset(new PrivacyWorker(state_directory_));
            if (!privacy_worker_->valid()) return fail("unsafe_or_locked_privacy_directory");
            bool muted=true; std::string reason;
            const bool valid=privacy_worker_->load(&muted,&reason);
            privacy_.restore(valid,muted); muted_=privacy_.muted();
            log(valid?"INFO":"WARN","privacy_state_load",0,reason);
            privacy_worker_->start();
        }
        if (controls_) {
            rgb_.reset(new SysfsRgb(sysfs_root_));
            runner_.reset(new LedRunner([this](unsigned mask){return rgb_->write(mask);}));
            led_available_=runner_->set({});
            buttons_.immediate_globals(global_keys_);
            buttons_available_=buttons_.discover(input_root_);
            log(led_available_?"INFO":"WARN","led_backend",0,led_available_?"available":"device_error");
            log(buttons_available_?"INFO":"WARN","button_backend",0,buttons_available_?"available":"missing_or_ambiguous");
        }
        sensor_state_=sensors_->read();
        if (initialize_volume_) {
            bool created=false; std::string error;
            if (!volume_backend_.initialize(&created,&error)) {
                log("ERROR","volume_initialize_failed",0,error); return 1;
            }
            log("INFO","volume_initialized",0,created?"created_quiet_control":"existing_control_preserved");
        }
        refresh_volume(); // Read current hardware only: no startup write/default restore.
        if (store_) {
            int saved=0; std::string reason;
            if (store_->load(&saved,&reason)) {
                if (alsa_volume_ && !volume_backend_.set(saved,&saved,&reason)) {
                    volume_available_=false;
                    store_->load_failure("restore_failed");
                    log("WARN","volume_restore_failed",0,reason);
                } else {
                    volume_=saved;
                    if (alsa_volume_) volume_available_=true;
                    log("INFO","volume_restored",0,"saved_state");
                }
            } else {
                store_->load_failure(reason);
                log(reason=="missing"?"INFO":"WARN","volume_state_load",0,reason);
            }
            store_->start();
            refresh_persistence();
            refresh_privacy();
        }
        if (readonly_ && !field(sensor_state_.battery,"available").get<bool>())
            log("WARN","battery_unavailable",0,"missing_or_invalid");
        if (readonly_ && !field(sensor_state_.temperature,"available").get<bool>())
            log("WARN","soc_temperature_unavailable",0,"missing_or_invalid");
        log("INFO", "ready", 0, controls_?"sysfs_controls":readonly_ ? "sysfs_read_only" : "mock_backend");
        while (!stopping) {
            expire();
            if (led_deadline_!=Clock::time_point{} && Clock::now()>=led_deadline_) {
                led_deadline_={}; led_=object({{"effect",str("off")}});
                if (controls_) runner_->set({});
                emit("led.changed",led_);
            }
            refresh_sensors();
            poll_controls();
            refresh_volume();
            refresh_persistence();
            refresh_privacy();
            std::vector<pollfd> fds{{listener_, POLLIN, 0}};
            const size_t count = clients_.size();
            for (auto& c : clients_) fds.push_back({c.fd,
                static_cast<short>(POLLIN | (c.output.empty() ? 0 : POLLOUT)), 0});
            int n = poll(fds.data(), fds.size(), 50);
            if (n < 0 && errno != EINTR) return fail("poll_failed");
            for (size_t i = 0; i < count; ++i) {
                auto& c = clients_[i];
                if (fds[i+1].revents & POLLIN) read_client(c);
                if (!c.closed && (fds[i+1].revents & POLLOUT)) write_client(c);
                if (fds[i+1].revents & (POLLERR | POLLHUP | POLLNVAL)) c.closed = true;
            }
            for (auto it = clients_.begin(); it != clients_.end();) {
                if (!it->closed) { ++it; continue; }
                if (owner_ == it->id) release("disconnect");
                log("DEBUG", "disconnected", it->id, "closed");
                close(it->fd); it = clients_.erase(it);
            }
            if (n > 0 && (fds[0].revents & POLLIN)) accept_client();
        }
        release("shutdown");
        if (store_) { store_->finish(); refresh_persistence(); }
        if (privacy_worker_) { privacy_worker_->finish(); refresh_privacy(); }
        log("INFO", "stopped", 0, "normal");
        return 0;
    }
private:
    void log(const char* level, const std::string& event, unsigned client,
             const std::string& result, const Value& request = Value(), long elapsed = -1) {
        if (std::string(level)=="DEBUG" && !debug_) return;
        const auto now = Clock::now();
        if (now - log_window_ >= std::chrono::seconds(1)) {
            log_window_ = now; log_count_ = 0;
            if (suppressed_logs_) {
                const unsigned suppressed = suppressed_logs_;
                suppressed_logs_ = 0;
                log("WARN", "suppressed_logs", 0, std::to_string(suppressed));
            }
        }
        if (std::string(level) != "ERROR" && ++log_count_ > 100) {
            if (suppressed_logs_ < 1000000000) ++suppressed_logs_;
            return;
        }
        Object fields{{"tag", str("lotus-hardwared")}, {"level", str(level)},
            {"component", str("service")}, {"event", str(event)},
            {"result", str(result)}, {"connection", num(client)},
            {"instance", str(instance_)}, {"uptime_ms", num(std::chrono::duration_cast<
                std::chrono::milliseconds>(Clock::now()-started_).count())}};
        if (!request.is<picojson::null>()) fields["request"] = request;
        if (elapsed >= 0) fields["elapsed_us"] = num(elapsed);
        // JSON escapes control characters; never log arbitrary params or frame bodies.
        logger_.push(object(std::move(fields)).serialize());
    }
    void warn(Client& c, const char* reason) {
        const auto now = Clock::now();
        if (now < c.next_warning) { ++c.log_suppressed; return; }
        log("WARN", "request_rejected", c.id, reason);
        if (c.log_suppressed) log("WARN", "suppressed_errors", c.id,
                                std::to_string(c.log_suppressed));
        c.log_suppressed = 0; c.next_warning = now + std::chrono::seconds(1);
    }
    int fail(const char* reason) { log("ERROR", "failed", 0, reason); return 1; }
    void queue(Client& c, const Value& message) {
        const std::string frame = message.serialize() + "\n";
        if (frame.size() > MAX_OUTPUT - std::min(MAX_OUTPUT, c.output.size())) {
            if (!c.closed) log("WARN", "slow_client", c.id, "output_limit");
            c.closed = true; return;
        }
        c.output += frame;
    }
    void emit(const char* event, Value data) {
        Value message = object({{"event", str(event)}, {"sequence", num(++sequence_)},
                                {"data", std::move(data)}});
        for (auto& c : clients_) if (c.subscribed && !c.closed) queue(c, message);
    }
    Value state() const {
        return object({{"backend", str(controls_?"sysfs_controls":readonly_?"sysfs_read_only":"mock")},
            {"volume_percent", alsa_volume_?(volume_available_?num(volume_):Value()):(readonly_||controls_)?Value():num(volume_)},
            {"microphone_muted", (readonly_||(controls_&&!privacy_enabled_))?Value():Value(muted_)},
            {"privacy_enforcement", str((readonly_||(controls_&&!privacy_enabled_))?"unavailable":"cooperative")},
            {"privacy_persistence",privacy_enabled_?privacy_.status():object({{"enabled",Value(false)}})},
            {"audio",object({{"owner_connection",num(audio_owner_)},{"capture",Value(audio_capture_)},
                {"playback",Value(audio_playback_)},{"enforcement",str("cooperative")}})},
            {"persistent", Value(privacy_enabled_)}, {"owner_connection", num(owner_)},
            {"volume_persistence",store_?store_->status():object({{"enabled",Value(false)}})},
            {"led", (readonly_||(controls_&&!led_available_))?Value():led_}, {"battery", sensor_state_.battery},
            {"temperature", sensor_state_.temperature}});
    }
    void refresh_sensors() {
        if (!(readonly_||controls_) || Clock::now()<next_sensor_poll_) return;
        next_sensor_poll_=Clock::now()+std::chrono::seconds(1);
        SensorSnapshot current=sensors_->read();
        if (current.battery!=sensor_state_.battery) {
            emit("battery.changed",current.battery);
            log("INFO","battery_state_changed",0,
                field(current.battery,"available").get<bool>()?"available":"unavailable");
        }
        if (current.temperature!=sensor_state_.temperature) {
            emit("temperature.changed",current.temperature);
            if (field(current.temperature,"available")!=field(sensor_state_.temperature,"available"))
                log("WARN","temperature_availability_changed",0,
                    field(current.temperature,"available").get<bool>()?"recovered":"unavailable");
        }
        sensor_state_=std::move(current);
    }
    void release(const char* reason) {
        if (!owner_) return;
        release_audio(reason);
        log("INFO", "lease_released", owner_, reason);
        owner_ = 0; led_ = object({{"effect", str("off")}});
        led_deadline_={};
        if (controls_) {
            buttons_.cancel_gestures();
            if (!runner_->set({})) log("WARN","led_close_failed",0,"device_error");
        }
        emit("lease.changed", object({{"owner_connection", num(0)}, {"reason", str(reason)}}));
        emit("led.changed", led_);
    }
    void expire() { if (owner_ && Clock::now() >= expiry_) release("expired"); }
    void release_audio(const char* reason) {
        if (!audio_owner_) return;
        audio_owner_=0; audio_capture_=audio_playback_=false;
        emit("audio.changed",object({{"owner_connection",num(0)},{"capture",Value(false)},
            {"playback",Value(false)},{"reason",str(reason)}}));
        log("INFO","audio_released",0,reason);
    }
    void privacy_changed(unsigned source) {
        if (muted_ && audio_capture_) release_audio("privacy_muted");
        // Privacy gates capture, not LED ownership or application appearance.
        emit("privacy.changed",object({{"muted",Value(muted_)},{"source_connection",num(source)}}));
        log("INFO","privacy_changed",source,muted_?"muted":"unmuted");
    }
    void set_privacy(bool requested,unsigned source) {
        // A newer API/key request cancels any outstanding physical-key cue.
        privacy_feedback_owner_=0;
        if (!privacy_enabled_) {
            if (muted_!=requested) { muted_=requested; privacy_changed(source); }
            return;
        }
        privacy_source_=source;
        const auto ticket=privacy_.request(requested);
        const bool changed=!muted_; muted_=true;
        if (changed) privacy_changed(source);
        privacy_worker_->schedule(ticket);
        emit("privacy.persistence",privacy_.status());
        log("INFO","privacy_save_pending",source,requested?"mute":"unmute");
    }
    void refresh_privacy() {
        if (!privacy_worker_) return;
        PrivacyWorker::Completion completion{};
        if (!privacy_worker_->take(&completion)) return;
        if (completion.failures!=privacy_failures_) {
            privacy_failures_=completion.failures;
            log("WARN","privacy_save_failed",privacy_source_,
                completion.error.empty()?"earlier_write_failed":completion.error);
        }
        if (!privacy_.complete(completion.save,completion.error)) return;
        const bool changed=muted_!=privacy_.muted(); muted_=privacy_.muted();
        emit("privacy.persistence",privacy_.status());
        log(completion.error.empty()?"INFO":"WARN","privacy_save_completed",privacy_source_,
            completion.error.empty()?"saved":completion.error);
        if (changed) privacy_changed(privacy_source_);
        if (privacy_feedback_owner_ && privacy_feedback_owner_==owner_)
            button_feedback(248);
        privacy_feedback_owner_=0;
    }
    void button_feedback(unsigned code) {
        if (!owner_) return;
        for (auto& client:clients_) if (client.id==owner_ && !client.closed)
            queue(client,object({{"event",str("button.feedback")},{"sequence",num(++sequence_)},
                {"data",object({{"code",num(code)},{"muted",Value(muted_)},{"percent",num(volume_)}})}}));
    }
    void refresh_persistence() {
        if (!store_) return;
        const Value status=store_->status();
        if (status==persistence_status_) return;
        const bool new_failure=field(status,"failure_count")!=field(persistence_status_,"failure_count") &&
            integer(field(status,"failure_count"),1,1000000000);
        if (new_failure) log("WARN","volume_save_failed",0,field(status,"last_failure").get<std::string>());
        persistence_status_=status;
        emit("volume.persistence",status);
        const std::string phase=field(status,"status").get<std::string>();
        log(phase=="error"?"WARN":"INFO","volume_persistence",0,
            phase=="error"?field(status,"error").get<std::string>():phase);
    }
    void refresh_volume() {
        if (!alsa_volume_ || Clock::now()<next_volume_poll_) return;
        next_volume_poll_=Clock::now()+std::chrono::seconds(1);
        int percent=0; std::string error;
        const bool ok=volume_backend_.read(&percent,&error);
        if (ok!=volume_available_ || (!ok && error!=volume_error_)) {
            log(ok?"INFO":"WARN","volume_availability",0,ok?"available":error);
            emit("volume.availability",object({{"available",Value(ok)}}));
        }
        if (ok && volume_available_ && volume_!=percent)
            emit("volume.changed",object({{"percent",num(percent)},{"source_connection",num(0)}}));
        volume_available_=ok; volume_error_=error;
        if (ok) volume_=percent;
    }
    void poll_controls() {
        if (!controls_) return;
        const bool healthy=runner_->healthy();
        if (healthy!=led_available_) {
            led_available_=healthy;
            log(healthy?"INFO":"WARN","led_availability_changed",0,healthy?"recovered":"device_error");
            if (!healthy) led_=Value();
            emit("led.availability",object({{"available",Value(healthy)}}));
        }
        auto batch=buttons_.poll(static_cast<uint64_t>(std::chrono::duration_cast<
            std::chrono::milliseconds>(Clock::now()-started_).count()));
        if (batch.input_lost) log("WARN","input_lost",0,"gestures_cancelled");
        const bool available=buttons_.available();
        if (available!=buttons_available_) {
            buttons_available_=available;
            log("WARN","button_availability_changed",0,available?"available":"unavailable");
        }
        for (const auto& event:batch.events) {
            const bool action=event.code==164;
            Value data=object({{"code",num(event.code)}, {"gesture",str(event.gesture)},
                {"time_ms",num(event.time_ms)}, {"owner_connection",num(action?owner_:0)},
                {"action_authorized",Value(action && owner_!=0)}});
            emit("button.observed",data);
            bool global_done=false;
            if (global_keys_ && (event.code==114 || event.code==115)) {
                int current=0; std::string reason;
                const int delta=(event.code==115?5:-5)*(event.gesture=="double"?2:1);
                if (volume_backend_.read(&current,&reason) &&
                    volume_backend_.set(std::max(0,std::min(100,current+delta)),&current,&reason)) {
                    volume_=current; volume_available_=true; global_done=true;
                    if (store_) store_->schedule(volume_);
                    emit("volume.changed",object({{"percent",num(volume_)},{"source_connection",num(0)}}));
                } else log("WARN","global_volume_failed",0,reason);
            } else if (global_keys_ && event.code==248 && event.gesture=="press") {
                set_privacy(!field(privacy_.status(),"requested_muted").get<bool>(),0);
                privacy_feedback_owner_=owner_; global_done=true;
            }
            if (global_done && event.code!=248) button_feedback(event.code);
            // Only the business-action owner receives an executable event, even
            // when not subscribed. Volume/privacy remain observation-only in P2.
            if (action && owner_) for (auto& client:clients_)
                if (client.id==owner_ && !client.closed)
                    queue(client,object({{"event",str("button.action")},
                        {"sequence",num(++sequence_)},{"data",data}}));
            log("INFO","button_gesture",action?owner_:0,
                action?"action_routed":global_done?"global_action_executed":"observation_only");
        }
    }
    void accept_client() {
        int fd = accept4(listener_, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (fd < 0) return;
        ucred cred{}; socklen_t size = sizeof(cred);
        if (clients_.size() >= 16 || getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &size) ||
            cred.uid != geteuid()) { close(fd); log("WARN", "connection_rejected", 0, "peer_or_limit"); return; }
        clients_.push_back(Client{fd, ++next_id_, {}, {}});
        auto& c = clients_.back();
        log("DEBUG", "connected", c.id, "uid=" + std::to_string(cred.uid));
        queue(c, object({{"version", num(1)}, {"event", str("snapshot")},
            {"sequence", num(sequence_)}, {"data", state()}}));
    }
    void read_client(Client& c) {
        char buffer[4096];
        // Bounded work per client per poll turn, so one sender cannot starve leases.
        ssize_t n = recv(c.fd, buffer, sizeof(buffer), 0);
        if (n == 0) { c.closed = true; return; }
        if (n < 0) { if (errno != EAGAIN && errno != EINTR) c.closed = true; return; }
        c.input.append(buffer, static_cast<size_t>(n));
        size_t pos;
        while (!c.closed && (pos = c.input.find('\n')) != std::string::npos) {
            if (pos > MAX_FRAME) { warn(c, "frame_limit"); c.closed = true; break; }
            std::string line = c.input.substr(0, pos); c.input.erase(0, pos+1);
            dispatch(c, line);
        }
        if (c.input.size() > MAX_FRAME) { warn(c, "frame_limit"); c.closed = true; }
    }
    void write_client(Client& c) {
        ssize_t n = send(c.fd, c.output.data(), c.output.size(), MSG_NOSIGNAL);
        if (n > 0) c.output.erase(0, static_cast<size_t>(n));
        else if (n < 0 && errno != EAGAIN && errno != EINTR) c.closed = true;
    }
    void dispatch(Client& c, const std::string& line) {
        const auto start = Clock::now();
        Value req;
        const bool parsed = hw::parse(req, line);
        Value id = field(req, "id");
        if (!integer(id, 0, 9007199254740991.0)) id = Value();
        auto error = [&](const char* code) {
            queue(c, object({{"id", id}, {"ok", Value(false)},
                {"error", object({{"code", str(code)}})}}));
            warn(c, code);
        };
        if (!parsed || !req.is<Object>() || id.is<picojson::null>() ||
            !field(req,"method").is<std::string>() || !field(req,"params").is<Object>()) {
            error("invalid_request"); return;
        }
        if (!integer(field(req,"version"), 1, 1)) { error("unsupported_version"); return; }
        const auto& method = field(req,"method").get<std::string>();
        const Value& p = field(req,"params");
        Value result = object({});
        if (readonly_ && method!="capabilities.get" && method!="state.get" &&
            method!="events.subscribe" && method!="events.unsubscribe") {
            error("not_supported"); return;
        }
        if (controls_ && ((method=="volume.set" || method=="volume.adjust") && !alsa_volume_)) {
            error("not_supported"); return;
        }
        if (controls_ && method=="privacy.set" && !privacy_enabled_) {
            error("not_supported"); return;
        }
        if (method == "capabilities.get") {
            result = object({{"version", num(1)}, {"backend", str("mock")},
                {"led", object({{"rgb", Value(true)}, {"effects", Value(picojson::array{
                    str("off"), str("solid"), str("breath"), str("gradient")})}})},
                {"buttons", object({{"available", Value(false)}})},
                {"volume", object({{"available", Value(true)}, {"unit", str("percent")}})},
                {"privacy", object({{"persistent", Value(false)}, {"enforcement", str("cooperative")}})},
                {"battery", object({{"available", Value(false)}})},
                {"temperature", object({{"available", Value(false)}})}});
            if (readonly_ || controls_) {
                result=object({{"version",num(1)}, {"backend",str("sysfs_read_only")},
                    {"led",object({{"rgb",Value(controls_&&led_available_)}})},
                    {"buttons",object({{"available",Value(controls_&&buttons_available_)},
                        {"global_actions",Value(global_keys_)}})},
                    {"volume",object({{"available",Value(false)}})},
                    {"privacy",object({{"available",Value(false)}})},
                    {"battery",object({{"supported",Value(true)},
                        {"available",field(sensor_state_.battery,"available")}})},
                    {"temperature",object({{"supported",Value(true)},
                        {"available",field(sensor_state_.temperature,"available")}})}});
                if (controls_) {
                    result.get<Object>()["backend"]=str("sysfs_controls");
                    picojson::array codes;
                    for (unsigned code:buttons_.codes()) codes.push_back(num(code));
                    result.get<Object>()["buttons"].get<Object>()["codes"]=Value(codes);
                    result.get<Object>()["led"].get<Object>()["effects"]=Value(picojson::array{
                        str("off"),str("solid"),str("breath"),str("gradient")});
                    result.get<Object>()["volume"]=object({{"available",Value(alsa_volume_&&volume_available_)},
                        {"supported",Value(alsa_volume_)},{"unit",str("percent")}});
                }
            }
            if (!readonly_) {
                result.get<Object>()["audio"]=object({{"available",Value(true)},{"enforcement",str("cooperative")}});
                if (privacy_enabled_) result.get<Object>()["privacy"]=object({{"available",Value(true)},
                    {"persistent",Value(true)},{"enforcement",str("cooperative")}});
            }
        } else if (method == "state.get") result = state();
        else if (method == "events.subscribe") {
            c.subscribed = true;
            result = object({{"sequence", num(sequence_)}, {"snapshot", state()}});
        } else if (method == "events.unsubscribe") c.subscribed = false;
        else if (method == "lease.acquire" || method == "lease.renew") {
            if (owner_ && owner_ != c.id) { error("busy"); return; }
            if (method == "lease.renew" && owner_ != c.id) { error("permission_denied"); return; }
            const Value& ttl = field(p,"ttl_ms");
            if (!integer(ttl, 1000, 60000)) { error("invalid_request"); return; }
            if (controls_ && !owner_) buttons_.cancel_gestures();
            owner_ = c.id; expiry_ = Clock::now() + std::chrono::milliseconds(static_cast<int>(ttl.get<double>()));
            result = object({{"owner_connection", num(owner_)}, {"ttl_ms", ttl},
                {"resources", Value(picojson::array{str("led"), str("action_buttons")})}});
            if (method=="lease.acquire") emit("lease.changed", result);
            if (method=="lease.acquire" || debug_) log(method=="lease.acquire"?"INFO":"DEBUG", method, c.id, "granted", id);
        } else if (method == "lease.release") {
            if (owner_ != c.id) { error("permission_denied"); return; }
            release("requested");
        } else if (method == "audio.acquire") {
            if (owner_!=c.id) { error("permission_denied"); return; }
            if (!field(p,"capture").is<bool>() || !field(p,"playback").is<bool>()) { error("invalid_request"); return; }
            const bool capture=field(p,"capture").get<bool>(),playback=field(p,"playback").get<bool>();
            if (!capture && !playback) { error("invalid_request"); return; }
            if (capture && muted_) { error("permission_denied"); return; }
            if (audio_owner_ && audio_owner_!=c.id) { error("busy"); return; }
            audio_owner_=c.id; audio_capture_=capture; audio_playback_=playback;
            result=field(state(),"audio"); emit("audio.changed",result);
            log("INFO","audio_acquired",c.id,capture?"capture_requested":"playback_requested");
        } else if (method == "audio.release") {
            if (audio_owner_!=c.id) { error("permission_denied"); return; }
            release_audio("requested");
        } else if (method == "led.set") {
            if (owner_ != c.id) { error("permission_denied"); return; }
            if (!field(p,"effect").is<std::string>()) { error("invalid_request"); return; }
            const auto effect = field(p,"effect").get<std::string>();
            if (effect != "off" && effect != "solid" && effect != "breath" && effect != "gradient") {
                error("not_supported"); return;
            }
            Object validated{{"effect", str(effect)}};
            if (!field(p,"duration_ms").is<picojson::null>()) {
                if (!integer(field(p,"duration_ms"),100,60000)) { error("invalid_request"); return; }
                validated["duration_ms"]=field(p,"duration_ms");
            }
            if (effect != "off") {
                const Value& color = field(p,"color");
                if (!integer(color, 0, 0xffffff)) { error("invalid_request"); return; }
                validated["color"] = color;
                if (!integer(field(p,"brightness_percent"), 0, 100)) { error("invalid_request"); return; }
                validated["brightness_percent"] = field(p,"brightness_percent");
            }
            if (effect == "breath" || effect == "gradient") {
                if (!integer(field(p,"period_ms"), 100, 60000)) { error("invalid_request"); return; }
                validated["period_ms"] = field(p,"period_ms");
            }
            if (effect == "gradient") {
                if (!integer(field(p,"color_2"), 0, 0xffffff)) { error("invalid_request"); return; }
                validated["color_2"] = field(p,"color_2");
            }
            if (controls_) {
                LedEffect requested;
                requested.kind=effect=="solid"?LedEffect::Solid:effect=="breath"?LedEffect::Breath:
                    effect=="gradient"?LedEffect::Gradient:LedEffect::Off;
                if (effect!="off") {
                    requested.color=static_cast<uint32_t>(field(p,"color").get<double>());
                    requested.brightness=static_cast<unsigned>(field(p,"brightness_percent").get<double>());
                }
                if (effect=="breath"||effect=="gradient") requested.period_ms=static_cast<unsigned>(field(p,"period_ms").get<double>());
                if (effect=="gradient") requested.color2=static_cast<uint32_t>(field(p,"color_2").get<double>());
                if (!runner_->set(requested)) {
                    log("WARN","led_set_failed",c.id,"device_error",id);
                    error("device_error"); return;
                }
            }
            led_ = object(std::move(validated)); result = led_;
            led_deadline_=integer(field(led_,"duration_ms"),100,60000)?
                Clock::now()+std::chrono::milliseconds(static_cast<int>(field(led_,"duration_ms").get<double>())):Clock::time_point{};
            emit("led.changed", led_); log("INFO", "led_changed", c.id, effect, id);
        } else if (method == "volume.set" || method=="volume.adjust") {
            const bool adjust=method=="volume.adjust";
            const Value& input=field(p,adjust?"delta":"percent");
            if (!integer(input,adjust?-100:0,100) || (adjust&&input.get<double>()==0)) { error("invalid_request"); return; }
            int current=volume_;
            std::string backend_error;
            if (alsa_volume_ && !volume_backend_.read(&current,&backend_error)) {
                log("WARN","volume_read_failed",c.id,backend_error,id); error("device_error"); return;
            }
            int requested=static_cast<int>(input.get<double>());
            if (adjust) requested=std::max(0,std::min(100,current+requested));
            if (alsa_volume_ && !volume_backend_.set(requested,&requested,&backend_error)) {
                log("WARN","volume_write_failed",c.id,backend_error,id); error("device_error"); return;
            }
            if (alsa_volume_) volume_available_=true;
            if (volume_ != requested) {
                volume_ = requested;
                emit("volume.changed", object({{"percent", num(volume_)}, {"source_connection", num(c.id)}}));
                log("INFO", "volume_changed", c.id, std::to_string(volume_), id);
            }
            result = object({{"percent", num(volume_)}});
            if (store_) {
                store_->schedule(volume_);
                result.get<Object>()["persistence"]=store_->status();
            }
        } else if (method == "privacy.set") {
            if (!field(p,"muted").is<bool>()) { error("invalid_request"); return; }
            set_privacy(field(p,"muted").get<bool>(),c.id);
            result = object({{"muted", Value(muted_)}, {"persistent", Value(privacy_enabled_)}});
            if (privacy_enabled_) result.get<Object>()["persistence"]=privacy_.status();
        } else { error("not_supported"); return; }
        queue(c, object({{"id", id}, {"ok", Value(true)}, {"result", result}}));
        if (debug_) log("DEBUG", "request_completed", c.id, "success", id,
            std::chrono::duration_cast<std::chrono::microseconds>(Clock::now()-start).count());
    }
    std::string path_;
    bool debug_ = false, bound_ = false, muted_ = true;
    int listener_ = -1, volume_ = 35;
    ino_t inode_ = 0; dev_t device_ = 0;
    unsigned next_id_ = 0, owner_ = 0;
    uint64_t sequence_ = 0;
    Clock::time_point started_ = Clock::now(), expiry_{};
    Clock::time_point led_deadline_{};
    Clock::time_point log_window_ = Clock::now();
    unsigned log_count_ = 0, suppressed_logs_ = 0;
    std::string instance_ = std::to_string(getpid()) + "-" + std::to_string(
        started_.time_since_epoch().count());
    Value led_ = object({{"effect", str("off")}});
    std::vector<Client> clients_;
    AsyncLog logger_{instance_};
    bool readonly_ = false;
    std::unique_ptr<SensorBackend> sensors_;
    SensorSnapshot sensor_state_;
    Clock::time_point next_sensor_poll_{};
    bool controls_=false,led_available_=false,buttons_available_=false;
    std::string sysfs_root_,input_root_;
    EvdevButtons buttons_;
    std::unique_ptr<SysfsRgb> rgb_;
    std::unique_ptr<LedRunner> runner_; // destroyed before rgb_ and logger_
    bool alsa_volume_=false,volume_available_=false;
    AlsaVolume volume_backend_;
    std::string volume_error_;
    Clock::time_point next_volume_poll_{};
    std::string state_directory_;
    std::unique_ptr<VolumeStore> store_;
    Value persistence_status_;
    bool privacy_enabled_=false,global_keys_=false,audio_capture_=false,audio_playback_=false;
    unsigned audio_owner_=0,privacy_source_=0,privacy_failures_=0,privacy_feedback_owner_=0;
    PrivacyState privacy_;
    std::unique_ptr<PrivacyWorker> privacy_worker_;
    bool initialize_volume_=false;
};

int main(int argc, char** argv) {
    std::string path,root="/sys",input="/dev/input",state_directory;
    bool debug = false, mock = false, readonly=false,controls=false,handoff=false,alsa_volume=false,state_requested=false,
        privacy=false,global_keys=false,initialize_volume=false;
    for (int i=1; i<argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--socket" && i+1<argc) path = argv[++i];
        else if (arg == "--mock") mock = true;
        else if (arg == "--read-only") readonly=true;
        else if (arg == "--controls") controls=true;
        else if (arg == "--handoff-confirmed") handoff=true;
        else if (arg == "--volume-alsa") alsa_volume=true;
        else if (arg == "--privacy") privacy=true;
        else if (arg == "--global-keys") global_keys=true;
        else if (arg == "--initialize-volume") initialize_volume=true;
        else if (arg == "--state-dir" && i+1<argc) { state_requested=true; state_directory=argv[++i]; }
        else if (arg == "--input-root" && i+1<argc) input=argv[++i];
        else if (arg == "--sysfs-root" && i+1<argc) root=argv[++i];
        else if (arg == "--debug") debug = true;
        else { std::cerr << "usage: lotus-hardwared (--mock|--read-only|--controls --handoff-confirmed) --socket PATH [--sysfs-root PATH] [--input-root PATH] [--volume-alsa] [--state-dir PATH] [--debug]\n"; return 2; }
    }
#ifndef HW_WITH_ALSA
    if (alsa_volume) { std::cerr<<"--volume-alsa requires an ALSA-enabled build\n"; return 2; }
#else
    if (alsa_volume) snd_lib_error_set_handler(quiet_alsa_error);
#endif
    if (static_cast<int>(mock)+readonly+controls!=1 || path.empty() || root.empty() || root[0]!='/' ||
        input.empty() || input[0]!='/' || (controls && !handoff) || (handoff && !controls) || (alsa_volume&&!controls) ||
        (state_requested && (state_directory.empty() || state_directory[0]!='/' || readonly || (controls&&!alsa_volume))) ||
        (privacy && (readonly || !state_requested)) || (global_keys && (!controls || !privacy || !alsa_volume)) ||
        (initialize_volume && !alsa_volume)) {
        std::cerr << "choose exactly one backend, absolute roots, and confirm handoff for controls\n"; return 2;
    }
    signal(SIGINT, stop_signal); signal(SIGTERM, stop_signal); signal(SIGPIPE, SIG_IGN);
    return Server(path, debug, readonly, root,controls,input,alsa_volume,state_directory,privacy,global_keys,initialize_volume).run();
}
