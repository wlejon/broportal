#include "broportal/screenshot.h"
#include "broportal/backend.h"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>

namespace broportal {

namespace fs = std::filesystem;

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

bool ScreenshotInterface::save_sample_bmp(
    const std::string& filepath,
    int width,
    int height,
    uint8_t r,
    uint8_t g,
    uint8_t b) {
    int row_stride = (width * 3 + 3) & ~3;
    uint32_t image_size = static_cast<uint32_t>(row_stride * height);
    uint32_t file_size = 54 + image_size;

    std::ofstream out(filepath, std::ios::binary);
    if (!out) return false;

    // Bitmap file header (14 bytes)
    uint8_t file_header[14] = {
        'B', 'M',
        static_cast<uint8_t>(file_size & 0xFF),
        static_cast<uint8_t>((file_size >> 8) & 0xFF),
        static_cast<uint8_t>((file_size >> 16) & 0xFF),
        static_cast<uint8_t>((file_size >> 24) & 0xFF),
        0, 0, 0, 0, // reserved
        54, 0, 0, 0 // offset
    };
    out.write(reinterpret_cast<const char*>(file_header), sizeof(file_header));

    // Bitmap info header (40 bytes)
    uint8_t info_header[40] = {
        40, 0, 0, 0, // header size
        static_cast<uint8_t>(width & 0xFF),
        static_cast<uint8_t>((width >> 8) & 0xFF),
        static_cast<uint8_t>((width >> 16) & 0xFF),
        static_cast<uint8_t>((width >> 24) & 0xFF),
        static_cast<uint8_t>(height & 0xFF),
        static_cast<uint8_t>((height >> 8) & 0xFF),
        static_cast<uint8_t>((height >> 16) & 0xFF),
        static_cast<uint8_t>((height >> 24) & 0xFF),
        1, 0, // planes
        24, 0, // bpp
        0, 0, 0, 0, // compression BI_RGB
        static_cast<uint8_t>(image_size & 0xFF),
        static_cast<uint8_t>((image_size >> 8) & 0xFF),
        static_cast<uint8_t>((image_size >> 16) & 0xFF),
        static_cast<uint8_t>((image_size >> 24) & 0xFF),
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
    };
    out.write(reinterpret_cast<const char*>(info_header), sizeof(info_header));

    std::vector<uint8_t> row_buf(row_stride, 0);
    for (int x = 0; x < width; ++x) {
        row_buf[x * 3 + 0] = b;
        row_buf[x * 3 + 1] = g;
        row_buf[x * 3 + 2] = r;
    }

    for (int y = 0; y < height; ++y) {
        out.write(reinterpret_cast<const char*>(row_buf.data()), row_stride);
    }

    return out.good();
}

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

    std::string uri;
    ResponseCode code = ResponseCode::Success;

    if (screenshot_callback_) {
        code = screenshot_callback_(handle, app_id, opts, uri, results);
    } else {
        auto now = std::chrono::steady_clock::now().time_since_epoch().count();
        std::string filename = "broportal-screenshot-" + std::to_string(now) + ".bmp";
        fs::path p = fs::path(screenshot_dir_) / filename;
        if (save_sample_bmp(p.string(), 64, 64, 30, 144, 255)) {
            uri = "file://" + fs::absolute(p).string();
            results["uri"] = Variant(uri);
        } else {
            code = ResponseCode::OtherError;
        }
    }

    if (code == ResponseCode::Success && !results.contains("uri") && !uri.empty()) {
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
    RgbColor color = default_color_;
    ResponseCode code = ResponseCode::Success;

    if (pick_color_callback_) {
        code = pick_color_callback_(handle, app_id, options, color, results);
    } else {
        results["color"] = Variant(color);
    }

    if (code == ResponseCode::Success && !results.contains("color")) {
        results["color"] = Variant(color);
    }

    return code;
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

    VariantMap results;
    ResponseCode code = self->take_screenshot(handle, app_id, parent_window, options, results);

    sd_bus_message* reply = nullptr;
    int r = sd_bus_message_new_method_return(m, &reply);
    if (r < 0) return r;

    dbus::Message reply_msg(reply, true);
    reply_msg.append_uint32(static_cast<uint32_t>(code));
    reply_msg.append_variant_map(results);

    if (request) {
        request->close();
    }

    return sd_bus_send(self->backend_.bus().raw(), reply_msg.raw(), nullptr);
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

    VariantMap results;
    ResponseCode code = self->pick_color(handle, app_id, parent_window, options, results);

    sd_bus_message* reply = nullptr;
    int r = sd_bus_message_new_method_return(m, &reply);
    if (r < 0) return r;

    dbus::Message reply_msg(reply, true);
    reply_msg.append_uint32(static_cast<uint32_t>(code));
    reply_msg.append_variant_map(results);

    if (request) {
        request->close();
    }

    return sd_bus_send(self->backend_.bus().raw(), reply_msg.raw(), nullptr);
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
