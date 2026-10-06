#pragma once

#include "broportal/dbus_helpers.h"
#include "broportal/types.h"

#include <functional>
#include <memory>
#include <string>

namespace broportal {

class PortalBackend;

enum class DeviceType : uint32_t {
    Keyboard = 1,
    Pointer = 2,
    Touchscreen = 4
};

struct RemoteDesktopInputListener {
    std::function<void(const ObjectPath& session, int32_t keycode, uint32_t state)> on_keyboard_keycode;
    std::function<void(const ObjectPath& session, int32_t keysym, uint32_t state)> on_keyboard_keysym;
    std::function<void(const ObjectPath& session, double dx, double dy)> on_pointer_motion;
    std::function<void(const ObjectPath& session, uint32_t stream, double x, double y)> on_pointer_motion_absolute;
    std::function<void(const ObjectPath& session, int32_t button, uint32_t state)> on_pointer_button;
    std::function<void(const ObjectPath& session, double dx, double dy, bool finish)> on_pointer_axis;
    std::function<void(const ObjectPath& session, uint32_t axis, int32_t steps)> on_pointer_axis_discrete;
    std::function<void(const ObjectPath& session, uint32_t stream, uint32_t slot, double x, double y)> on_touch_down;
    std::function<void(const ObjectPath& session, uint32_t stream, uint32_t slot, double x, double y)> on_touch_motion;
    std::function<void(const ObjectPath& session, uint32_t slot)> on_touch_up;
};

// Grants (or refuses) a session's remote control: the host asks the user,
// may narrow `devices` (the DeviceType bits SelectDevices asked for), and
// answers. Without one, Start answers OtherError: nobody granted control.
using RemoteDesktopStartCallback = std::function<ResponseCode(
    const ObjectPath& handle,
    const ObjectPath& session_handle,
    const std::string& app_id,
    uint32_t& devices,
    VariantMap& out_results)>;

class RemoteDesktopInterface {
public:
    explicit RemoteDesktopInterface(PortalBackend& backend);
    ~RemoteDesktopInterface();

    void set_input_listener(RemoteDesktopInputListener listener) {
        listener_ = std::move(listener);
    }

    void set_start_callback(RemoteDesktopStartCallback callback) {
        start_callback_ = std::move(callback);
    }

    // True once Start granted `session` the device: the Notify* methods are
    // refused (AccessDenied) for any other session or device.
    bool input_allowed(const ObjectPath& session, DeviceType device) const;

    const RemoteDesktopInputListener& input_listener() const noexcept { return listener_; }

    ResponseCode create_session(
        const ObjectPath& handle,
        const ObjectPath& session_handle,
        const std::string& app_id,
        const VariantMap& options,
        VariantMap& results);

    ResponseCode select_devices(
        const ObjectPath& handle,
        const ObjectPath& session_handle,
        const std::string& app_id,
        const VariantMap& options,
        VariantMap& results);

    ResponseCode start(
        const ObjectPath& handle,
        const ObjectPath& session_handle,
        const std::string& app_id,
        const std::string& parent_window,
        const VariantMap& options,
        VariantMap& results);

    // Event notifications
    void notify_keyboard_keycode(const ObjectPath& session, const VariantMap& options, int32_t keycode, uint32_t state);
    void notify_keyboard_keysym(const ObjectPath& session, const VariantMap& options, int32_t keysym, uint32_t state);
    void notify_pointer_motion(const ObjectPath& session, const VariantMap& options, double dx, double dy);
    void notify_pointer_motion_absolute(const ObjectPath& session, const VariantMap& options, uint32_t stream, double x, double y);
    void notify_pointer_button(const ObjectPath& session, const VariantMap& options, int32_t button, uint32_t state);
    void notify_pointer_axis(const ObjectPath& session, const VariantMap& options, double dx, double dy);
    void notify_pointer_axis_discrete(const ObjectPath& session, const VariantMap& options, uint32_t axis, int32_t steps);
    void notify_touch_down(const ObjectPath& session, const VariantMap& options, uint32_t stream, uint32_t slot, double x, double y);
    void notify_touch_motion(const ObjectPath& session, const VariantMap& options, uint32_t stream, uint32_t slot, double x, double y);
    void notify_touch_up(const ObjectPath& session, const VariantMap& options, uint32_t slot);

    // D-Bus method and property handlers
    static int dbus_create_session(sd_bus_message* m, void* userdata, sd_bus_error* ret_error);
    static int dbus_select_devices(sd_bus_message* m, void* userdata, sd_bus_error* ret_error);
    static int dbus_start(sd_bus_message* m, void* userdata, sd_bus_error* ret_error);
    static int dbus_notify_keyboard_keycode(sd_bus_message* m, void* userdata, sd_bus_error* ret_error);
    static int dbus_notify_keyboard_keysym(sd_bus_message* m, void* userdata, sd_bus_error* ret_error);
    static int dbus_notify_pointer_motion(sd_bus_message* m, void* userdata, sd_bus_error* ret_error);
    static int dbus_notify_pointer_motion_absolute(sd_bus_message* m, void* userdata, sd_bus_error* ret_error);
    static int dbus_notify_pointer_button(sd_bus_message* m, void* userdata, sd_bus_error* ret_error);
    static int dbus_notify_pointer_axis(sd_bus_message* m, void* userdata, sd_bus_error* ret_error);
    static int dbus_notify_pointer_axis_discrete(sd_bus_message* m, void* userdata, sd_bus_error* ret_error);
    static int dbus_notify_touch_down(sd_bus_message* m, void* userdata, sd_bus_error* ret_error);
    static int dbus_notify_touch_motion(sd_bus_message* m, void* userdata, sd_bus_error* ret_error);
    static int dbus_notify_touch_up(sd_bus_message* m, void* userdata, sd_bus_error* ret_error);
    static int dbus_get_property(
        sd_bus* bus,
        const char* path,
        const char* interface,
        const char* property,
        sd_bus_message* reply,
        void* userdata,
        sd_bus_error* ret_error);

    static const sd_bus_vtable vtable[];

private:
    PortalBackend& backend_;
    RemoteDesktopInputListener listener_;
    RemoteDesktopStartCallback start_callback_;
};

} // namespace broportal
