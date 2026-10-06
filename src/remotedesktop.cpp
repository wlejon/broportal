#include "broportal/remotedesktop.h"
#include "broportal/backend.h"

#include <iostream>

namespace broportal {

const sd_bus_vtable RemoteDesktopInterface::vtable[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_METHOD("CreateSession", "oosa{sv}", "ua{sv}", &RemoteDesktopInterface::dbus_create_session, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("SelectDevices", "oosa{sv}", "ua{sv}", &RemoteDesktopInterface::dbus_select_devices, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("Start", "oossa{sv}", "ua{sv}", &RemoteDesktopInterface::dbus_start, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("NotifyKeyboardKeycode", "oa{sv}iu", "", &RemoteDesktopInterface::dbus_notify_keyboard_keycode, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("NotifyKeyboardKeysym", "oa{sv}iu", "", &RemoteDesktopInterface::dbus_notify_keyboard_keysym, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("NotifyPointerMotion", "oa{sv}dd", "", &RemoteDesktopInterface::dbus_notify_pointer_motion, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("NotifyPointerMotionAbsolute", "oa{sv}udd", "", &RemoteDesktopInterface::dbus_notify_pointer_motion_absolute, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("NotifyPointerButton", "oa{sv}iu", "", &RemoteDesktopInterface::dbus_notify_pointer_button, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("NotifyPointerAxis", "oa{sv}dd", "", &RemoteDesktopInterface::dbus_notify_pointer_axis, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("NotifyPointerAxisDiscrete", "oa{sv}ui", "", &RemoteDesktopInterface::dbus_notify_pointer_axis_discrete, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("NotifyTouchDown", "oa{sv}uudd", "", &RemoteDesktopInterface::dbus_notify_touch_down, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("NotifyTouchMotion", "oa{sv}uudd", "", &RemoteDesktopInterface::dbus_notify_touch_motion, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("NotifyTouchUp", "oa{sv}u", "", &RemoteDesktopInterface::dbus_notify_touch_up, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_PROPERTY("AvailableDeviceTypes", "u", &RemoteDesktopInterface::dbus_get_property, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("version", "u", &RemoteDesktopInterface::dbus_get_property, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_VTABLE_END
};

RemoteDesktopInterface::RemoteDesktopInterface(PortalBackend& backend)
    : backend_(backend) {}

RemoteDesktopInterface::~RemoteDesktopInterface() = default;

ResponseCode RemoteDesktopInterface::create_session(
    const ObjectPath& /*handle*/,
    const ObjectPath& session_handle,
    const std::string& app_id,
    const VariantMap& /*options*/,
    VariantMap& results) {
    auto session = backend_.create_session(session_handle, app_id, SessionType::RemoteDesktop);
    if (!session) {
        return ResponseCode::OtherError;
    }

    std::string sess_id = "remotedesktop-" + std::to_string(reinterpret_cast<uintptr_t>(session.get()));
    session->set_session_id(sess_id);
    results["session_id"] = Variant(sess_id);

    return ResponseCode::Success;
}

ResponseCode RemoteDesktopInterface::select_devices(
    const ObjectPath& /*handle*/,
    const ObjectPath& session_handle,
    const std::string& /*app_id*/,
    const VariantMap& options,
    VariantMap& /*results*/) {
    auto session = backend_.get_session(session_handle);
    if (!session) {
        return ResponseCode::OtherError;
    }

    uint32_t types = get_uint32_or(options, "types", 7);
    session->set_context("device_types", types);
    return ResponseCode::Success;
}

ResponseCode RemoteDesktopInterface::start(
    const ObjectPath& handle,
    const ObjectPath& session_handle,
    const std::string& app_id,
    const std::string& /*parent_window*/,
    const VariantMap& /*options*/,
    VariantMap& results) {
    auto session = backend_.get_session(session_handle);
    if (!session || session->type() != SessionType::RemoteDesktop) {
        return ResponseCode::OtherError;
    }
    // Control of the keyboard and pointer is the user's to give, through the
    // host; with no host to ask, nothing is granted.
    if (!start_callback_) {
        return ResponseCode::OtherError;
    }

    uint32_t devices = 7;
    if (session->has_context("device_types")) {
        devices = std::any_cast<uint32_t>(session->get_context("device_types"));
    }
    const uint32_t requested = devices;

    ResponseCode code = start_callback_(handle, session_handle, app_id, devices, results);
    if (code != ResponseCode::Success) {
        return code;
    }
    devices &= requested;  // the host may narrow the request, never widen it
    session->set_context("granted_devices", devices);
    results["devices"] = Variant(devices);
    if (!results.contains("clipboard_enabled")) {
        results["clipboard_enabled"] = Variant(false);
    }
    return ResponseCode::Success;
}

bool RemoteDesktopInterface::input_allowed(const ObjectPath& session_handle, DeviceType device) const {
    auto session = backend_.get_session(session_handle);
    if (!session || !session->has_context("granted_devices")) {
        return false;
    }
    uint32_t granted = std::any_cast<uint32_t>(session->get_context("granted_devices"));
    return (granted & static_cast<uint32_t>(device)) != 0;
}

namespace {

int deny_input(sd_bus_message* m, const ObjectPath& session) {
    return sd_bus_reply_method_errorf(m, SD_BUS_ERROR_ACCESS_DENIED,
                                      "Session %s was not granted that device", session.path.c_str());
}

}  // namespace

void RemoteDesktopInterface::notify_keyboard_keycode(
    const ObjectPath& session,
    const VariantMap& /*options*/,
    int32_t keycode,
    uint32_t state) {
    if (listener_.on_keyboard_keycode) {
        listener_.on_keyboard_keycode(session, keycode, state);
    }
}

void RemoteDesktopInterface::notify_keyboard_keysym(
    const ObjectPath& session,
    const VariantMap& /*options*/,
    int32_t keysym,
    uint32_t state) {
    if (listener_.on_keyboard_keysym) {
        listener_.on_keyboard_keysym(session, keysym, state);
    }
}

void RemoteDesktopInterface::notify_pointer_motion(
    const ObjectPath& session,
    const VariantMap& /*options*/,
    double dx,
    double dy) {
    if (listener_.on_pointer_motion) {
        listener_.on_pointer_motion(session, dx, dy);
    }
}

void RemoteDesktopInterface::notify_pointer_motion_absolute(
    const ObjectPath& session,
    const VariantMap& /*options*/,
    uint32_t stream,
    double x,
    double y) {
    if (listener_.on_pointer_motion_absolute) {
        listener_.on_pointer_motion_absolute(session, stream, x, y);
    }
}

void RemoteDesktopInterface::notify_pointer_button(
    const ObjectPath& session,
    const VariantMap& /*options*/,
    int32_t button,
    uint32_t state) {
    if (listener_.on_pointer_button) {
        listener_.on_pointer_button(session, button, state);
    }
}

void RemoteDesktopInterface::notify_pointer_axis(
    const ObjectPath& session,
    const VariantMap& options,
    double dx,
    double dy) {
    bool finish = get_bool_or(options, "finish", false);
    if (listener_.on_pointer_axis) {
        listener_.on_pointer_axis(session, dx, dy, finish);
    }
}

void RemoteDesktopInterface::notify_pointer_axis_discrete(
    const ObjectPath& session,
    const VariantMap& /*options*/,
    uint32_t axis,
    int32_t steps) {
    if (listener_.on_pointer_axis_discrete) {
        listener_.on_pointer_axis_discrete(session, axis, steps);
    }
}

void RemoteDesktopInterface::notify_touch_down(
    const ObjectPath& session,
    const VariantMap& /*options*/,
    uint32_t stream,
    uint32_t slot,
    double x,
    double y) {
    if (listener_.on_touch_down) {
        listener_.on_touch_down(session, stream, slot, x, y);
    }
}

void RemoteDesktopInterface::notify_touch_motion(
    const ObjectPath& session,
    const VariantMap& /*options*/,
    uint32_t stream,
    uint32_t slot,
    double x,
    double y) {
    if (listener_.on_touch_motion) {
        listener_.on_touch_motion(session, stream, slot, x, y);
    }
}

void RemoteDesktopInterface::notify_touch_up(
    const ObjectPath& session,
    const VariantMap& /*options*/,
    uint32_t slot) {
    if (listener_.on_touch_up) {
        listener_.on_touch_up(session, slot);
    }
}

int RemoteDesktopInterface::dbus_create_session(sd_bus_message* m, void* userdata, sd_bus_error* /*ret_error*/) {
    auto* self = static_cast<RemoteDesktopInterface*>(userdata);
    dbus::Message msg(m, false);

    ObjectPath handle, session_handle;
    std::string app_id;
    VariantMap options;

    if (!msg.read_object_path(&handle) ||
        !msg.read_object_path(&session_handle) ||
        !msg.read_string(&app_id) ||
        !msg.read_variant_map(&options)) {
        return sd_bus_reply_method_errorf(m, SD_BUS_ERROR_INVALID_ARGS, "Invalid parameters");
    }

    auto req = self->backend_.create_request(handle, app_id);
    VariantMap results;
    ResponseCode code = self->create_session(handle, session_handle, app_id, options, results);

    sd_bus_message* reply = nullptr;
    int r = sd_bus_message_new_method_return(m, &reply);
    if (r < 0) return r;

    dbus::Message reply_msg(reply, true);
    reply_msg.append_uint32(static_cast<uint32_t>(code));
    reply_msg.append_variant_map(results);

    if (req) req->close();
    return sd_bus_send(self->backend_.bus().raw(), reply_msg.raw(), nullptr);
}

int RemoteDesktopInterface::dbus_select_devices(sd_bus_message* m, void* userdata, sd_bus_error* /*ret_error*/) {
    auto* self = static_cast<RemoteDesktopInterface*>(userdata);
    dbus::Message msg(m, false);

    ObjectPath handle, session_handle;
    std::string app_id;
    VariantMap options;

    if (!msg.read_object_path(&handle) ||
        !msg.read_object_path(&session_handle) ||
        !msg.read_string(&app_id) ||
        !msg.read_variant_map(&options)) {
        return sd_bus_reply_method_errorf(m, SD_BUS_ERROR_INVALID_ARGS, "Invalid parameters");
    }

    auto req = self->backend_.create_request(handle, app_id);
    VariantMap results;
    ResponseCode code = self->select_devices(handle, session_handle, app_id, options, results);

    sd_bus_message* reply = nullptr;
    int r = sd_bus_message_new_method_return(m, &reply);
    if (r < 0) return r;

    dbus::Message reply_msg(reply, true);
    reply_msg.append_uint32(static_cast<uint32_t>(code));
    reply_msg.append_variant_map(results);

    if (req) req->close();
    return sd_bus_send(self->backend_.bus().raw(), reply_msg.raw(), nullptr);
}

int RemoteDesktopInterface::dbus_start(sd_bus_message* m, void* userdata, sd_bus_error* /*ret_error*/) {
    auto* self = static_cast<RemoteDesktopInterface*>(userdata);
    dbus::Message msg(m, false);

    ObjectPath handle, session_handle;
    std::string app_id, parent_window;
    VariantMap options;

    if (!msg.read_object_path(&handle) ||
        !msg.read_object_path(&session_handle) ||
        !msg.read_string(&app_id) ||
        !msg.read_string(&parent_window) ||
        !msg.read_variant_map(&options)) {
        return sd_bus_reply_method_errorf(m, SD_BUS_ERROR_INVALID_ARGS, "Invalid parameters");
    }

    auto req = self->backend_.create_request(handle, app_id);
    VariantMap results;
    ResponseCode code = self->start(handle, session_handle, app_id, parent_window, options, results);

    sd_bus_message* reply = nullptr;
    int r = sd_bus_message_new_method_return(m, &reply);
    if (r < 0) return r;

    dbus::Message reply_msg(reply, true);
    reply_msg.append_uint32(static_cast<uint32_t>(code));
    reply_msg.append_variant_map(results);

    if (req) req->close();
    return sd_bus_send(self->backend_.bus().raw(), reply_msg.raw(), nullptr);
}

int RemoteDesktopInterface::dbus_notify_keyboard_keycode(sd_bus_message* m, void* userdata, sd_bus_error* /*ret_error*/) {
    auto* self = static_cast<RemoteDesktopInterface*>(userdata);
    dbus::Message msg(m, false);

    ObjectPath session;
    VariantMap options;
    int32_t keycode = 0;
    uint32_t state = 0;

    if (!msg.read_object_path(&session) ||
        !msg.read_variant_map(&options) ||
        !msg.read_int32(&keycode) ||
        !msg.read_uint32(&state)) {
        return sd_bus_reply_method_errorf(m, SD_BUS_ERROR_INVALID_ARGS, "Invalid parameters");
    }

    if (!self->input_allowed(session, DeviceType::Keyboard)) return deny_input(m, session);
    self->notify_keyboard_keycode(session, options, keycode, state);
    return sd_bus_reply_method_return(m, "");
}

int RemoteDesktopInterface::dbus_notify_keyboard_keysym(sd_bus_message* m, void* userdata, sd_bus_error* /*ret_error*/) {
    auto* self = static_cast<RemoteDesktopInterface*>(userdata);
    dbus::Message msg(m, false);

    ObjectPath session;
    VariantMap options;
    int32_t keysym = 0;
    uint32_t state = 0;

    if (!msg.read_object_path(&session) ||
        !msg.read_variant_map(&options) ||
        !msg.read_int32(&keysym) ||
        !msg.read_uint32(&state)) {
        return sd_bus_reply_method_errorf(m, SD_BUS_ERROR_INVALID_ARGS, "Invalid parameters");
    }

    if (!self->input_allowed(session, DeviceType::Keyboard)) return deny_input(m, session);
    self->notify_keyboard_keysym(session, options, keysym, state);
    return sd_bus_reply_method_return(m, "");
}

int RemoteDesktopInterface::dbus_notify_pointer_motion(sd_bus_message* m, void* userdata, sd_bus_error* /*ret_error*/) {
    auto* self = static_cast<RemoteDesktopInterface*>(userdata);
    dbus::Message msg(m, false);

    ObjectPath session;
    VariantMap options;
    double dx = 0.0, dy = 0.0;

    if (!msg.read_object_path(&session) ||
        !msg.read_variant_map(&options) ||
        !msg.read_double(&dx) ||
        !msg.read_double(&dy)) {
        return sd_bus_reply_method_errorf(m, SD_BUS_ERROR_INVALID_ARGS, "Invalid parameters");
    }

    if (!self->input_allowed(session, DeviceType::Pointer)) return deny_input(m, session);
    self->notify_pointer_motion(session, options, dx, dy);
    return sd_bus_reply_method_return(m, "");
}

int RemoteDesktopInterface::dbus_notify_pointer_motion_absolute(sd_bus_message* m, void* userdata, sd_bus_error* /*ret_error*/) {
    auto* self = static_cast<RemoteDesktopInterface*>(userdata);
    dbus::Message msg(m, false);

    ObjectPath session;
    VariantMap options;
    uint32_t stream = 0;
    double x = 0.0, y = 0.0;

    if (!msg.read_object_path(&session) ||
        !msg.read_variant_map(&options) ||
        !msg.read_uint32(&stream) ||
        !msg.read_double(&x) ||
        !msg.read_double(&y)) {
        return sd_bus_reply_method_errorf(m, SD_BUS_ERROR_INVALID_ARGS, "Invalid parameters");
    }

    if (!self->input_allowed(session, DeviceType::Pointer)) return deny_input(m, session);
    self->notify_pointer_motion_absolute(session, options, stream, x, y);
    return sd_bus_reply_method_return(m, "");
}

int RemoteDesktopInterface::dbus_notify_pointer_button(sd_bus_message* m, void* userdata, sd_bus_error* /*ret_error*/) {
    auto* self = static_cast<RemoteDesktopInterface*>(userdata);
    dbus::Message msg(m, false);

    ObjectPath session;
    VariantMap options;
    int32_t button = 0;
    uint32_t state = 0;

    if (!msg.read_object_path(&session) ||
        !msg.read_variant_map(&options) ||
        !msg.read_int32(&button) ||
        !msg.read_uint32(&state)) {
        return sd_bus_reply_method_errorf(m, SD_BUS_ERROR_INVALID_ARGS, "Invalid parameters");
    }

    if (!self->input_allowed(session, DeviceType::Pointer)) return deny_input(m, session);
    self->notify_pointer_button(session, options, button, state);
    return sd_bus_reply_method_return(m, "");
}

int RemoteDesktopInterface::dbus_notify_pointer_axis(sd_bus_message* m, void* userdata, sd_bus_error* /*ret_error*/) {
    auto* self = static_cast<RemoteDesktopInterface*>(userdata);
    dbus::Message msg(m, false);

    ObjectPath session;
    VariantMap options;
    double dx = 0.0, dy = 0.0;

    if (!msg.read_object_path(&session) ||
        !msg.read_variant_map(&options) ||
        !msg.read_double(&dx) ||
        !msg.read_double(&dy)) {
        return sd_bus_reply_method_errorf(m, SD_BUS_ERROR_INVALID_ARGS, "Invalid parameters");
    }

    if (!self->input_allowed(session, DeviceType::Pointer)) return deny_input(m, session);
    self->notify_pointer_axis(session, options, dx, dy);
    return sd_bus_reply_method_return(m, "");
}

int RemoteDesktopInterface::dbus_notify_pointer_axis_discrete(sd_bus_message* m, void* userdata, sd_bus_error* /*ret_error*/) {
    auto* self = static_cast<RemoteDesktopInterface*>(userdata);
    dbus::Message msg(m, false);

    ObjectPath session;
    VariantMap options;
    uint32_t axis = 0;
    int32_t steps = 0;

    if (!msg.read_object_path(&session) ||
        !msg.read_variant_map(&options) ||
        !msg.read_uint32(&axis) ||
        !msg.read_int32(&steps)) {
        return sd_bus_reply_method_errorf(m, SD_BUS_ERROR_INVALID_ARGS, "Invalid parameters");
    }

    if (!self->input_allowed(session, DeviceType::Pointer)) return deny_input(m, session);
    self->notify_pointer_axis_discrete(session, options, axis, steps);
    return sd_bus_reply_method_return(m, "");
}

int RemoteDesktopInterface::dbus_notify_touch_down(sd_bus_message* m, void* userdata, sd_bus_error* /*ret_error*/) {
    auto* self = static_cast<RemoteDesktopInterface*>(userdata);
    dbus::Message msg(m, false);

    ObjectPath session;
    VariantMap options;
    uint32_t stream = 0, slot = 0;
    double x = 0.0, y = 0.0;

    if (!msg.read_object_path(&session) ||
        !msg.read_variant_map(&options) ||
        !msg.read_uint32(&stream) ||
        !msg.read_uint32(&slot) ||
        !msg.read_double(&x) ||
        !msg.read_double(&y)) {
        return sd_bus_reply_method_errorf(m, SD_BUS_ERROR_INVALID_ARGS, "Invalid parameters");
    }

    if (!self->input_allowed(session, DeviceType::Touchscreen)) return deny_input(m, session);
    self->notify_touch_down(session, options, stream, slot, x, y);
    return sd_bus_reply_method_return(m, "");
}

int RemoteDesktopInterface::dbus_notify_touch_motion(sd_bus_message* m, void* userdata, sd_bus_error* /*ret_error*/) {
    auto* self = static_cast<RemoteDesktopInterface*>(userdata);
    dbus::Message msg(m, false);

    ObjectPath session;
    VariantMap options;
    uint32_t stream = 0, slot = 0;
    double x = 0.0, y = 0.0;

    if (!msg.read_object_path(&session) ||
        !msg.read_variant_map(&options) ||
        !msg.read_uint32(&stream) ||
        !msg.read_uint32(&slot) ||
        !msg.read_double(&x) ||
        !msg.read_double(&y)) {
        return sd_bus_reply_method_errorf(m, SD_BUS_ERROR_INVALID_ARGS, "Invalid parameters");
    }

    if (!self->input_allowed(session, DeviceType::Touchscreen)) return deny_input(m, session);
    self->notify_touch_motion(session, options, stream, slot, x, y);
    return sd_bus_reply_method_return(m, "");
}

int RemoteDesktopInterface::dbus_notify_touch_up(sd_bus_message* m, void* userdata, sd_bus_error* /*ret_error*/) {
    auto* self = static_cast<RemoteDesktopInterface*>(userdata);
    dbus::Message msg(m, false);

    ObjectPath session;
    VariantMap options;
    uint32_t slot = 0;

    if (!msg.read_object_path(&session) ||
        !msg.read_variant_map(&options) ||
        !msg.read_uint32(&slot)) {
        return sd_bus_reply_method_errorf(m, SD_BUS_ERROR_INVALID_ARGS, "Invalid parameters");
    }

    if (!self->input_allowed(session, DeviceType::Touchscreen)) return deny_input(m, session);
    self->notify_touch_up(session, options, slot);
    return sd_bus_reply_method_return(m, "");
}

int RemoteDesktopInterface::dbus_get_property(
    sd_bus* /*bus*/,
    const char* /*path*/,
    const char* /*interface*/,
    const char* property,
    sd_bus_message* reply,
    void* /*userdata*/,
    sd_bus_error* /*ret_error*/) {
    if (std::string(property) == "AvailableDeviceTypes") {
        uint32_t val = 7; // KEYBOARD (1) | POINTER (2) | TOUCHSCREEN (4)
        return sd_bus_message_append_basic(reply, 'u', &val);
    } else if (std::string(property) == "version") {
        uint32_t val = 2;
        return sd_bus_message_append_basic(reply, 'u', &val);
    }
    return -EINVAL;
}

} // namespace broportal
