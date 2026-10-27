// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <picojson.h>
#include <cmath>
#include <string>
#include <sys/socket.h>
#include <sys/un.h>
#include <cstring>

namespace hw {
using Value = picojson::value;
using Object = picojson::object;
inline Value object(Object v) { return Value(std::move(v)); }
inline Value str(const std::string& s) { return Value(s); }
inline Value num(double n) { return Value(n); }
class StrictContext : public picojson::default_parse_context {
public:
    explicit StrictContext(Value* v) : picojson::default_parse_context(v) {}
    template<typename Iter> bool parse_array_item(picojson::input<Iter>& in, size_t) {
        auto& a = out_->get<picojson::array>();
        a.push_back(Value()); StrictContext child(&a.back());
        return picojson::_parse(child, in);
    }
    template<typename Iter> bool parse_object_item(picojson::input<Iter>& in, const std::string& key) {
        auto& o = out_->get<Object>();
        if (o.count(key)) return false;
        StrictContext child(&o[key]); return picojson::_parse(child, in);
    }
};
inline bool parse(Value& v, const std::string& text) {
    // Bound recursion before calling the library and reject invalid UTF-8.
    int depth = 0;
    bool quoted = false, escape = false;
    for (size_t i = 0; i < text.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        if (c >= 0x80) {
            unsigned count = c >= 0xc2 && c <= 0xdf ? 1 :
                c >= 0xe0 && c <= 0xef ? 2 : c >= 0xf0 && c <= 0xf4 ? 3 : 0;
            if (!count || i + count >= text.size()) return false;
            unsigned code = c & ((1u << (6-count)) - 1);
            for (unsigned j = 0; j < count; ++j) {
                const unsigned char next = static_cast<unsigned char>(text[++i]);
                if ((next & 0xc0) != 0x80) return false;
                code = (code << 6) | (next & 0x3f);
            }
            if (code < (count == 1 ? 0x80u : count == 2 ? 0x800u : 0x10000u) ||
                code > 0x10ffff || (code >= 0xd800 && code <= 0xdfff)) return false;
            continue;
        }
        if (quoted) {
            if (escape) escape = false;
            else if (c == '\\') escape = true;
            else if (c == '"') quoted = false;
        } else if (c == '"') quoted = true;
        else if (c == '[' || c == '{') { if (++depth > 16) return false; }
        else if (c == ']' || c == '}') { if (--depth < 0) return false; }
    }
    if (depth || quoted) return false;
    try {
        std::string error;
        StrictContext context(&v);
        auto end = picojson::_parse(context, text.begin(), text.end(), &error);
        while (end != text.end() && (*end == ' ' || *end == '\t' || *end == '\r')) ++end;
        return error.empty() && end == text.end();
    } catch (const std::exception&) { return false; }
}
inline const Value& field(const Value& v, const std::string& key) {
    static const Value empty;
    if (!v.is<Object>()) return empty;
    auto it = v.get<Object>().find(key);
    return it == v.get<Object>().end() ? empty : it->second;
}
inline bool integer(const Value& v, double lo, double hi) {
    return v.is<double>() && std::isfinite(v.get<double>()) &&
        v.get<double>() >= lo && v.get<double>() <= hi &&
        std::floor(v.get<double>()) == v.get<double>();
}
inline bool address(const std::string& path, sockaddr_un* addr) {
    if (path.empty() || path.size() >= sizeof(addr->sun_path)) return false;
    std::memset(addr, 0, sizeof(*addr));
    addr->sun_family = AF_UNIX;
    std::memcpy(addr->sun_path, path.c_str(), path.size() + 1);
    return true;
}
} // namespace hw
