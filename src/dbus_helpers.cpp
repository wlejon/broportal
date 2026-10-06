#include "broportal/dbus_helpers.h"

#include <cstring>
#include <iostream>
#include <stdexcept>

namespace broportal::dbus {

// --- Slot ---

Slot::~Slot() {
    reset();
}

Slot::Slot(Slot&& other) noexcept : slot_(other.slot_) {
    other.slot_ = nullptr;
}

Slot& Slot::operator=(Slot&& other) noexcept {
    if (this != &other) {
        reset();
        slot_ = other.slot_;
        other.slot_ = nullptr;
    }
    return *this;
}

void Slot::reset(sd_bus_slot* slot) noexcept {
    if (slot_) {
        sd_bus_slot_unref(slot_);
    }
    slot_ = slot;
}

sd_bus_slot* Slot::release() noexcept {
    sd_bus_slot* s = slot_;
    slot_ = nullptr;
    return s;
}

// --- Message ---

Message::~Message() {
    if (owned_ && msg_) {
        sd_bus_message_unref(msg_);
    }
}

Message::Message(Message&& other) noexcept
    : msg_(other.msg_), owned_(other.owned_) {
    other.msg_ = nullptr;
    other.owned_ = false;
}

Message& Message::operator=(Message&& other) noexcept {
    if (this != &other) {
        if (owned_ && msg_) {
            sd_bus_message_unref(msg_);
        }
        msg_ = other.msg_;
        owned_ = other.owned_;
        other.msg_ = nullptr;
        other.owned_ = false;
    }
    return *this;
}

std::string Message::get_path() const {
    const char* p = sd_bus_message_get_path(msg_);
    return p ? std::string(p) : std::string();
}

std::string Message::get_interface() const {
    const char* i = sd_bus_message_get_interface(msg_);
    return i ? std::string(i) : std::string();
}

std::string Message::get_member() const {
    const char* m = sd_bus_message_get_member(msg_);
    return m ? std::string(m) : std::string();
}

std::string Message::get_sender() const {
    const char* s = sd_bus_message_get_sender(msg_);
    return s ? std::string(s) : std::string();
}

int Message::open_container(char type, const char* contents) {
    return sd_bus_message_open_container(msg_, type, contents);
}

int Message::close_container() {
    return sd_bus_message_close_container(msg_);
}

int Message::enter_container(char type, const char* contents) {
    return sd_bus_message_enter_container(msg_, type, contents);
}

int Message::exit_container() {
    return sd_bus_message_exit_container(msg_);
}

bool Message::at_end(bool complete) const {
    return sd_bus_message_at_end(msg_, complete ? 1 : 0) > 0;
}

int Message::peek_type(char* type, const char** contents) const {
    return sd_bus_message_peek_type(msg_, type, contents);
}

bool Message::append_basic(char type, const void* value) {
    return sd_bus_message_append_basic(msg_, type, value) >= 0;
}

bool Message::append_bool(bool val) {
    int b = val ? 1 : 0;
    return append_basic('b', &b);
}

bool Message::append_byte(uint8_t val) {
    return append_basic('y', &val);
}

bool Message::append_int16(int16_t val) {
    return append_basic('n', &val);
}

bool Message::append_uint16(uint16_t val) {
    return append_basic('q', &val);
}

bool Message::append_int32(int32_t val) {
    return append_basic('i', &val);
}

bool Message::append_uint32(uint32_t val) {
    return append_basic('u', &val);
}

bool Message::append_int64(int64_t val) {
    return append_basic('x', &val);
}

bool Message::append_uint64(uint64_t val) {
    return append_basic('t', &val);
}

bool Message::append_double(double val) {
    return append_basic('d', &val);
}

bool Message::append_string(const std::string& val) {
    const char* str = val.c_str();
    return append_basic('s', str);
}

bool Message::append_object_path(const ObjectPath& val) {
    const char* str = val.path.c_str();
    return append_basic('o', str);
}

bool Message::append_unix_fd(const UnixFd& val) {
    int fd = val.fd;
    return append_basic('h', &fd);
}

bool Message::append_string_list(const std::vector<std::string>& list) {
    if (open_container('a', "s") < 0) return false;
    for (const auto& item : list) {
        if (!append_string(item)) return false;
    }
    return close_container() >= 0;
}

bool Message::append_byte_list(const std::vector<uint8_t>& bytes) {
    if (open_container('a', "y") < 0) return false;
    for (uint8_t b : bytes) {
        if (!append_byte(b)) return false;
    }
    return close_container() >= 0;
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

// Reading methods
bool Message::read_basic(char type, void* out) {
    return sd_bus_message_read_basic(msg_, type, out) > 0;
}

bool Message::read_bool(bool* out) {
    int b = 0;
    if (!read_basic('b', &b)) return false;
    if (out) *out = (b != 0);
    return true;
}

bool Message::read_byte(uint8_t* out) {
    return read_basic('y', out);
}

bool Message::read_int16(int16_t* out) {
    return read_basic('n', out);
}

bool Message::read_uint16(uint16_t* out) {
    return read_basic('q', out);
}

bool Message::read_int32(int32_t* out) {
    return read_basic('i', out);
}

bool Message::read_uint32(uint32_t* out) {
    return read_basic('u', out);
}

bool Message::read_int64(int64_t* out) {
    return read_basic('x', out);
}

bool Message::read_uint64(uint64_t* out) {
    return read_basic('t', out);
}

bool Message::read_double(double* out) {
    return read_basic('d', out);
}

bool Message::read_string(std::string* out) {
    const char* s = nullptr;
    if (!read_basic('s', &s)) return false;
    if (out) *out = (s ? s : "");
    return true;
}

bool Message::read_object_path(ObjectPath* out) {
    const char* o = nullptr;
    if (!read_basic('o', &o)) return false;
    if (out) out->path = (o ? o : "");
    return true;
}

bool Message::read_unix_fd(UnixFd* out) {
    int fd = -1;
    if (!read_basic('h', &fd)) return false;
    if (out) out->fd = fd;
    return true;
}

bool Message::read_string_list(std::vector<std::string>* out) {
    if (enter_container('a', "s") < 0) return false;
    if (out) out->clear();
    while (!at_end()) {
        std::string s;
        if (!read_string(&s)) {
            exit_container();
            return false;
        }
        if (out) out->push_back(std::move(s));
    }
    return exit_container() >= 0;
}

bool Message::read_byte_list(std::vector<uint8_t>* out) {
    if (enter_container('a', "y") < 0) return false;
    if (out) out->clear();
    while (!at_end()) {
        uint8_t b = 0;
        if (!read_byte(&b)) {
            exit_container();
            return false;
        }
        if (out) out->push_back(b);
    }
    return exit_container() >= 0;
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

Bus::~Bus() {
    if (bus_) {
        sd_bus_flush_close_unref(bus_);
        bus_ = nullptr;
    }
}

Bus::Bus(Bus&& other) noexcept : bus_(other.bus_) {
    other.bus_ = nullptr;
}

Bus& Bus::operator=(Bus&& other) noexcept {
    if (this != &other) {
        if (bus_) {
            sd_bus_flush_close_unref(bus_);
        }
        bus_ = other.bus_;
        other.bus_ = nullptr;
    }
    return *this;
}

std::unique_ptr<Bus> Bus::open_user(std::string* error) {
    sd_bus* raw_bus = nullptr;
    int r = sd_bus_open_user(&raw_bus);
    if (r < 0) {
        if (error) *error = strerror(-r);
        return nullptr;
    }
    return std::make_unique<Bus>(raw_bus);
}

std::unique_ptr<Bus> Bus::open_system(std::string* error) {
    sd_bus* raw_bus = nullptr;
    int r = sd_bus_open_system(&raw_bus);
    if (r < 0) {
        if (error) *error = strerror(-r);
        return nullptr;
    }
    return std::make_unique<Bus>(raw_bus);
}

std::unique_ptr<Bus> Bus::open_address(const std::string& address, std::string* error) {
    sd_bus* raw_bus = nullptr;
    int r = sd_bus_new(&raw_bus);
    if (r < 0) {
        if (error) *error = strerror(-r);
        return nullptr;
    }
    r = sd_bus_set_address(raw_bus, address.c_str());
    if (r < 0) {
        if (error) *error = strerror(-r);
        sd_bus_unref(raw_bus);
        return nullptr;
    }
    // A message bus, not a peer-to-peer connection: send Hello, so the
    // connection gets a unique name and can own names and register matches.
    r = sd_bus_set_bus_client(raw_bus, 1);
    if (r < 0) {
        if (error) *error = strerror(-r);
        sd_bus_unref(raw_bus);
        return nullptr;
    }
    r = sd_bus_start(raw_bus);
    if (r < 0) {
        if (error) *error = strerror(-r);
        sd_bus_unref(raw_bus);
        return nullptr;
    }
    return std::make_unique<Bus>(raw_bus);
}

int Bus::get_fd() const noexcept {
    return bus_ ? sd_bus_get_fd(bus_) : -1;
}

int Bus::process() {
    return bus_ ? sd_bus_process(bus_, nullptr) : -1;
}

int Bus::wait(uint64_t timeout_usec) {
    return bus_ ? sd_bus_wait(bus_, timeout_usec) : -1;
}

int Bus::flush() {
    return bus_ ? sd_bus_flush(bus_) : -1;
}

bool Bus::request_name(const std::string& name, uint64_t flags, std::string* error) {
    if (!bus_) return false;
    int r = sd_bus_request_name(bus_, name.c_str(), flags);
    if (r < 0) {
        if (error) *error = strerror(-r);
        return false;
    }
    return true;
}

bool Bus::release_name(const std::string& name, std::string* error) {
    if (!bus_) return false;
    int r = sd_bus_release_name(bus_, name.c_str());
    if (r < 0) {
        if (error) *error = strerror(-r);
        return false;
    }
    return true;
}

Slot Bus::add_object_vtable(
    const std::string& path,
    const std::string& interface,
    const sd_bus_vtable* vtable,
    void* userdata,
    std::string* error) {
    if (!bus_) return Slot();
    sd_bus_slot* slot = nullptr;
    int r = sd_bus_add_object_vtable(bus_, &slot, path.c_str(), interface.c_str(), vtable, userdata);
    if (r < 0) {
        if (error) *error = strerror(-r);
        return Slot();
    }
    return Slot(slot);
}

struct MatchContext {
    SignalHandler handler;
};

static int on_match_signal(sd_bus_message* m, void* userdata, sd_bus_error* /*ret_error*/) {
    auto* ctx = static_cast<MatchContext*>(userdata);
    if (ctx && ctx->handler) {
        Message msg(m, false);
        ctx->handler(msg);
    }
    return 0;
}

Slot Bus::add_match(
    const std::string& match_rule,
    SignalHandler callback,
    std::string* error) {
    if (!bus_) return Slot();
    auto ctx = std::make_unique<MatchContext>();
    ctx->handler = std::move(callback);

    sd_bus_slot* slot = nullptr;
    int r = sd_bus_add_match(bus_, &slot, match_rule.c_str(), on_match_signal, ctx.get());
    if (r < 0) {
        if (error) *error = strerror(-r);
        return Slot();
    }

    // Attach ctx lifecycle to slot userdata if desired, or release ownership
    // sd_bus_slot_set_userdata cleanup:
    sd_bus_slot_set_destroy_callback(slot, [](void* ud) {
        delete static_cast<MatchContext*>(ud);
    });
    ctx.release();

    return Slot(slot);
}

bool Bus::emit_signal(
    const std::string& path,
    const std::string& interface,
    const std::string& member,
    std::function<void(Message&)> build_args,
    std::string* error) {
    if (!bus_) return false;
    sd_bus_message* m = nullptr;
    int r = sd_bus_message_new_signal(bus_, &m, path.c_str(), interface.c_str(), member.c_str());
    if (r < 0) {
        if (error) *error = strerror(-r);
        return false;
    }

    Message msg(m, true);
    if (build_args) {
        build_args(msg);
    }

    r = sd_bus_send(bus_, msg.raw(), nullptr);
    if (r < 0) {
        if (error) *error = strerror(-r);
        return false;
    }
    return true;
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
    if (!bus_) return false;
    sd_bus_message* m = nullptr;
    int r = sd_bus_message_new_method_call(bus_, &m, destination.c_str(), path.c_str(), interface.c_str(), member.c_str());
    if (r < 0) {
        if (error) *error = strerror(-r);
        return false;
    }

    Message call_msg(m, true);
    if (build_args) {
        build_args(call_msg);
    }

    sd_bus_error sdbus_err = SD_BUS_ERROR_NULL;
    sd_bus_message* reply = nullptr;
    r = sd_bus_call(bus_, call_msg.raw(), timeout_usec, &sdbus_err, &reply);
    if (r < 0) {
        if (error) {
            *error = sdbus_err.message ? sdbus_err.message : strerror(-r);
        }
        sd_bus_error_free(&sdbus_err);
        return false;
    }

    if (parse_reply && reply) {
        Message reply_msg(reply, true);
        parse_reply(reply_msg);
    } else if (reply) {
        sd_bus_message_unref(reply);
    }

    return true;
}

} // namespace broportal::dbus
