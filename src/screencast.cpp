#include "broportal/screencast.h"
#include "broportal/backend.h"
#include "screencast_pipewire.h"

#include <iostream>

namespace broportal {

const sd_bus_vtable ScreenCastInterface::vtable[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_METHOD("CreateSession", "oosa{sv}", "ua{sv}", &ScreenCastInterface::dbus_create_session, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("SelectSources", "oosa{sv}", "ua{sv}", &ScreenCastInterface::dbus_select_sources, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("Start", "oossa{sv}", "ua{sv}", &ScreenCastInterface::dbus_start, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_PROPERTY("AvailableSourceTypes", "u", &ScreenCastInterface::dbus_get_property, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("AvailableCursorModes", "u", &ScreenCastInterface::dbus_get_property, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("version", "u", &ScreenCastInterface::dbus_get_property, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_VTABLE_END
};

ScreenCastInterface::ScreenCastInterface(PortalBackend& backend)
    : backend_(backend) {}

ScreenCastInterface::~ScreenCastInterface() = default;

ResponseCode ScreenCastInterface::create_session(
    const ObjectPath& /*handle*/,
    const ObjectPath& session_handle,
    const std::string& app_id,
    const VariantMap& /*options*/,
    VariantMap& results) {
    auto session = backend_.create_session(session_handle, app_id, SessionType::ScreenCast);
    if (!session) {
        return ResponseCode::OtherError;
    }

    std::string sess_id = "screencast-" + std::to_string(reinterpret_cast<uintptr_t>(session.get()));
    session->set_session_id(sess_id);
    results["session_id"] = Variant(sess_id);

    return ResponseCode::Success;
}

ResponseCode ScreenCastInterface::select_sources(
    const ObjectPath& /*handle*/,
    const ObjectPath& session_handle,
    const std::string& /*app_id*/,
    const VariantMap& options,
    VariantMap& /*results*/) {
    auto session = backend_.get_session(session_handle);
    if (!session) {
        return ResponseCode::OtherError;
    }

    ScreenCastSourceOptions opts;
    opts.types = get_uint32_or(options, "types", 1);
    opts.multiple = get_bool_or(options, "multiple", false);
    opts.cursor_mode = get_uint32_or(options, "cursor_mode", 1);
    opts.persist_mode = get_uint32_or(options, "persist_mode", 0);
    opts.extra_options = options;

    session->set_context("screencast_options", opts);
    return ResponseCode::Success;
}

ResponseCode ScreenCastInterface::start(
    const ObjectPath& handle,
    const ObjectPath& session_handle,
    const std::string& app_id,
    const std::string& /*parent_window*/,
    const VariantMap& /*options*/,
    VariantMap& results) {
    auto session = backend_.get_session(session_handle);
    if (!session) {
        return ResponseCode::OtherError;
    }

    ScreenCastSourceOptions opts;
    if (session->has_context("screencast_options")) {
        opts = std::any_cast<ScreenCastSourceOptions>(session->get_context("screencast_options"));
    }

    StreamList stream_list;
    ResponseCode code = ResponseCode::Success;

    if (negotiate_callback_) {
        code = negotiate_callback_(handle, session_handle, app_id, opts, stream_list, results);
    } else {
        auto node = std::make_shared<PipeWireStreamNode>("broportal-screencast", 1920, 1080);
        if (!node->initialize()) {
            return ResponseCode::OtherError;
        }

        VariantMap stream_props;
        stream_props["position"] = Variant(node->position());
        stream_props["size"] = Variant(node->size());
        stream_props["source_type"] = Variant(static_cast<uint32_t>(SourceType::Monitor));
        stream_props["pipewire-serial"] = Variant(node->serial());

        stream_list.emplace_back(node->node_id(), std::move(stream_props));

        // Keep node alive as long as session is alive
        session->set_context("pipewire_stream_node", node);

        results["streams"] = Variant(stream_list);
        results["persist_mode"] = Variant(opts.persist_mode);
    }

    if (code == ResponseCode::Success && !results.contains("streams")) {
        results["streams"] = Variant(stream_list);
    }

    return code;
}

int ScreenCastInterface::dbus_create_session(sd_bus_message* m, void* userdata, sd_bus_error* /*ret_error*/) {
    auto* self = static_cast<ScreenCastInterface*>(userdata);
    dbus::Message msg(m, false);

    ObjectPath handle;
    ObjectPath session_handle;
    std::string app_id;
    VariantMap options;

    if (!msg.read_object_path(&handle) ||
        !msg.read_object_path(&session_handle) ||
        !msg.read_string(&app_id) ||
        !msg.read_variant_map(&options)) {
        return sd_bus_reply_method_errorf(m, SD_BUS_ERROR_INVALID_ARGS, "Invalid parameters");
    }

    auto request = self->backend_.create_request(handle, app_id);

    VariantMap results;
    ResponseCode code = self->create_session(handle, session_handle, app_id, options, results);

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

int ScreenCastInterface::dbus_select_sources(sd_bus_message* m, void* userdata, sd_bus_error* /*ret_error*/) {
    auto* self = static_cast<ScreenCastInterface*>(userdata);
    dbus::Message msg(m, false);

    ObjectPath handle;
    ObjectPath session_handle;
    std::string app_id;
    VariantMap options;

    if (!msg.read_object_path(&handle) ||
        !msg.read_object_path(&session_handle) ||
        !msg.read_string(&app_id) ||
        !msg.read_variant_map(&options)) {
        return sd_bus_reply_method_errorf(m, SD_BUS_ERROR_INVALID_ARGS, "Invalid parameters");
    }

    auto request = self->backend_.create_request(handle, app_id);

    VariantMap results;
    ResponseCode code = self->select_sources(handle, session_handle, app_id, options, results);

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

int ScreenCastInterface::dbus_start(sd_bus_message* m, void* userdata, sd_bus_error* /*ret_error*/) {
    auto* self = static_cast<ScreenCastInterface*>(userdata);
    dbus::Message msg(m, false);

    ObjectPath handle;
    ObjectPath session_handle;
    std::string app_id;
    std::string parent_window;
    VariantMap options;

    if (!msg.read_object_path(&handle) ||
        !msg.read_object_path(&session_handle) ||
        !msg.read_string(&app_id) ||
        !msg.read_string(&parent_window) ||
        !msg.read_variant_map(&options)) {
        return sd_bus_reply_method_errorf(m, SD_BUS_ERROR_INVALID_ARGS, "Invalid parameters");
    }

    auto request = self->backend_.create_request(handle, app_id);

    VariantMap results;
    ResponseCode code = self->start(handle, session_handle, app_id, parent_window, options, results);

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

int ScreenCastInterface::dbus_get_property(
    sd_bus* /*bus*/,
    const char* /*path*/,
    const char* /*interface*/,
    const char* property,
    sd_bus_message* reply,
    void* /*userdata*/,
    sd_bus_error* /*ret_error*/) {
    if (std::string(property) == "AvailableSourceTypes") {
        uint32_t val = 7; // MONITOR (1) | WINDOW (2) | VIRTUAL (4)
        return sd_bus_message_append_basic(reply, 'u', &val);
    } else if (std::string(property) == "AvailableCursorModes") {
        uint32_t val = 7; // Hidden (1) | Embedded (2) | Metadata (4)
        return sd_bus_message_append_basic(reply, 'u', &val);
    } else if (std::string(property) == "version") {
        uint32_t val = 6;
        return sd_bus_message_append_basic(reply, 'u', &val);
    }
    return -EINVAL;
}

} // namespace broportal
