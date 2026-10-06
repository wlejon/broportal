#pragma once

#if !defined(__linux__)
#error "broportal's D-Bus backend is Linux-only; off Linux use broportal/availability.h and broportal/types.h"
#endif

#include "broportal/types.h"
#include "brodbus/brodbus.h"

#include <systemd/sd-bus.h>
#include <systemd/sd-bus-vtable.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace broportal::dbus {

using Slot = brodbus::Slot;

class Message {
public:
    Message() noexcept : inner_() {}
    explicit Message(sd_bus_message* msg, bool owned = true) noexcept
        : inner_(msg, owned) {}
    explicit Message(brodbus::Message msg) noexcept
        : inner_(std::move(msg)) {}
    ~Message() = default;

    Message(const Message&) = delete;
    Message& operator=(const Message&) = delete;

    Message(Message&& other) noexcept = default;
    Message& operator=(Message&& other) noexcept = default;

    sd_bus_message* raw() const noexcept { return inner_.raw(); }
    bool is_valid() const noexcept { return inner_.is_valid(); }
    explicit operator bool() const noexcept { return is_valid(); }

    void reset(sd_bus_message* msg = nullptr, bool owned = true) noexcept { inner_.reset(msg, owned); }
    sd_bus_message* release() noexcept { return inner_.release(); }

    brodbus::Message& inner() noexcept { return inner_; }
    const brodbus::Message& inner() const noexcept { return inner_; }

    operator brodbus::Message&() noexcept { return inner_; }
    operator const brodbus::Message&() const noexcept { return inner_; }

    std::string get_path() const { return inner_.get_path(); }
    std::string get_interface() const { return inner_.get_interface(); }
    std::string get_member() const { return inner_.get_member(); }
    std::string get_sender() const { return inner_.get_sender(); }
    std::string get_destination() const { return inner_.get_destination(); }

    // Appending values
    bool append_basic(char type, const void* value) { return inner_.append_basic(type, value); }
    bool append_bool(bool val) { return inner_.append_bool(val); }
    bool append_byte(uint8_t val) { return inner_.append_byte(val); }
    bool append_int16(int16_t val) { return inner_.append_int16(val); }
    bool append_uint16(uint16_t val) { return inner_.append_uint16(val); }
    bool append_int32(int32_t val) { return inner_.append_int32(val); }
    bool append_uint32(uint32_t val) { return inner_.append_uint32(val); }
    bool append_int64(int64_t val) { return inner_.append_int64(val); }
    bool append_uint64(uint64_t val) { return inner_.append_uint64(val); }
    bool append_double(double val) { return inner_.append_double(val); }
    bool append_string(const std::string& val) { return inner_.append_string(val); }
    bool append_string(const char* val) { return inner_.append_string(val); }
    bool append_object_path(const ObjectPath& val);
    bool append_unix_fd(const UnixFd& val);
    bool append_string_list(const std::vector<std::string>& list) { return inner_.append_string_list(list); }
    bool append_byte_list(const std::vector<uint8_t>& bytes) { return inner_.append_byte_list(bytes); }
    bool append_rgb(const RgbColor& color);
    bool append_coord2d(const Coord2D& coord);
    bool append_string_pair_list(const StringPairList& pairs);
    bool append_variant(const Variant& var);
    bool append_variant_map(const VariantMap& map);
    bool append_stream_list(const StreamList& streams);
    bool append_shortcut_list(const ShortcutList& shortcuts);
    bool append_settings_map(const SettingsMap& settings);

    // Reading values
    bool read_basic(char type, void* out) { return inner_.read_basic(type, out); }
    bool read_bool(bool* out) { return inner_.read_bool(out); }
    bool read_byte(uint8_t* out) { return inner_.read_byte(out); }
    bool read_int16(int16_t* out) { return inner_.read_int16(out); }
    bool read_uint16(uint16_t* out) { return inner_.read_uint16(out); }
    bool read_int32(int32_t* out) { return inner_.read_int32(out); }
    bool read_uint32(uint32_t* out) { return inner_.read_uint32(out); }
    bool read_int64(int64_t* out) { return inner_.read_int64(out); }
    bool read_uint64(uint64_t* out) { return inner_.read_uint64(out); }
    bool read_double(double* out) { return inner_.read_double(out); }
    bool read_string(std::string* out) { return inner_.read_string(out); }
    bool read_object_path(ObjectPath* out);
    bool read_unix_fd(UnixFd* out);
    bool read_string_list(std::vector<std::string>* out) { return inner_.read_string_list(out); }
    bool read_byte_list(std::vector<uint8_t>* out) { return inner_.read_byte_list(out); }
    bool read_rgb(RgbColor* out);
    bool read_coord2d(Coord2D* out);
    bool read_string_pair_list(StringPairList* out);
    bool read_variant(Variant* out);
    bool read_variant_map(VariantMap* out);
    bool read_stream_list(StreamList* out);
    bool read_shortcut_list(ShortcutList* out);
    bool read_settings_map(SettingsMap* out);

    int enter_container(char type, const char* contents = nullptr) { return inner_.enter_container(type, contents); }
    int exit_container() { return inner_.exit_container(); }
    int open_container(char type, const char* contents = nullptr) { return inner_.open_container(type, contents); }
    int close_container() { return inner_.close_container(); }
    bool at_end(bool complete = false) const { return inner_.at_end(complete); }
    int peek_type(char* type = nullptr, const char** contents = nullptr) const { return inner_.peek_type(type, contents); }

private:
    brodbus::Message inner_;
};

using SignalHandler = std::function<void(Message& msg)>;

class Bus {
public:
    Bus() noexcept = default;
    explicit Bus(sd_bus* bus) noexcept : inner_(bus) {}
    explicit Bus(brodbus::Bus bus) noexcept : inner_(std::move(bus)) {}
    ~Bus() = default;

    Bus(const Bus&) = delete;
    Bus& operator=(const Bus&) = delete;

    Bus(Bus&& other) noexcept = default;
    Bus& operator=(Bus&& other) noexcept = default;

    static std::unique_ptr<Bus> open_user(std::string* error = nullptr);
    static std::unique_ptr<Bus> open_system(std::string* error = nullptr);
    static std::unique_ptr<Bus> open_address(const std::string& address, std::string* error = nullptr);

    sd_bus* raw() const noexcept { return inner_.raw(); }
    bool is_valid() const noexcept { return inner_.is_valid(); }
    explicit operator bool() const noexcept { return is_valid(); }

    int get_fd() const noexcept { return inner_.get_fd(); }
    int process() { return inner_.process(); }
    int wait(uint64_t timeout_usec = UINT64_MAX) { return inner_.wait(timeout_usec); }
    int flush() { return inner_.flush(); }

    bool request_name(const std::string& name, uint64_t flags = 0, std::string* error = nullptr) {
        return inner_.request_name(name, flags, error);
    }
    bool release_name(const std::string& name, std::string* error = nullptr) {
        return inner_.release_name(name, error);
    }

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

    brodbus::Bus& inner() noexcept { return inner_; }
    const brodbus::Bus& inner() const noexcept { return inner_; }

private:
    brodbus::Bus inner_;
};

} // namespace broportal::dbus
