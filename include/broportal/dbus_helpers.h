#pragma once

#include "broportal/types.h"

#include <systemd/sd-bus.h>
#include <systemd/sd-bus-vtable.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace broportal::dbus {

class Slot {
public:
    Slot() noexcept : slot_(nullptr) {}
    explicit Slot(sd_bus_slot* slot) noexcept : slot_(slot) {}
    ~Slot();

    Slot(const Slot&) = delete;
    Slot& operator=(const Slot&) = delete;

    Slot(Slot&& other) noexcept;
    Slot& operator=(Slot&& other) noexcept;

    void reset(sd_bus_slot* slot = nullptr) noexcept;
    sd_bus_slot* release() noexcept;
    sd_bus_slot* get() const noexcept { return slot_; }
    bool is_valid() const noexcept { return slot_ != nullptr; }
    explicit operator bool() const noexcept { return is_valid(); }

private:
    sd_bus_slot* slot_ = nullptr;
};

class Message {
public:
    Message() noexcept : msg_(nullptr), owned_(true) {}
    explicit Message(sd_bus_message* msg, bool owned = true) noexcept
        : msg_(msg), owned_(owned) {}
    ~Message();

    Message(const Message&) = delete;
    Message& operator=(const Message&) = delete;

    Message(Message&& other) noexcept;
    Message& operator=(Message&& other) noexcept;

    sd_bus_message* raw() const noexcept { return msg_; }
    bool is_valid() const noexcept { return msg_ != nullptr; }
    explicit operator bool() const noexcept { return is_valid(); }

    std::string get_path() const;
    std::string get_interface() const;
    std::string get_member() const;
    std::string get_sender() const;

    // Appending values
    bool append_basic(char type, const void* value);
    bool append_bool(bool val);
    bool append_byte(uint8_t val);
    bool append_int16(int16_t val);
    bool append_uint16(uint16_t val);
    bool append_int32(int32_t val);
    bool append_uint32(uint32_t val);
    bool append_int64(int64_t val);
    bool append_uint64(uint64_t val);
    bool append_double(double val);
    bool append_string(const std::string& val);
    bool append_object_path(const ObjectPath& val);
    bool append_unix_fd(const UnixFd& val);
    bool append_string_list(const std::vector<std::string>& list);
    bool append_byte_list(const std::vector<uint8_t>& bytes);
    bool append_rgb(const RgbColor& color);
    bool append_coord2d(const Coord2D& coord);
    bool append_string_pair_list(const StringPairList& pairs);
    bool append_variant(const Variant& var);
    bool append_variant_map(const VariantMap& map);
    bool append_stream_list(const StreamList& streams);
    bool append_shortcut_list(const ShortcutList& shortcuts);
    bool append_settings_map(const SettingsMap& settings);

    // Reading values
    bool read_basic(char type, void* out);
    bool read_bool(bool* out);
    bool read_byte(uint8_t* out);
    bool read_int16(int16_t* out);
    bool read_uint16(uint16_t* out);
    bool read_int32(int32_t* out);
    bool read_uint32(uint32_t* out);
    bool read_int64(int64_t* out);
    bool read_uint64(uint64_t* out);
    bool read_double(double* out);
    bool read_string(std::string* out);
    bool read_object_path(ObjectPath* out);
    bool read_unix_fd(UnixFd* out);
    bool read_string_list(std::vector<std::string>* out);
    bool read_byte_list(std::vector<uint8_t>* out);
    bool read_rgb(RgbColor* out);
    bool read_coord2d(Coord2D* out);
    bool read_string_pair_list(StringPairList* out);
    bool read_variant(Variant* out);
    bool read_variant_map(VariantMap* out);
    bool read_stream_list(StreamList* out);
    bool read_shortcut_list(ShortcutList* out);
    bool read_settings_map(SettingsMap* out);

    int enter_container(char type, const char* contents);
    int exit_container();
    int open_container(char type, const char* contents);
    int close_container();
    bool at_end(bool complete = false) const;
    int peek_type(char* type, const char** contents) const;

private:
    sd_bus_message* msg_ = nullptr;
    bool owned_ = true;
};

using SignalHandler = std::function<void(Message& msg)>;

class Bus {
public:
    Bus() noexcept : bus_(nullptr) {}
    explicit Bus(sd_bus* bus) noexcept : bus_(bus) {}
    ~Bus();

    Bus(const Bus&) = delete;
    Bus& operator=(const Bus&) = delete;

    Bus(Bus&& other) noexcept;
    Bus& operator=(Bus&& other) noexcept;

    static std::unique_ptr<Bus> open_user(std::string* error = nullptr);
    static std::unique_ptr<Bus> open_system(std::string* error = nullptr);
    static std::unique_ptr<Bus> open_address(const std::string& address, std::string* error = nullptr);

    sd_bus* raw() const noexcept { return bus_; }
    bool is_valid() const noexcept { return bus_ != nullptr; }
    explicit operator bool() const noexcept { return is_valid(); }

    int get_fd() const noexcept;
    int process();
    int wait(uint64_t timeout_usec = UINT64_MAX);
    int flush();

    bool request_name(const std::string& name, uint64_t flags = 0, std::string* error = nullptr);
    bool release_name(const std::string& name, std::string* error = nullptr);

    Slot add_object_vtable(
        const std::string& path,
        const std::string& interface,
        const sd_bus_vtable* vtable,
        void* userdata,
        std::string* error = nullptr);

    Slot add_match(
        const std::string& match_rule,
        SignalHandler callback,
        std::string* error = nullptr);

    bool emit_signal(
        const std::string& path,
        const std::string& interface,
        const std::string& member,
        std::function<void(Message&)> build_args = nullptr,
        std::string* error = nullptr);

    bool call_method(
        const std::string& destination,
        const std::string& path,
        const std::string& interface,
        const std::string& member,
        std::function<void(Message&)> build_args,
        std::function<void(Message&)> parse_reply,
        std::string* error = nullptr,
        uint64_t timeout_usec = 5000000);

private:
    sd_bus* bus_ = nullptr;
};

} // namespace broportal::dbus
