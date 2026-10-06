#include "broportal/screenshot.h"
#include "broportal/backend.h"

#include <cerrno>
#include <string>

namespace broportal {

const sd_bus_vtable ScreenshotInterface::vtable[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_METHOD("Screenshot", "ossa{sv}", "ua{sv}", &ScreenshotInterface::dbus_screenshot, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("PickColor", "ossa{sv}", "ua{sv}", &ScreenshotInterface::dbus_pick_color, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_PROPERTY("AvailableTargets", "u", &ScreenshotInterface::dbus_get_property, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("version", "u", &ScreenshotInterface::dbus_get_property, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_VTABLE_END
};

ScreenshotInterface::ScreenshotInterface(PortalBackend& backend)
    : backend_(backend) {}

ScreenshotInterface::~ScreenshotInterface() = default;

ResponseCode ScreenshotInterface::take_screenshot(
    const ObjectPath& handle,
    const std::string& app_id,
    const std::string& /*parent_window*/,
    const VariantMap& options,
    VariantMap& results) {
    ScreenshotOptions opts;
    opts.modal = get_bool_or(options, "modal", true);
    opts.interactive = get_bool_or(options, "interactive", false);
    opts.target = get_uint32_or(options, "target", 1);
    opts.permission_store_checked = get_bool_or(options, "permission_store_checked", false);
    opts.extra_options = options;

    // The compositor takes screenshots; the backend only relays. Without a
    // host callback there is no image to give, so the answer is an error,
    ScreenshotCallback cb;
    {
        std::lock_guard<std::mutex> lock(cb_mutex_);
        cb = screenshot_callback_;
    }

    if (!cb) {
        return ResponseCode::OtherError;
    }

    std::string uri;
    ResponseCode code = cb(handle, app_id, opts, uri, results);
    if (code == ResponseCode::Success && !results.contains("uri")) {
        if (uri.empty()) return ResponseCode::OtherError;
        results["uri"] = Variant(uri);
    }
    return code;
}

ResponseCode ScreenshotInterface::pick_color(
    const ObjectPath& handle,
    const std::string& app_id,
    const std::string& /*parent_window*/,
    const VariantMap& options,
    VariantMap& results) {
    PickColorCallback cb;
    std::optional<RgbColor> def_col;
    {
        std::lock_guard<std::mutex> lock(cb_mutex_);
        cb = pick_color_callback_;
        def_col = default_color_;
    }

    if (cb) {
        RgbColor color = def_col.value_or(RgbColor{});
        ResponseCode code = cb(handle, app_id, options, color, results);
        if (code == ResponseCode::Success && !results.contains("color")) {
            results["color"] = Variant(color);
        }
        return code;
    }
    if (def_col) {
        // The host preselected the answer (set_default_color).
        results["color"] = Variant(*def_col);
        return ResponseCode::Success;
    }
    // Nobody picked a color.
    return ResponseCode::OtherError;
}

int ScreenshotInterface::dbus_screenshot(sd_bus_message* m, void* userdata, sd_bus_error* /*ret_error*/) {
    auto* self = static_cast<ScreenshotInterface*>(userdata);
    dbus::Message msg(m, false);

    ObjectPath handle;
    std::string app_id;
    std::string parent_window;
    VariantMap options;

    if (!msg.read_object_path(&handle) ||
        !msg.read_string(&app_id) ||
        !msg.read_string(&parent_window) ||
        !msg.read_variant_map(&options)) {
        return sd_bus_reply_method_errorf(m, SD_BUS_ERROR_INVALID_ARGS, "Invalid parameters");
    }

    auto request = self->backend_.create_request(handle, app_id);
    sd_bus_message_ref(m);

    self->backend_.post_worker([self, m, request, handle, app_id, parent_window, options = std::move(options)]() {
        VariantMap results;
        ResponseCode code = self->take_screenshot(handle, app_id, parent_window, options, results);

        self->backend_.send_method_reply_and_unref(m, [code, &results](dbus::Message& reply_msg) {
            reply_msg.append_uint32(static_cast<uint32_t>(code));
            reply_msg.append_variant_map(results);
        });

        if (request) {
            request->close();
        }
    });

    return 1;
}

int ScreenshotInterface::dbus_pick_color(sd_bus_message* m, void* userdata, sd_bus_error* /*ret_error*/) {
    auto* self = static_cast<ScreenshotInterface*>(userdata);
    dbus::Message msg(m, false);

    ObjectPath handle;
    std::string app_id;
    std::string parent_window;
    VariantMap options;

    if (!msg.read_object_path(&handle) ||
        !msg.read_string(&app_id) ||
        !msg.read_string(&parent_window) ||
        !msg.read_variant_map(&options)) {
        return sd_bus_reply_method_errorf(m, SD_BUS_ERROR_INVALID_ARGS, "Invalid parameters");
    }

    auto request = self->backend_.create_request(handle, app_id);
    sd_bus_message_ref(m);

    self->backend_.post_worker([self, m, request, handle, app_id, parent_window, options = std::move(options)]() {
        VariantMap results;
        ResponseCode code = self->pick_color(handle, app_id, parent_window, options, results);

        self->backend_.send_method_reply_and_unref(m, [code, &results](dbus::Message& reply_msg) {
            reply_msg.append_uint32(static_cast<uint32_t>(code));
            reply_msg.append_variant_map(results);
        });

        if (request) {
            request->close();
        }
    });

    return 1;
}

int ScreenshotInterface::dbus_get_property(
    sd_bus* /*bus*/,
    const char* /*path*/,
    const char* /*interface*/,
    const char* property,
    sd_bus_message* reply,
    void* /*userdata*/,
    sd_bus_error* /*ret_error*/) {
    if (std::string(property) == "AvailableTargets") {
        uint32_t targets = 15; // 1 | 2 | 4 | 8 (Screen | Window | Area | ActiveWindow)
        return sd_bus_message_append_basic(reply, 'u', &targets);
    } else if (std::string(property) == "version") {
        uint32_t v = 3;
        return sd_bus_message_append_basic(reply, 'u', &v);
    }
    return -EINVAL;
}

} // namespace broportal
