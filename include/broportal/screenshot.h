#pragma once

#include "broportal/dbus_helpers.h"
#include "broportal/types.h"

#include <functional>
#include <string>

namespace broportal {

class PortalBackend;

enum class ScreenshotTarget : uint32_t {
    Screen = 1,
    Window = 2,
    Area = 4,
    ActiveWindow = 8
};

struct ScreenshotOptions {
    bool modal = true;
    bool interactive = false;
    uint32_t target = 1;
    bool permission_store_checked = false;
    VariantMap extra_options;
};

using ScreenshotCallback = std::function<ResponseCode(
    const ObjectPath& handle,
    const std::string& app_id,
    const ScreenshotOptions& options,
    std::string& out_uri,
    VariantMap& out_results)>;

using PickColorCallback = std::function<ResponseCode(
    const ObjectPath& handle,
    const std::string& app_id,
    const VariantMap& options,
    RgbColor& out_color,
    VariantMap& out_results)>;

class ScreenshotInterface {
public:
    explicit ScreenshotInterface(PortalBackend& backend);
    ~ScreenshotInterface();

    void set_screenshot_callback(ScreenshotCallback callback) {
        screenshot_callback_ = std::move(callback);
    }

    void set_pick_color_callback(PickColorCallback callback) {
        pick_color_callback_ = std::move(callback);
    }

    void set_default_color(const RgbColor& color) {
        default_color_ = color;
    }

    void set_screenshot_dir(const std::string& dir) {
        screenshot_dir_ = dir;
    }

    ResponseCode take_screenshot(
        const ObjectPath& handle,
        const std::string& app_id,
        const std::string& parent_window,
        const VariantMap& options,
        VariantMap& results);

    ResponseCode pick_color(
        const ObjectPath& handle,
        const std::string& app_id,
        const std::string& parent_window,
        const VariantMap& options,
        VariantMap& results);

    // Creates a valid sample test BMP screenshot file on disk
    static bool save_sample_bmp(const std::string& filepath, int width, int height, uint8_t r, uint8_t g, uint8_t b);

    // D-Bus method and property handlers
    static int dbus_screenshot(sd_bus_message* m, void* userdata, sd_bus_error* ret_error);
    static int dbus_pick_color(sd_bus_message* m, void* userdata, sd_bus_error* ret_error);
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
    ScreenshotCallback screenshot_callback_;
    PickColorCallback pick_color_callback_;
    RgbColor default_color_{0.2, 0.4, 0.8};
    std::string screenshot_dir_ = "/tmp";
};

} // namespace broportal
