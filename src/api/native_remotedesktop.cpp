#include "host_portal_internal.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <vector>

namespace broportal::api {

namespace {

struct QueuedRemoteDesktopRequest {
    uint64_t id = 0;
    ObjectPath handle;
    ObjectPath session_handle;
    std::string app_id;
    uint32_t requested_devices = 7;

    std::mutex mu;
    std::condition_variable cv;
    bool answered = false;
    ResponseCode response_code = ResponseCode::OtherError;
    uint32_t out_devices = 0;
    VariantMap out_results;
};

struct QueuedRemoteDesktopInputEvent {
    std::string type;
    ObjectPath session;
    int32_t keycode = 0;
    int32_t keysym = 0;
    int32_t button = 0;
    uint32_t state = 0;
    double dx = 0.0;
    double dy = 0.0;
    bool finish = false;
    uint32_t axis = 0;
    int32_t steps = 0;
    uint32_t stream = 0;
    uint32_t slot = 0;
    double x = 0.0;
    double y = 0.0;
};

struct HandlerEntry {
    uint64_t id = 0;
    std::shared_ptr<ev::Persistent> callback;
};

std::mutex g_rd_mu;
uint64_t g_next_rd_handler_id = 1;
uint64_t g_next_rd_req_id = 1;
std::vector<HandlerEntry> g_rd_handlers;
std::vector<HandlerEntry> g_rd_input_handlers;
std::vector<std::shared_ptr<QueuedRemoteDesktopRequest>> g_active_rd_requests;
std::vector<std::shared_ptr<QueuedRemoteDesktopRequest>> g_unhandled_rd_requests;
std::vector<QueuedRemoteDesktopInputEvent> g_unhandled_rd_input_events;

void fulfillRemoteDesktop(const std::shared_ptr<QueuedRemoteDesktopRequest>& req,
                          ResponseCode code,
                          uint32_t devices,
                          VariantMap results = {}) {
    std::lock_guard lock(req->mu);
    if (req->answered) return;
    req->response_code = code;
    req->out_devices = devices;
    req->out_results = std::move(results);
    req->answered = true;
    req->cv.notify_all();
}

void parseAndFulfillRemoteDesktop(const std::shared_ptr<QueuedRemoteDesktopRequest>& req, Value resVal) {
    ev::Persistent val(resVal);
    if (!ev::isObject(val.get())) return;

    ev::Persistent pCancelled(ev::getProperty(val.get(), "cancelled"));
    if (ev::isBool(pCancelled.get()) && ev::toBool(pCancelled.get())) {
        fulfillRemoteDesktop(req, ResponseCode::Cancelled, 0);
        return;
    }

    uint32_t devices = req->requested_devices;
    ev::Persistent pDevices(ev::getProperty(val.get(), "devices"));
    if (ev::isUndefined(pDevices.get())) pDevices.set(ev::getProperty(val.get(), "deviceTypes"));
    if (ev::isUndefined(pDevices.get())) pDevices.set(ev::getProperty(val.get(), "device_types"));
    if (ev::isUndefined(pDevices.get())) pDevices.set(ev::getProperty(val.get(), "types"));

    if (ev::isNumber(pDevices.get())) {
        devices = static_cast<uint32_t>(ev::toDouble(pDevices.get()));
    } else if (ev::isObject(pDevices.get())) {
        uint32_t mask = 0;
        ev::Persistent pk(ev::getProperty(pDevices.get(), "keyboard"));
        ev::Persistent pp(ev::getProperty(pDevices.get(), "pointer"));
        ev::Persistent pt(ev::getProperty(pDevices.get(), "touchscreen"));
        if (ev::toBool(pk.get())) mask |= static_cast<uint32_t>(DeviceType::Keyboard);
        if (ev::toBool(pp.get())) mask |= static_cast<uint32_t>(DeviceType::Pointer);
        if (ev::toBool(pt.get())) mask |= static_cast<uint32_t>(DeviceType::Touchscreen);
        devices = mask;
    } else {
        ev::Persistent pk(ev::getProperty(val.get(), "keyboard"));
        ev::Persistent pp(ev::getProperty(val.get(), "pointer"));
        ev::Persistent pt(ev::getProperty(val.get(), "touchscreen"));
        if (ev::isBool(pk.get()) || ev::isBool(pp.get()) || ev::isBool(pt.get())) {
            uint32_t mask = 0;
            if (ev::toBool(pk.get())) mask |= static_cast<uint32_t>(DeviceType::Keyboard);
            if (ev::toBool(pp.get())) mask |= static_cast<uint32_t>(DeviceType::Pointer);
            if (ev::toBool(pt.get())) mask |= static_cast<uint32_t>(DeviceType::Touchscreen);
            devices = mask;
        }
    }

    VariantMap results;
    ev::Persistent pClip(ev::getProperty(val.get(), "clipboardEnabled"));
    if (ev::isUndefined(pClip.get())) pClip.set(ev::getProperty(val.get(), "clipboard_enabled"));
    if (ev::isBool(pClip.get())) {
        results["clipboard_enabled"] = Variant(ev::toBool(pClip.get()));
    }

    ev::Persistent pToken(ev::getProperty(val.get(), "restoreToken"));
    if (ev::isUndefined(pToken.get())) pToken.set(ev::getProperty(val.get(), "restore_token"));
    if (ev::isString(pToken.get())) {
        results["restore_token"] = Variant(ev::toUtf8(pToken.get()));
    }

    ev::Persistent pStreams(ev::getProperty(val.get(), "streams"));
    if (ev::isObject(pStreams.get())) {
        ev::Persistent pLen(ev::getProperty(pStreams.get(), "length"));
        if (ev::isNumber(pLen.get())) {
            StreamList stream_list;
            uint32_t len = static_cast<uint32_t>(ev::toDouble(pLen.get()));
            for (uint32_t i = 0; i < len; ++i) {
                ev::Persistent item(ev::getElement(pStreams.get(), i));
                if (ev::isNumber(item.get())) {
                    uint32_t nid = static_cast<uint32_t>(ev::toDouble(item.get()));
                    stream_list.push_back({nid, VariantMap{}});
                } else if (ev::isObject(item.get())) {
                    ev::Persistent pId(ev::getProperty(item.get(), "nodeId"));
                    if (!ev::isNumber(pId.get())) pId.set(ev::getProperty(item.get(), "node_id"));
                    if (ev::isNumber(pId.get())) {
                        uint32_t nid = static_cast<uint32_t>(ev::toDouble(pId.get()));
                        stream_list.push_back({nid, VariantMap{}});
                    }
                }
            }
            results["streams"] = Variant(std::move(stream_list));
        }
    }

    fulfillRemoteDesktop(req, ResponseCode::Success, devices, std::move(results));
}

Value buildRemoteDesktopRequest(const std::shared_ptr<QueuedRemoteDesktopRequest>& req) {
    ObjectBuilder b;
    b.set("handle", req->handle.path);
    b.set("sessionHandle", req->session_handle.path);
    b.set("session_handle", req->session_handle.path);
    b.set("appId", req->app_id);
    b.set("app_id", req->app_id);
    b.set("devices", static_cast<double>(req->requested_devices));
    b.set("deviceTypes", static_cast<double>(req->requested_devices));
    b.set("device_types", static_cast<double>(req->requested_devices));

    bool kb = (req->requested_devices & static_cast<uint32_t>(DeviceType::Keyboard)) != 0;
    bool ptr = (req->requested_devices & static_cast<uint32_t>(DeviceType::Pointer)) != 0;
    bool touch = (req->requested_devices & static_cast<uint32_t>(DeviceType::Touchscreen)) != 0;
    b.set("keyboard", kb);
    b.set("pointer", ptr);
    b.set("touchscreen", touch);

    ObjectBuilder opts;
    opts.set("types", static_cast<double>(req->requested_devices));
    opts.set("devices", static_cast<double>(req->requested_devices));
    opts.set("keyboard", kb);
    opts.set("pointer", ptr);
    opts.set("touchscreen", touch);
    b.set("options", opts.get());

    b.def("respond", 1, [req](Value, std::span<const Value> args) -> Value {
        ev::Persistent p0(args.size() > 0 ? args[0] : ev::undefined());
        parseAndFulfillRemoteDesktop(req, p0.get());
        return ev::fromBool(true);
    });

    b.def("cancel", 0, [req](Value, std::span<const Value>) -> Value {
        fulfillRemoteDesktop(req, ResponseCode::Cancelled, 0);
        return ev::fromBool(true);
    });

    return b.get();
}

Value buildInputEventJs(const QueuedRemoteDesktopInputEvent& evItem) {
    ObjectBuilder b;
    b.set("type", evItem.type);
    b.set("sessionHandle", evItem.session.path);
    b.set("session_handle", evItem.session.path);
    b.set("session", evItem.session.path);

    if (evItem.type == "keyboard_keycode") {
        b.set("keycode", static_cast<double>(evItem.keycode));
        b.set("state", static_cast<double>(evItem.state));
    } else if (evItem.type == "keyboard_keysym") {
        b.set("keysym", static_cast<double>(evItem.keysym));
        b.set("state", static_cast<double>(evItem.state));
    } else if (evItem.type == "pointer_motion") {
        b.set("dx", evItem.dx);
        b.set("dy", evItem.dy);
    } else if (evItem.type == "pointer_motion_absolute") {
        b.set("stream", static_cast<double>(evItem.stream));
        b.set("x", evItem.x);
        b.set("y", evItem.y);
    } else if (evItem.type == "pointer_button") {
        b.set("button", static_cast<double>(evItem.button));
        b.set("state", static_cast<double>(evItem.state));
    } else if (evItem.type == "pointer_axis") {
        b.set("dx", evItem.dx);
        b.set("dy", evItem.dy);
        b.set("finish", evItem.finish);
    } else if (evItem.type == "pointer_axis_discrete") {
        b.set("axis", static_cast<double>(evItem.axis));
        b.set("steps", static_cast<double>(evItem.steps));
    } else if (evItem.type == "touch_down" || evItem.type == "touch_motion") {
        b.set("stream", static_cast<double>(evItem.stream));
        b.set("slot", static_cast<double>(evItem.slot));
        b.set("x", evItem.x);
        b.set("y", evItem.y);
    } else if (evItem.type == "touch_up") {
        b.set("slot", static_cast<double>(evItem.slot));
    }
    return b.get();
}

void pushInputEvent(QueuedRemoteDesktopInputEvent evItem) {
    std::lock_guard lock(g_rd_mu);
    if (g_rd_input_handlers.empty()) return;
    g_unhandled_rd_input_events.push_back(std::move(evItem));
}

RemoteDesktopInputListener makeNativeInputListener() {
    RemoteDesktopInputListener l;
    l.on_keyboard_keycode = [](const ObjectPath& s, int32_t kc, uint32_t st) {
        QueuedRemoteDesktopInputEvent e;
        e.type = "keyboard_keycode";
        e.session = s;
        e.keycode = kc;
        e.state = st;
        pushInputEvent(std::move(e));
    };
    l.on_keyboard_keysym = [](const ObjectPath& s, int32_t ks, uint32_t st) {
        QueuedRemoteDesktopInputEvent e;
        e.type = "keyboard_keysym";
        e.session = s;
        e.keysym = ks;
        e.state = st;
        pushInputEvent(std::move(e));
    };
    l.on_pointer_motion = [](const ObjectPath& s, double dx, double dy) {
        QueuedRemoteDesktopInputEvent e;
        e.type = "pointer_motion";
        e.session = s;
        e.dx = dx;
        e.dy = dy;
        pushInputEvent(std::move(e));
    };
    l.on_pointer_motion_absolute = [](const ObjectPath& s, uint32_t stream, double x, double y) {
        QueuedRemoteDesktopInputEvent e;
        e.type = "pointer_motion_absolute";
        e.session = s;
        e.stream = stream;
        e.x = x;
        e.y = y;
        pushInputEvent(std::move(e));
    };
    l.on_pointer_button = [](const ObjectPath& s, int32_t btn, uint32_t st) {
        QueuedRemoteDesktopInputEvent e;
        e.type = "pointer_button";
        e.session = s;
        e.button = btn;
        e.state = st;
        pushInputEvent(std::move(e));
    };
    l.on_pointer_axis = [](const ObjectPath& s, double dx, double dy, bool finish) {
        QueuedRemoteDesktopInputEvent e;
        e.type = "pointer_axis";
        e.session = s;
        e.dx = dx;
        e.dy = dy;
        e.finish = finish;
        pushInputEvent(std::move(e));
    };
    l.on_pointer_axis_discrete = [](const ObjectPath& s, uint32_t axis, int32_t steps) {
        QueuedRemoteDesktopInputEvent e;
        e.type = "pointer_axis_discrete";
        e.session = s;
        e.axis = axis;
        e.steps = steps;
        pushInputEvent(std::move(e));
    };
    l.on_touch_down = [](const ObjectPath& s, uint32_t stream, uint32_t slot, double x, double y) {
        QueuedRemoteDesktopInputEvent e;
        e.type = "touch_down";
        e.session = s;
        e.stream = stream;
        e.slot = slot;
        e.x = x;
        e.y = y;
        pushInputEvent(std::move(e));
    };
    l.on_touch_motion = [](const ObjectPath& s, uint32_t stream, uint32_t slot, double x, double y) {
        QueuedRemoteDesktopInputEvent e;
        e.type = "touch_motion";
        e.session = s;
        e.stream = stream;
        e.slot = slot;
        e.x = x;
        e.y = y;
        pushInputEvent(std::move(e));
    };
    l.on_touch_up = [](const ObjectPath& s, uint32_t slot) {
        QueuedRemoteDesktopInputEvent e;
        e.type = "touch_up";
        e.session = s;
        e.slot = slot;
        pushInputEvent(std::move(e));
    };
    return l;
}

} // namespace

ResponseCode handleNativeRemoteDesktopStart(
    const ObjectPath& handle,
    const ObjectPath& session_handle,
    const std::string& app_id,
    uint32_t& devices,
    VariantMap& out_results) {
    auto req = std::make_shared<QueuedRemoteDesktopRequest>();
    req->handle = handle;
    req->session_handle = session_handle;
    req->app_id = app_id;
    req->requested_devices = devices;
    req->out_devices = devices;

    {
        std::lock_guard lock(g_rd_mu);
        if (g_rd_handlers.empty()) {
            return ResponseCode::OtherError;
        }
        req->id = ++g_next_rd_req_id;
        g_active_rd_requests.push_back(req);
        g_unhandled_rd_requests.push_back(req);
    }

    std::unique_lock ulock(req->mu);
    bool ok = req->cv.wait_for(ulock, std::chrono::seconds(60), [&]() {
        return req->answered;
    });

    {
        std::lock_guard lock(g_rd_mu);
        for (auto it = g_active_rd_requests.begin(); it != g_active_rd_requests.end(); ++it) {
            if ((*it)->id == req->id) {
                g_active_rd_requests.erase(it);
                break;
            }
        }
    }

    if (!ok || !req->answered) {
        return ResponseCode::OtherError;
    }

    devices = req->out_devices;
    out_results = std::move(req->out_results);
    return req->response_code;
}

void attachRemoteDesktopBackend(broportal::PortalBackend& backend) {
    backend.remote_desktop().set_start_callback(handleNativeRemoteDesktopStart);
    backend.remote_desktop().set_input_listener(makeNativeInputListener());
}

void drainRemoteDesktop() {
    // 1. Start requests
    {
        std::vector<std::shared_ptr<QueuedRemoteDesktopRequest>> toDispatch;
        std::vector<std::shared_ptr<ev::Persistent>> callbacks;
        {
            std::lock_guard lock(g_rd_mu);
            toDispatch.swap(g_unhandled_rd_requests);
            for (const auto& entry : g_rd_handlers) {
                if (entry.callback) callbacks.push_back(entry.callback);
            }
        }

        if (!toDispatch.empty()) {
            if (callbacks.empty()) {
                for (const auto& req : toDispatch) {
                    fulfillRemoteDesktop(req, ResponseCode::OtherError, 0);
                }
            } else {
                for (const auto& req : toDispatch) {
                    ev::Persistent reqObj(buildRemoteDesktopRequest(req));
                    bool hasPendingPromise = false;
                    for (const auto& cb : callbacks) {
                        if (!cb || !ev::isFunction(cb->get())) continue;
                        const Value arg = reqObj.get();
                        auto callRes = ev::call(cb->get(), ev::undefined(), std::span<const Value>(&arg, 1));
                        if (callRes.thrown) continue;
                        ev::Persistent retVal(callRes.value);

                        if (ev::isPromise(retVal.get())) {
                            hasPendingPromise = true;
                            attachPromiseReaction(
                                retVal.get(),
                                [req](Value val) {
                                    parseAndFulfillRemoteDesktop(req, val);
                                    fulfillRemoteDesktop(req, ResponseCode::OtherError, 0);
                                },
                                [req](Value) { fulfillRemoteDesktop(req, ResponseCode::Cancelled, 0); });
                        } else if (ev::isObject(retVal.get())) {
                            parseAndFulfillRemoteDesktop(req, retVal.get());
                        }
                    }
                    if (!hasPendingPromise) {
                        fulfillRemoteDesktop(req, ResponseCode::OtherError, 0);
                    }
                }
            }
        }
    }

    // 2. Input events
    {
        std::vector<QueuedRemoteDesktopInputEvent> toDispatch;
        std::vector<std::shared_ptr<ev::Persistent>> callbacks;
        {
            std::lock_guard lock(g_rd_mu);
            toDispatch.swap(g_unhandled_rd_input_events);
            for (const auto& entry : g_rd_input_handlers) {
                if (entry.callback) callbacks.push_back(entry.callback);
            }
        }

        if (!toDispatch.empty() && !callbacks.empty()) {
            for (const auto& item : toDispatch) {
                ev::Persistent evObj(buildInputEventJs(item));
                for (const auto& cb : callbacks) {
                    if (!cb || !ev::isFunction(cb->get())) continue;
                    const Value arg = evObj.get();
                    ev::call(cb->get(), ev::undefined(), std::span<const Value>(&arg, 1));
                }
            }
        }
    }
}

void shutdownRemoteDesktop() {
    std::vector<std::shared_ptr<QueuedRemoteDesktopRequest>> toCancel;
    {
        std::lock_guard lock(g_rd_mu);
        toCancel.swap(g_active_rd_requests);
        g_unhandled_rd_requests.clear();
        g_unhandled_rd_input_events.clear();
        g_rd_handlers.clear();
        g_rd_input_handlers.clear();
    }

    for (const auto& req : toCancel) {
        std::lock_guard lock(req->mu);
        req->response_code = ResponseCode::Cancelled;
        req->answered = true;
        req->cv.notify_all();
    }
}

void installRemoteDesktopOnto(Value portalObj) {
    ObjectBuilder portal(portalObj);

    // bro.portal.onRemoteDesktop(handler) -> HandlerHandle
    auto onStartFn = [](Value, std::span<const Value> args) -> Value {
        ev::Persistent p0(args.size() > 0 ? args[0] : ev::undefined());
        if (!ev::isFunction(p0.get())) {
            return ev::throwTypeError("onRemoteDesktop: handler must be a function");
        }

        uint64_t hid = 0;
        {
            std::lock_guard lock(g_rd_mu);
            hid = ++g_next_rd_handler_id;
            g_rd_handlers.push_back(HandlerEntry{
                .id = hid,
                .callback = std::make_shared<ev::Persistent>(p0.get())
            });
        }

        auto b = activeBackend();
        if (b) {
            attachRemoteDesktopBackend(*b);
        }

        ObjectBuilder handle;
        handle.set("id", static_cast<double>(hid));
        auto removeFn = [hid](Value, std::span<const Value>) -> Value {
            std::lock_guard lock(g_rd_mu);
            g_rd_handlers.erase(
                std::remove_if(g_rd_handlers.begin(), g_rd_handlers.end(),
                               [hid](const HandlerEntry& e) { return e.id == hid; }),
                g_rd_handlers.end());
            return ev::fromBool(true);
        };
        handle.def("dispose", 0, removeFn);
        handle.def("unregister", 0, removeFn);
        return handle.get();
    };
    portal.def("onRemoteDesktop", 1, onStartFn);
    portal.def("onRemoteDesktopStart", 1, onStartFn);

    // bro.portal.offRemoteDesktop(handlerOrHandle) -> boolean
    auto offStartFn = [](Value, std::span<const Value> args) -> Value {
        ev::Persistent p0(args.size() > 0 ? args[0] : ev::undefined());
        std::lock_guard lock(g_rd_mu);
        if (ev::isFunction(p0.get())) {
            g_rd_handlers.erase(
                std::remove_if(g_rd_handlers.begin(), g_rd_handlers.end(),
                               [&](const HandlerEntry& e) {
                                   return e.callback && e.callback->get() == p0.get();
                               }),
                g_rd_handlers.end());
            return ev::fromBool(true);
        }
        if (ev::isObject(p0.get())) {
            ev::Persistent pId(ev::getProperty(p0.get(), "id"));
            if (ev::isNumber(pId.get())) {
                uint64_t hid = static_cast<uint64_t>(ev::toDouble(pId.get()));
                g_rd_handlers.erase(
                    std::remove_if(g_rd_handlers.begin(), g_rd_handlers.end(),
                                   [hid](const HandlerEntry& e) { return e.id == hid; }),
                    g_rd_handlers.end());
                return ev::fromBool(true);
            }
        }
        return ev::fromBool(false);
    };
    portal.def("offRemoteDesktop", 1, offStartFn);
    portal.def("offRemoteDesktopStart", 1, offStartFn);

    // bro.portal.onRemoteDesktopInput(handler) -> HandlerHandle
    portal.def("onRemoteDesktopInput", 1, [](Value, std::span<const Value> args) -> Value {
        ev::Persistent p0(args.size() > 0 ? args[0] : ev::undefined());
        if (!ev::isFunction(p0.get())) {
            return ev::throwTypeError("onRemoteDesktopInput: handler must be a function");
        }

        uint64_t hid = 0;
        {
            std::lock_guard lock(g_rd_mu);
            hid = ++g_next_rd_handler_id;
            g_rd_input_handlers.push_back(HandlerEntry{
                .id = hid,
                .callback = std::make_shared<ev::Persistent>(p0.get())
            });
        }

        auto b = activeBackend();
        if (b) {
            attachRemoteDesktopBackend(*b);
        }

        ObjectBuilder handle;
        handle.set("id", static_cast<double>(hid));
        auto removeFn = [hid](Value, std::span<const Value>) -> Value {
            std::lock_guard lock(g_rd_mu);
            g_rd_input_handlers.erase(
                std::remove_if(g_rd_input_handlers.begin(), g_rd_input_handlers.end(),
                               [hid](const HandlerEntry& e) { return e.id == hid; }),
                g_rd_input_handlers.end());
            return ev::fromBool(true);
        };
        handle.def("dispose", 0, removeFn);
        handle.def("unregister", 0, removeFn);
        return handle.get();
    });

    // bro.portal.offRemoteDesktopInput(handlerOrHandle) -> boolean
    portal.def("offRemoteDesktopInput", 1, [](Value, std::span<const Value> args) -> Value {
        ev::Persistent p0(args.size() > 0 ? args[0] : ev::undefined());
        std::lock_guard lock(g_rd_mu);
        if (ev::isFunction(p0.get())) {
            g_rd_input_handlers.erase(
                std::remove_if(g_rd_input_handlers.begin(), g_rd_input_handlers.end(),
                               [&](const HandlerEntry& e) {
                                   return e.callback && e.callback->get() == p0.get();
                               }),
                g_rd_input_handlers.end());
            return ev::fromBool(true);
        }
        if (ev::isObject(p0.get())) {
            ev::Persistent pId(ev::getProperty(p0.get(), "id"));
            if (ev::isNumber(pId.get())) {
                uint64_t hid = static_cast<uint64_t>(ev::toDouble(pId.get()));
                g_rd_input_handlers.erase(
                    std::remove_if(g_rd_input_handlers.begin(), g_rd_input_handlers.end(),
                                   [hid](const HandlerEntry& e) { return e.id == hid; }),
                    g_rd_input_handlers.end());
                return ev::fromBool(true);
            }
        }
        return ev::fromBool(false);
    });
}

} // namespace broportal::api
