#pragma once

#include "broportal/dbus_helpers.h"
#include "broportal/types.h"

#include <functional>
#include <memory>
#include <string>

namespace broportal {

class PortalBackend;

enum class SourceType : uint32_t {
    Monitor = 1,
    Window = 2,
    Virtual = 4
};

enum class CursorMode : uint32_t {
    Hidden = 1,
    Embedded = 2,
    Metadata = 4
};

struct ScreenCastSourceOptions {
    uint32_t types = 1;
    bool multiple = false;
    uint32_t cursor_mode = 1;
    uint32_t persist_mode = 0;
    VariantMap extra_options;
};

using ScreenCastNegotiateCallback = std::function<ResponseCode(
    const ObjectPath& handle,
    const ObjectPath& session_handle,
    const std::string& app_id,
    const ScreenCastSourceOptions& options,
    StreamList& out_streams,
    VariantMap& out_results)>;

class ScreenCastInterface {
public:
    explicit ScreenCastInterface(PortalBackend& backend);
    ~ScreenCastInterface();

    void set_negotiate_callback(ScreenCastNegotiateCallback callback) {
        negotiate_callback_ = std::move(callback);
    }

    ResponseCode create_session(
        const ObjectPath& handle,
        const ObjectPath& session_handle,
        const std::string& app_id,
        const VariantMap& options,
        VariantMap& results);

    ResponseCode select_sources(
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

    // D-Bus method and property handlers
    static int dbus_create_session(sd_bus_message* m, void* userdata, sd_bus_error* ret_error);
    static int dbus_select_sources(sd_bus_message* m, void* userdata, sd_bus_error* ret_error);
    static int dbus_start(sd_bus_message* m, void* userdata, sd_bus_error* ret_error);
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
    ScreenCastNegotiateCallback negotiate_callback_;
};

} // namespace broportal
