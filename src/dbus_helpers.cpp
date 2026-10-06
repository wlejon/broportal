#include "broportal/dbus_helpers.h"

#include <cstring>
#include <iostream>
#include <stdexcept>

namespace broportal::dbus {

bool Message::append_object_path(const ObjectPath& val) {
    return inner_.append_object_path(val.path);
}

bool Message::append_unix_fd(const UnixFd& val) {
    return inner_.append_unix_fd(val.fd);
}

bool Message::read_object_path(ObjectPath* out) {
    std::string s;
    if (!inner_.read_object_path(&s)) return false;
    if (out) out->path = std::move(s);
    return true;
}

bool Message::read_unix_fd(UnixFd* out) {
    int fd = -1;
    if (!inner_.read_unix_fd(&fd)) return false;
    if (out) out->fd = fd;
    return true;
}

bool Message::append_rgb(const RgbColor& color) {
    if (open_container('r', "ddd") < 0) return false;
    if (!append_double(color.r)) return false;
    if (!append_double(color.g)) return false;
    if (!append_double(color.b)) return false;
    return close_container() >= 0;
}

bool Message::append_coord2d(const Coord2D& coord) {
    if (open_container('r', "ii") < 0) return false;
    if (!append_int32(coord.x)) return false;
    if (!append_int32(coord.y)) return false;
    return close_container() >= 0;
}

bool Message::append_string_pair_list(const StringPairList& pairs) {
    if (open_container('a', "(ss)") < 0) return false;
    for (const auto& [first, second] : pairs) {
        if (open_container('r', "ss") < 0) return false;
        if (!append_string(first)) return false;
        if (!append_string(second)) return false;
        if (close_container() < 0) return false;
    }
    return close_container() >= 0;
}

bool Message::append_variant_map(const VariantMap& map) {
    if (open_container('a', "{sv}") < 0) return false;
    for (const auto& [key, var] : map) {
        if (open_container('e', "sv") < 0) return false;
        if (!append_string(key)) return false;
        if (!append_variant(var)) return false;
        if (close_container() < 0) return false;
    }
    return close_container() >= 0;
}

bool Message::append_stream_list(const StreamList& streams) {
    if (open_container('a', "(ua{sv})") < 0) return false;
    for (const auto& [node_id, props] : streams) {
        if (open_container('r', "ua{sv}") < 0) return false;
        if (!append_uint32(node_id)) return false;
        if (!append_variant_map(props)) return false;
        if (close_container() < 0) return false;
    }
    return close_container() >= 0;
}

bool Message::append_shortcut_list(const ShortcutList& shortcuts) {
    if (open_container('a', "(sa{sv})") < 0) return false;
    for (const auto& [sc_id, props] : shortcuts) {
        if (open_container('r', "sa{sv}") < 0) return false;
        if (!append_string(sc_id)) return false;
        if (!append_variant_map(props)) return false;
        if (close_container() < 0) return false;
    }
    return close_container() >= 0;
}

bool Message::append_settings_map(const SettingsMap& settings) {
    if (open_container('a', "{sa{sv}}") < 0) return false;
    for (const auto& [ns, dict] : settings) {
        if (open_container('e', "sa{sv}") < 0) return false;
        if (!append_string(ns)) return false;
        if (!append_variant_map(dict)) return false;
        if (close_container() < 0) return false;
    }
    return close_container() >= 0;
}

bool Message::append_variant(const Variant& var) {
    std::string sig = var.signature();
    if (open_container('v', sig.c_str()) < 0) return false;

    bool ok = std::visit(
        [this](const auto& val) -> bool {
            using T = std::decay_t<decltype(val)>;
            if constexpr (std::is_same_v<T, std::monostate>) {
                return true;
            } else if constexpr (std::is_same_v<T, bool>) {
                return append_bool(val);
            } else if constexpr (std::is_same_v<T, uint8_t>) {
                return append_byte(val);
            } else if constexpr (std::is_same_v<T, int16_t>) {
                return append_int16(val);
            } else if constexpr (std::is_same_v<T, uint16_t>) {
                return append_uint16(val);
            } else if constexpr (std::is_same_v<T, int32_t>) {
                return append_int32(val);
            } else if constexpr (std::is_same_v<T, uint32_t>) {
                return append_uint32(val);
            } else if constexpr (std::is_same_v<T, int64_t>) {
                return append_int64(val);
            } else if constexpr (std::is_same_v<T, uint64_t>) {
                return append_uint64(val);
            } else if constexpr (std::is_same_v<T, double>) {
                return append_double(val);
            } else if constexpr (std::is_same_v<T, std::string>) {
                return append_string(val);
            } else if constexpr (std::is_same_v<T, ObjectPath>) {
                return append_object_path(val);
            } else if constexpr (std::is_same_v<T, UnixFd>) {
                return append_unix_fd(val);
            } else if constexpr (std::is_same_v<T, std::vector<std::string>>) {
                return append_string_list(val);
            } else if constexpr (std::is_same_v<T, std::vector<uint8_t>>) {
                return append_byte_list(val);
            } else if constexpr (std::is_same_v<T, RgbColor>) {
                return append_rgb(val);
            } else if constexpr (std::is_same_v<T, Coord2D>) {
                return append_coord2d(val);
            } else if constexpr (std::is_same_v<T, StringPairList>) {
                return append_string_pair_list(val);
            } else if constexpr (std::is_same_v<T, std::shared_ptr<VariantMap>>) {
                return val ? append_variant_map(*val) : append_variant_map({});
            } else if constexpr (std::is_same_v<T, std::shared_ptr<SettingsMap>>) {
                return val ? append_settings_map(*val) : append_settings_map({});
            } else if constexpr (std::is_same_v<T, std::shared_ptr<StreamList>>) {
                return val ? append_stream_list(*val) : append_stream_list({});
            } else if constexpr (std::is_same_v<T, std::shared_ptr<ShortcutList>>) {
                return val ? append_shortcut_list(*val) : append_shortcut_list({});
            } else {
                return false;
            }
        },
        var.raw());

    if (!ok) return false;
    return close_container() >= 0;
}

bool Message::read_rgb(RgbColor* out) {
    if (enter_container('r', "ddd") < 0) return false;
    RgbColor color;
    if (!read_double(&color.r) || !read_double(&color.g) || !read_double(&color.b)) {
        exit_container();
        return false;
    }
    if (out) *out = color;
    return exit_container() >= 0;
}

bool Message::read_coord2d(Coord2D* out) {
    if (enter_container('r', "ii") < 0) return false;
    Coord2D coord;
    if (!read_int32(&coord.x) || !read_int32(&coord.y)) {
        exit_container();
        return false;
    }
    if (out) *out = coord;
    return exit_container() >= 0;
}

bool Message::read_string_pair_list(StringPairList* out) {
    if (enter_container('a', "(ss)") < 0) return false;
    if (out) out->clear();
    while (!at_end()) {
        if (enter_container('r', "ss") < 0) return false;
        std::string first, second;
        if (!read_string(&first) || !read_string(&second)) {
            exit_container();
            exit_container();
            return false;
        }
        if (exit_container() < 0) return false;
        if (out) out->emplace_back(std::move(first), std::move(second));
    }
    return exit_container() >= 0;
}

bool Message::read_variant_map(VariantMap* out) {
    if (enter_container('a', "{sv}") < 0) return false;
    if (out) out->clear();
    while (!at_end()) {
        if (enter_container('e', "sv") < 0) return false;
        std::string key;
        if (!read_string(&key)) {
            exit_container();
            exit_container();
            return false;
        }
        Variant var;
        if (!read_variant(&var)) {
            exit_container();
            exit_container();
            return false;
        }
        if (exit_container() < 0) return false;
        if (out) (*out)[key] = std::move(var);
    }
    return exit_container() >= 0;
}

bool Message::read_stream_list(StreamList* out) {
    if (enter_container('a', "(ua{sv})") < 0) return false;
    if (out) out->clear();
    while (!at_end()) {
        if (enter_container('r', "ua{sv}") < 0) return false;
        uint32_t node_id = 0;
        if (!read_uint32(&node_id)) {
            exit_container();
            exit_container();
            return false;
        }
        VariantMap props;
        if (!read_variant_map(&props)) {
            exit_container();
            exit_container();
            return false;
        }
        if (exit_container() < 0) return false;
        if (out) out->emplace_back(node_id, std::move(props));
    }
    return exit_container() >= 0;
}

bool Message::read_shortcut_list(ShortcutList* out) {
    if (enter_container('a', "(sa{sv})") < 0) return false;
    if (out) out->clear();
    while (!at_end()) {
        if (enter_container('r', "sa{sv}") < 0) return false;
        std::string id;
        if (!read_string(&id)) {
            exit_container();
            exit_container();
            return false;
        }
        VariantMap props;
        if (!read_variant_map(&props)) {
            exit_container();
            exit_container();
            return false;
        }
        if (exit_container() < 0) return false;
        if (out) out->emplace_back(std::move(id), std::move(props));
    }
    return exit_container() >= 0;
}

bool Message::read_settings_map(SettingsMap* out) {
    if (enter_container('a', "{sa{sv}}") < 0) return false;
    if (out) out->clear();
    while (!at_end()) {
        if (enter_container('e', "sa{sv}") < 0) return false;
        std::string ns;
        if (!read_string(&ns)) {
            exit_container();
            exit_container();
            return false;
        }
        VariantMap dict;
        if (!read_variant_map(&dict)) {
            exit_container();
            exit_container();
            return false;
        }
        if (exit_container() < 0) return false;
        if (out) (*out)[ns] = std::move(dict);
    }
    return exit_container() >= 0;
}

bool Message::read_variant(Variant* out) {
    char type = 0;
    const char* contents = nullptr;
    if (peek_type(&type, &contents) <= 0) return false;

    if (type != 'v') return false;

    if (enter_container('v', contents) < 0) return false;

    std::string sig(contents ? contents : "");
    bool ok = false;

    if (sig == "b") {
        bool val = false;
        if ((ok = read_bool(&val)) && out) *out = Variant(val);
    } else if (sig == "y") {
        uint8_t val = 0;
        if ((ok = read_byte(&val)) && out) *out = Variant(val);
    } else if (sig == "n") {
        int16_t val = 0;
        if ((ok = read_int16(&val)) && out) *out = Variant(val);
    } else if (sig == "q") {
        uint16_t val = 0;
        if ((ok = read_uint16(&val)) && out) *out = Variant(val);
    } else if (sig == "i") {
        int32_t val = 0;
        if ((ok = read_int32(&val)) && out) *out = Variant(val);
    } else if (sig == "u") {
        uint32_t val = 0;
        if ((ok = read_uint32(&val)) && out) *out = Variant(val);
    } else if (sig == "x") {
        int64_t val = 0;
        if ((ok = read_int64(&val)) && out) *out = Variant(val);
    } else if (sig == "t") {
        uint64_t val = 0;
        if ((ok = read_uint64(&val)) && out) *out = Variant(val);
    } else if (sig == "d") {
        double val = 0.0;
        if ((ok = read_double(&val)) && out) *out = Variant(val);
    } else if (sig == "s") {
        std::string val;
        if ((ok = read_string(&val)) && out) *out = Variant(val);
    } else if (sig == "o") {
        ObjectPath val;
        if ((ok = read_object_path(&val)) && out) *out = Variant(val);
    } else if (sig == "h") {
        UnixFd val;
        if ((ok = read_unix_fd(&val)) && out) *out = Variant(val);
    } else if (sig == "as") {
        std::vector<std::string> val;
        if ((ok = read_string_list(&val)) && out) *out = Variant(val);
    } else if (sig == "ay") {
        std::vector<uint8_t> val;
        if ((ok = read_byte_list(&val)) && out) *out = Variant(val);
    } else if (sig == "(ddd)") {
        RgbColor val;
        if ((ok = read_rgb(&val)) && out) *out = Variant(val);
    } else if (sig == "(ii)") {
        Coord2D val;
        if ((ok = read_coord2d(&val)) && out) *out = Variant(val);
    } else if (sig == "a(ss)") {
        StringPairList val;
        if ((ok = read_string_pair_list(&val)) && out) *out = Variant(val);
    } else if (sig == "a{sv}") {
        VariantMap val;
        if ((ok = read_variant_map(&val)) && out) *out = Variant(val);
    } else if (sig == "a{sa{sv}}") {
        SettingsMap val;
        if ((ok = read_settings_map(&val)) && out) *out = Variant(val);
    } else if (sig == "a(ua{sv})") {
        StreamList val;
        if ((ok = read_stream_list(&val)) && out) *out = Variant(val);
    } else if (sig == "a(sa{sv})") {
        ShortcutList val;
        if ((ok = read_shortcut_list(&val)) && out) *out = Variant(val);
    } else {
        ok = true; // unsupported complex variant stored as monostate
        if (out) *out = Variant();
    }

    if (!ok) {
        exit_container();
        return false;
    }

    return exit_container() >= 0;
}

// --- Bus ---

std::unique_ptr<Bus> Bus::open_user(std::string* error) {
    auto b = brodbus::Bus::open_user(error);
    if (!b) return nullptr;
    return std::make_unique<Bus>(std::move(*b));
}

std::unique_ptr<Bus> Bus::open_system(std::string* error) {
    auto b = brodbus::Bus::open_system(error);
    if (!b) return nullptr;
    return std::make_unique<Bus>(std::move(*b));
}

std::unique_ptr<Bus> Bus::open_address(const std::string& address, std::string* error) {
    auto b = brodbus::Bus::open_address(address, error);
    if (!b) return nullptr;
    return std::make_unique<Bus>(std::move(*b));
}

Slot Bus::add_object_vtable(
    const std::string& path,
    const std::string& interface,
    const sd_bus_vtable* vtable,
    void* userdata,
    std::string* error) {
    if (!inner_.raw()) {
        if (error) *error = "bus not connected";
        return Slot();
    }
    sd_bus_slot* slot = nullptr;
    int r = sd_bus_add_object_vtable(inner_.raw(), &slot, path.c_str(), interface.c_str(), vtable, userdata);
    if (r < 0) {
        if (error) *error = strerror(-r);
        return Slot();
    }
    return Slot(slot);
}

Slot Bus::add_match(
    const std::string& match_rule,
    SignalHandler callback,
    std::string* error) {
    return inner_.add_match(
        match_rule,
        [cb = std::move(callback)](sd_bus_message* m) {
            if (cb) {
                Message msg(m, false);
                cb(msg);
            }
        },
        error);
}

bool Bus::emit_signal(
    const std::string& path,
    const std::string& interface,
    const std::string& member,
    std::function<void(Message&)> build_args,
    std::string* error) {
    brodbus::Message raw_msg = inner_.new_signal(path, interface, member, error);
    if (!raw_msg) return false;
    Message msg(std::move(raw_msg));
    if (build_args) {
        build_args(msg);
    }
    return inner_.send(msg.inner(), nullptr, error);
}

bool Bus::call_method(
    const std::string& destination,
    const std::string& path,
    const std::string& interface,
    const std::string& member,
    std::function<void(Message&)> build_args,
    std::function<void(Message&)> parse_reply,
    std::string* error,
    uint64_t timeout_usec) {
    brodbus::Message raw_call = inner_.new_method_call(destination, path, interface, member, error);
    if (!raw_call) return false;
    Message call_msg(std::move(raw_call));
    if (build_args) {
        build_args(call_msg);
    }
    brodbus::Error err;
    brodbus::Message reply = inner_.call(call_msg.inner(), timeout_usec, &err);
    if (!reply) {
        if (error) {
            std::string s = err.to_string();
            *error = !s.empty() ? s : "call failed";
        }
        return false;
    }
    if (parse_reply) {
        Message reply_msg(std::move(reply));
        parse_reply(reply_msg);
    }
    return true;
}

} // namespace broportal::dbus
