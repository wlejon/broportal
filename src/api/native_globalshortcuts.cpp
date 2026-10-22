#include "host_portal_internal.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <vector>

namespace broportal::api {

namespace {

struct QueuedBindShortcutsRequest {
    uint64_t id = 0;
    ObjectPath handle;
    ObjectPath session_handle;
    std::string app_id;
    ShortcutList shortcuts;

    std::mutex mu;
    std::condition_variable cv;
    bool answered = false;
    ResponseCode response_code = ResponseCode::OtherError;
    ShortcutList out_bound_shortcuts;
    VariantMap out_results;
};

struct QueuedListShortcutsRequest {
    uint64_t id = 0;
    ObjectPath handle;
    ObjectPath session_handle;
    std::string app_id;
    ShortcutList current_shortcuts;

    std::mutex mu;
    std::condition_variable cv;
    bool answered = false;
    ResponseCode response_code = ResponseCode::OtherError;
    ShortcutList out_shortcuts;
    VariantMap out_results;
};

struct QueuedConfigureShortcutsEvent {
    ObjectPath session_handle;
    std::string parent_window;
    VariantMap options;
};

struct HandlerEntry {
    uint64_t id = 0;
    std::shared_ptr<ev::Persistent> callback;
};

std::mutex g_gs_mu;
uint64_t g_next_gs_handler_id = 1;
uint64_t g_next_gs_req_id = 1;
std::string g_last_gs_session_path;
std::vector<HandlerEntry> g_bind_handlers;
std::vector<HandlerEntry> g_list_handlers;
std::vector<HandlerEntry> g_cfg_handlers;
std::vector<std::shared_ptr<QueuedBindShortcutsRequest>> g_active_bind_requests;
std::vector<std::shared_ptr<QueuedBindShortcutsRequest>> g_unhandled_bind_requests;
std::vector<std::shared_ptr<QueuedListShortcutsRequest>> g_active_list_requests;
std::vector<std::shared_ptr<QueuedListShortcutsRequest>> g_unhandled_list_requests;
std::vector<QueuedConfigureShortcutsEvent> g_unhandled_cfg_events;

Value shortcutListToJs(const ShortcutList& shortcuts) {
    ev::Persistent arr(ev::makeArray(static_cast<uint32_t>(shortcuts.size())));
    for (uint32_t i = 0; i < shortcuts.size(); ++i) {
        const auto& [id, props] = shortcuts[i];
        ObjectBuilder item;
        item.set("id", id);
        item.set("shortcutId", id);
        item.set("shortcut_id", id);
        std::string desc = get_string(props, "description").value_or("");
        std::string pref = get_string(props, "preferred_trigger").value_or("");
        std::string trig = get_string(props, "trigger_description").value_or("");
        item.set("description", desc);
        item.set("preferredTrigger", pref);
        item.set("preferred_trigger", pref);
        item.set("triggerDescription", trig);
        item.set("trigger_description", trig);
        item.set("properties", variantMapToJs(props));
        ev::Persistent elem(item.get());
        arr.set(ev::setElement(arr.get(), i, elem.get()));
    }
    return arr.get();
}

ShortcutList parseShortcutListFromJs(Value arrVal, const ShortcutList& fallback_requested = {}) {
    ShortcutList out;
    ev::Persistent arr(arrVal);
    if (!ev::isObject(arr.get())) return out;
    ev::Persistent pLen(ev::getProperty(arr.get(), "length"));
    if (!ev::isNumber(pLen.get())) return out;

    uint32_t len = static_cast<uint32_t>(ev::toDouble(pLen.get()));
    for (uint32_t i = 0; i < len; ++i) {
        ev::Persistent item(ev::getElement(arr.get(), i));
        if (!ev::isObject(item.get())) continue;

        ev::Persistent pId(ev::getProperty(item.get(), "id"));
        if (!ev::isString(pId.get())) pId.set(ev::getProperty(item.get(), "shortcutId"));
        if (!ev::isString(pId.get())) pId.set(ev::getProperty(item.get(), "shortcut_id"));
        if (!ev::isString(pId.get())) continue;

        std::string id = ev::toUtf8(pId.get());
        VariantMap props;

        // Inherit description from requested shortcut if not overridden
        for (const auto& [req_id, req_props] : fallback_requested) {
            if (req_id == id) {
                if (auto d = get_string(req_props, "description")) {
                    props["description"] = Variant(*d);
                }
                break;
            }
        }

        ev::Persistent pProps(ev::getProperty(item.get(), "properties"));
        if (ev::isObject(pProps.get())) {
            ev::Persistent pDescP(ev::getProperty(pProps.get(), "description"));
            if (ev::isString(pDescP.get())) {
                props["description"] = Variant(ev::toUtf8(pDescP.get()));
            }
            ev::Persistent pTrigP(ev::getProperty(pProps.get(), "trigger_description"));
            if (!ev::isString(pTrigP.get())) pTrigP.set(ev::getProperty(pProps.get(), "triggerDescription"));
            if (ev::isString(pTrigP.get())) {
                props["trigger_description"] = Variant(ev::toUtf8(pTrigP.get()));
            }
        }

        ev::Persistent pDesc(ev::getProperty(item.get(), "description"));
        if (ev::isString(pDesc.get())) {
            props["description"] = Variant(ev::toUtf8(pDesc.get()));
        }

        ev::Persistent pTrig(ev::getProperty(item.get(), "triggerDescription"));
        if (!ev::isString(pTrig.get())) pTrig.set(ev::getProperty(item.get(), "trigger_description"));
        if (!ev::isString(pTrig.get())) pTrig.set(ev::getProperty(item.get(), "trigger"));
        if (!ev::isString(pTrig.get())) pTrig.set(ev::getProperty(item.get(), "preferredTrigger"));
        if (!ev::isString(pTrig.get())) pTrig.set(ev::getProperty(item.get(), "preferred_trigger"));
        if (ev::isString(pTrig.get())) {
            props["trigger_description"] = Variant(ev::toUtf8(pTrig.get()));
        }

        out.emplace_back(id, std::move(props));
    }
    return out;
}

void fulfillBindShortcuts(const std::shared_ptr<QueuedBindShortcutsRequest>& req,
                          ResponseCode code,
                          ShortcutList bound,
                          VariantMap results = {}) {
    std::lock_guard lock(req->mu);
    if (req->answered) return;
    req->response_code = code;
    req->out_bound_shortcuts = std::move(bound);
    req->out_results = std::move(results);
    req->answered = true;
    req->cv.notify_all();
}

void parseAndFulfillBindShortcuts(const std::shared_ptr<QueuedBindShortcutsRequest>& req, Value resVal) {
    ev::Persistent val(resVal);
    if (!ev::isObject(val.get())) return;

    ev::Persistent pCancelled(ev::getProperty(val.get(), "cancelled"));
    if (ev::isBool(pCancelled.get()) && ev::toBool(pCancelled.get())) {
        fulfillBindShortcuts(req, ResponseCode::Cancelled, {});
        return;
    }

    ev::Persistent pArr(val.get());
    ev::Persistent pLen(ev::getProperty(val.get(), "length"));
    if (!ev::isNumber(pLen.get())) {
        pArr.set(ev::getProperty(val.get(), "shortcuts"));
        if (!ev::isObject(pArr.get())) pArr.set(ev::getProperty(val.get(), "boundShortcuts"));
        if (!ev::isObject(pArr.get())) pArr.set(ev::getProperty(val.get(), "bound_shortcuts"));
    }

    ShortcutList bound;
    if (ev::isObject(pArr.get())) {
        bound = parseShortcutListFromJs(pArr.get(), req->shortcuts);
    } else {
        for (const auto& [id, props] : req->shortcuts) {
            VariantMap p = props;
            if (!p.contains("trigger_description")) {
                if (auto pref = get_string(props, "preferred_trigger")) {
                    p["trigger_description"] = Variant(*pref);
                }
            }
            bound.emplace_back(id, std::move(p));
        }
    }

    fulfillBindShortcuts(req, ResponseCode::Success, std::move(bound));
}

Value buildBindShortcutsRequest(const std::shared_ptr<QueuedBindShortcutsRequest>& req) {
    ObjectBuilder b;
    b.set("handle", req->handle.path);
    b.set("sessionHandle", req->session_handle.path);
    b.set("session_handle", req->session_handle.path);
    b.set("appId", req->app_id);
    b.set("app_id", req->app_id);
    b.set("shortcuts", shortcutListToJs(req->shortcuts));

    b.def("respond", 1, [req](Value, std::span<const Value> args) -> Value {
        ev::Persistent p0(args.size() > 0 ? args[0] : ev::undefined());
        parseAndFulfillBindShortcuts(req, p0.get());
        return ev::fromBool(true);
    });

    b.def("cancel", 0, [req](Value, std::span<const Value>) -> Value {
        fulfillBindShortcuts(req, ResponseCode::Cancelled, {});
        return ev::fromBool(true);
    });

    return b.get();
}

void fulfillListShortcuts(const std::shared_ptr<QueuedListShortcutsRequest>& req,
                          ResponseCode code,
                          ShortcutList shortcuts,
                          VariantMap results = {}) {
    std::lock_guard lock(req->mu);
    if (req->answered) return;
    req->response_code = code;
    req->out_shortcuts = std::move(shortcuts);
    req->out_results = std::move(results);
    req->answered = true;
    req->cv.notify_all();
}

void parseAndFulfillListShortcuts(const std::shared_ptr<QueuedListShortcutsRequest>& req, Value resVal) {
    ev::Persistent val(resVal);
    if (!ev::isObject(val.get())) return;

    ev::Persistent pCancelled(ev::getProperty(val.get(), "cancelled"));
    if (ev::isBool(pCancelled.get()) && ev::toBool(pCancelled.get())) {
        fulfillListShortcuts(req, ResponseCode::Cancelled, {});
        return;
    }

    ev::Persistent pArr(val.get());
    ev::Persistent pLen(ev::getProperty(val.get(), "length"));
    if (!ev::isNumber(pLen.get())) {
        pArr.set(ev::getProperty(val.get(), "shortcuts"));
    }

    ShortcutList list = req->current_shortcuts;
    if (ev::isObject(pArr.get())) {
        list = parseShortcutListFromJs(pArr.get(), req->current_shortcuts);
    }

    fulfillListShortcuts(req, ResponseCode::Success, std::move(list));
}

Value buildListShortcutsRequest(const std::shared_ptr<QueuedListShortcutsRequest>& req) {
    ObjectBuilder b;
    b.set("handle", req->handle.path);
    b.set("sessionHandle", req->session_handle.path);
    b.set("session_handle", req->session_handle.path);
    b.set("appId", req->app_id);
    b.set("app_id", req->app_id);
    b.set("shortcuts", shortcutListToJs(req->current_shortcuts));

    b.def("respond", 1, [req](Value, std::span<const Value> args) -> Value {
        ev::Persistent p0(args.size() > 0 ? args[0] : ev::undefined());
        parseAndFulfillListShortcuts(req, p0.get());
        return ev::fromBool(true);
    });

    b.def("cancel", 0, [req](Value, std::span<const Value>) -> Value {
        fulfillListShortcuts(req, ResponseCode::Cancelled, {});
        return ev::fromBool(true);
    });

    return b.get();
}

bool parseTriggerArgs(std::span<const Value> args,
                      std::string* out_session,
                      std::string* out_shortcut_id,
                      uint64_t* out_timestamp) {
    *out_timestamp = 0;
    if (args.empty()) return false;

    ev::Persistent p0(args[0]);
    if (ev::isObject(p0.get())) {
        ev::Persistent pSess(ev::getProperty(p0.get(), "sessionHandle"));
        if (!ev::isString(pSess.get())) pSess.set(ev::getProperty(p0.get(), "session_handle"));
        if (!ev::isString(pSess.get())) pSess.set(ev::getProperty(p0.get(), "session"));
        if (ev::isString(pSess.get())) {
            *out_session = ev::toUtf8(pSess.get());
        }

        ev::Persistent pId(ev::getProperty(p0.get(), "shortcutId"));
        if (!ev::isString(pId.get())) pId.set(ev::getProperty(p0.get(), "shortcut_id"));
        if (!ev::isString(pId.get())) pId.set(ev::getProperty(p0.get(), "id"));
        if (ev::isString(pId.get())) {
            *out_shortcut_id = ev::toUtf8(pId.get());
        }

        ev::Persistent pTs(ev::getProperty(p0.get(), "timestamp"));
        if (ev::isNumber(pTs.get())) {
            *out_timestamp = static_cast<uint64_t>(ev::toDouble(pTs.get()));
        }
    } else if (args.size() == 1 && ev::isString(p0.get())) {
        *out_shortcut_id = ev::toUtf8(p0.get());
    } else if (args.size() >= 2 && ev::isString(p0.get())) {
        ev::Persistent p1(args[1]);
        if (ev::isString(p1.get())) {
            *out_session = ev::toUtf8(p0.get());
            *out_shortcut_id = ev::toUtf8(p1.get());
            if (args.size() >= 3 && ev::isNumber(args[2])) {
                *out_timestamp = static_cast<uint64_t>(ev::toDouble(args[2]));
            }
        } else if (ev::isNumber(p1.get())) {
            *out_shortcut_id = ev::toUtf8(p0.get());
            *out_timestamp = static_cast<uint64_t>(ev::toDouble(p1.get()));
        }
    }

    if (out_session->empty()) {
        std::lock_guard lock(g_gs_mu);
        *out_session = g_last_gs_session_path;
    }
    return !out_session->empty() && !out_shortcut_id->empty();
}

} // namespace

ResponseCode handleNativeBindShortcuts(
    const ObjectPath& handle,
    const ObjectPath& session_handle,
    const std::string& app_id,
    const ShortcutList& shortcuts,
    ShortcutList& out_bound_shortcuts,
    VariantMap& out_results) {
    auto req = std::make_shared<QueuedBindShortcutsRequest>();
    req->handle = handle;
    req->session_handle = session_handle;
    req->app_id = app_id;
    req->shortcuts = shortcuts;

    {
        std::lock_guard lock(g_gs_mu);
        g_last_gs_session_path = session_handle.path;
        if (g_bind_handlers.empty()) {
            return ResponseCode::OtherError;
        }
        req->id = ++g_next_gs_req_id;
        g_active_bind_requests.push_back(req);
        g_unhandled_bind_requests.push_back(req);
    }

    std::unique_lock ulock(req->mu);
    bool ok = req->cv.wait_for(ulock, std::chrono::seconds(60), [&]() {
        return req->answered;
    });

    {
        std::lock_guard lock(g_gs_mu);
        for (auto it = g_active_bind_requests.begin(); it != g_active_bind_requests.end(); ++it) {
            if ((*it)->id == req->id) {
                g_active_bind_requests.erase(it);
                break;
            }
        }
    }

    if (!ok || !req->answered) {
        return ResponseCode::OtherError;
    }

    out_bound_shortcuts = std::move(req->out_bound_shortcuts);
    out_results = std::move(req->out_results);
    return req->response_code;
}

ResponseCode handleNativeListShortcuts(
    const ObjectPath& handle,
    const ObjectPath& session_handle,
    const std::string& app_id,
    ShortcutList& out_shortcuts,
    VariantMap& out_results) {
    auto req = std::make_shared<QueuedListShortcutsRequest>();
    req->handle = handle;
    req->session_handle = session_handle;
    req->app_id = app_id;
    req->current_shortcuts = out_shortcuts;

    {
        std::lock_guard lock(g_gs_mu);
        g_last_gs_session_path = session_handle.path;
        if (g_list_handlers.empty()) {
            return ResponseCode::Success;
        }
        req->id = ++g_next_gs_req_id;
        g_active_list_requests.push_back(req);
        g_unhandled_list_requests.push_back(req);
    }

    std::unique_lock ulock(req->mu);
    bool ok = req->cv.wait_for(ulock, std::chrono::seconds(60), [&]() {
        return req->answered;
    });

    {
        std::lock_guard lock(g_gs_mu);
        for (auto it = g_active_list_requests.begin(); it != g_active_list_requests.end(); ++it) {
            if ((*it)->id == req->id) {
                g_active_list_requests.erase(it);
                break;
            }
        }
    }

    if (!ok || !req->answered) {
        return ResponseCode::OtherError;
    }

    out_shortcuts = std::move(req->out_shortcuts);
    out_results = std::move(req->out_results);
    return req->response_code;
}

void handleNativeConfigureShortcuts(
    const ObjectPath& session_handle,
    const std::string& parent_window,
    const VariantMap& options) {
    std::lock_guard lock(g_gs_mu);
    g_last_gs_session_path = session_handle.path;
    if (g_cfg_handlers.empty()) return;
    g_unhandled_cfg_events.push_back(QueuedConfigureShortcutsEvent{session_handle, parent_window, options});
}

void attachGlobalShortcutsBackend(broportal::PortalBackend& backend) {
    backend.global_shortcuts().set_async_dispatch(true);
    backend.global_shortcuts().set_bind_shortcuts_callback(handleNativeBindShortcuts);
    bool has_list = false;
    bool has_cfg = false;
    {
        std::lock_guard lock(g_gs_mu);
        has_list = !g_list_handlers.empty();
        has_cfg = !g_cfg_handlers.empty();
    }
    if (has_list) {
        backend.global_shortcuts().set_list_shortcuts_callback(handleNativeListShortcuts);
    }
    if (has_cfg) {
        backend.global_shortcuts().set_configure_callback(handleNativeConfigureShortcuts);
    }
}

void drainGlobalShortcuts() {
    // 1. BindShortcuts requests
    {
        std::vector<std::shared_ptr<QueuedBindShortcutsRequest>> toDispatch;
        std::vector<std::shared_ptr<ev::Persistent>> callbacks;
        {
            std::lock_guard lock(g_gs_mu);
            toDispatch.swap(g_unhandled_bind_requests);
            for (const auto& entry : g_bind_handlers) {
                if (entry.callback) callbacks.push_back(entry.callback);
            }
        }
        if (!toDispatch.empty()) {
            if (callbacks.empty()) {
                for (const auto& req : toDispatch) {
                    fulfillBindShortcuts(req, ResponseCode::OtherError, {});
                }
            } else {
                for (const auto& req : toDispatch) {
                    ev::Persistent reqObj(buildBindShortcutsRequest(req));
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
                                    parseAndFulfillBindShortcuts(req, val);
                                    fulfillBindShortcuts(req, ResponseCode::OtherError, {});
                                },
                                [req](Value) { fulfillBindShortcuts(req, ResponseCode::Cancelled, {}); });
                        } else if (ev::isObject(retVal.get())) {
                            parseAndFulfillBindShortcuts(req, retVal.get());
                        }
                    }
                    if (!hasPendingPromise) {
                        fulfillBindShortcuts(req, ResponseCode::OtherError, {});
                    }
                }
            }
        }
    }

    // 2. ListShortcuts requests
    {
        std::vector<std::shared_ptr<QueuedListShortcutsRequest>> toDispatch;
        std::vector<std::shared_ptr<ev::Persistent>> callbacks;
        {
            std::lock_guard lock(g_gs_mu);
            toDispatch.swap(g_unhandled_list_requests);
            for (const auto& entry : g_list_handlers) {
                if (entry.callback) callbacks.push_back(entry.callback);
            }
        }
        if (!toDispatch.empty()) {
            if (callbacks.empty()) {
                for (const auto& req : toDispatch) {
                    fulfillListShortcuts(req, ResponseCode::OtherError, {});
                }
            } else {
                for (const auto& req : toDispatch) {
                    ev::Persistent reqObj(buildListShortcutsRequest(req));
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
                                    parseAndFulfillListShortcuts(req, val);
                                    fulfillListShortcuts(req, ResponseCode::OtherError, {});
                                },
                                [req](Value) { fulfillListShortcuts(req, ResponseCode::Cancelled, {}); });
                        } else if (ev::isObject(retVal.get())) {
                            parseAndFulfillListShortcuts(req, retVal.get());
                        }
                    }
                    if (!hasPendingPromise) {
                        fulfillListShortcuts(req, ResponseCode::OtherError, {});
                    }
                }
            }
        }
    }

    // 3. ConfigureShortcuts events
    {
        std::vector<QueuedConfigureShortcutsEvent> toDispatch;
        std::vector<std::shared_ptr<ev::Persistent>> callbacks;
        {
            std::lock_guard lock(g_gs_mu);
            toDispatch.swap(g_unhandled_cfg_events);
            for (const auto& entry : g_cfg_handlers) {
                if (entry.callback) callbacks.push_back(entry.callback);
            }
        }
        if (!toDispatch.empty() && !callbacks.empty()) {
            for (const auto& item : toDispatch) {
                ObjectBuilder b;
                b.set("sessionHandle", item.session_handle.path);
                b.set("session_handle", item.session_handle.path);
                b.set("parentWindow", item.parent_window);
                b.set("parent_window", item.parent_window);
                b.set("options", variantMapToJs(item.options));
                ev::Persistent evObj(b.get());
                for (const auto& cb : callbacks) {
                    if (!cb || !ev::isFunction(cb->get())) continue;
                    const Value arg = evObj.get();
                    ev::call(cb->get(), ev::undefined(), std::span<const Value>(&arg, 1));
                }
            }
        }
    }
}

void shutdownGlobalShortcuts() {
    std::vector<std::shared_ptr<QueuedBindShortcutsRequest>> bindToCancel;
    std::vector<std::shared_ptr<QueuedListShortcutsRequest>> listToCancel;
    {
        std::lock_guard lock(g_gs_mu);
        bindToCancel.swap(g_active_bind_requests);
        listToCancel.swap(g_active_list_requests);
        g_unhandled_bind_requests.clear();
        g_unhandled_list_requests.clear();
        g_unhandled_cfg_events.clear();
        g_bind_handlers.clear();
        g_list_handlers.clear();
        g_cfg_handlers.clear();
    }

    for (const auto& req : bindToCancel) {
        std::lock_guard lock(req->mu);
        req->response_code = ResponseCode::Cancelled;
        req->answered = true;
        req->cv.notify_all();
    }
    for (const auto& req : listToCancel) {
        std::lock_guard lock(req->mu);
        req->response_code = ResponseCode::Cancelled;
        req->answered = true;
        req->cv.notify_all();
    }
}

void installGlobalShortcutsOnto(Value portalObj) {
    ObjectBuilder portal(portalObj);

    // bro.portal.onBindShortcuts(handler) -> HandlerHandle
    portal.def("onBindShortcuts", 1, [](Value, std::span<const Value> args) -> Value {
        ev::Persistent p0(args.size() > 0 ? args[0] : ev::undefined());
        if (!ev::isFunction(p0.get())) {
            return ev::throwTypeError("onBindShortcuts: handler must be a function");
        }

        uint64_t hid = 0;
        {
            std::lock_guard lock(g_gs_mu);
            hid = ++g_next_gs_handler_id;
            g_bind_handlers.push_back(HandlerEntry{
                .id = hid,
                .callback = std::make_shared<ev::Persistent>(p0.get())
            });
        }

        auto b = activeBackend();
        if (b) {
            b->global_shortcuts().set_async_dispatch(true);
            b->global_shortcuts().set_bind_shortcuts_callback(handleNativeBindShortcuts);
        }

        ObjectBuilder handle;
        handle.set("id", static_cast<double>(hid));
        auto removeFn = [hid](Value, std::span<const Value>) -> Value {
            std::lock_guard lock(g_gs_mu);
            g_bind_handlers.erase(
                std::remove_if(g_bind_handlers.begin(), g_bind_handlers.end(),
                               [hid](const HandlerEntry& e) { return e.id == hid; }),
                g_bind_handlers.end());
            return ev::fromBool(true);
        };
        handle.def("dispose", 0, removeFn);
        handle.def("unregister", 0, removeFn);
        return handle.get();
    });

    // bro.portal.offBindShortcuts(handlerOrHandle) -> boolean
    portal.def("offBindShortcuts", 1, [](Value, std::span<const Value> args) -> Value {
        ev::Persistent p0(args.size() > 0 ? args[0] : ev::undefined());
        std::lock_guard lock(g_gs_mu);
        if (ev::isFunction(p0.get())) {
            g_bind_handlers.erase(
                std::remove_if(g_bind_handlers.begin(), g_bind_handlers.end(),
                               [&](const HandlerEntry& e) {
                                   return e.callback && e.callback->get() == p0.get();
                               }),
                g_bind_handlers.end());
            return ev::fromBool(true);
        }
        if (ev::isObject(p0.get())) {
            ev::Persistent pId(ev::getProperty(p0.get(), "id"));
            if (ev::isNumber(pId.get())) {
                uint64_t hid = static_cast<uint64_t>(ev::toDouble(pId.get()));
                g_bind_handlers.erase(
                    std::remove_if(g_bind_handlers.begin(), g_bind_handlers.end(),
                                   [hid](const HandlerEntry& e) { return e.id == hid; }),
                    g_bind_handlers.end());
                return ev::fromBool(true);
            }
        }
        return ev::fromBool(false);
    });

    // bro.portal.onListShortcuts(handler) -> HandlerHandle
    portal.def("onListShortcuts", 1, [](Value, std::span<const Value> args) -> Value {
        ev::Persistent p0(args.size() > 0 ? args[0] : ev::undefined());
        if (!ev::isFunction(p0.get())) {
            return ev::throwTypeError("onListShortcuts: handler must be a function");
        }

        uint64_t hid = 0;
        {
            std::lock_guard lock(g_gs_mu);
            hid = ++g_next_gs_handler_id;
            g_list_handlers.push_back(HandlerEntry{
                .id = hid,
                .callback = std::make_shared<ev::Persistent>(p0.get())
            });
        }

        auto b = activeBackend();
        if (b) {
            b->global_shortcuts().set_async_dispatch(true);
            b->global_shortcuts().set_list_shortcuts_callback(handleNativeListShortcuts);
        }

        ObjectBuilder handle;
        handle.set("id", static_cast<double>(hid));
        auto removeFn = [hid](Value, std::span<const Value>) -> Value {
            bool empty = false;
            {
                std::lock_guard lock(g_gs_mu);
                g_list_handlers.erase(
                    std::remove_if(g_list_handlers.begin(), g_list_handlers.end(),
                                   [hid](const HandlerEntry& e) { return e.id == hid; }),
                    g_list_handlers.end());
                empty = g_list_handlers.empty();
            }
            if (empty) {
                auto b2 = activeBackend();
                if (b2) b2->global_shortcuts().set_list_shortcuts_callback(nullptr);
            }
            return ev::fromBool(true);
        };
        handle.def("dispose", 0, removeFn);
        handle.def("unregister", 0, removeFn);
        return handle.get();
    });

    // bro.portal.offListShortcuts(handlerOrHandle) -> boolean
    portal.def("offListShortcuts", 1, [](Value, std::span<const Value> args) -> Value {
        ev::Persistent p0(args.size() > 0 ? args[0] : ev::undefined());
        bool removed = false;
        bool empty = false;
        {
            std::lock_guard lock(g_gs_mu);
            if (ev::isFunction(p0.get())) {
                g_list_handlers.erase(
                    std::remove_if(g_list_handlers.begin(), g_list_handlers.end(),
                                   [&](const HandlerEntry& e) {
                                       return e.callback && e.callback->get() == p0.get();
                                   }),
                    g_list_handlers.end());
                removed = true;
            } else if (ev::isObject(p0.get())) {
                ev::Persistent pId(ev::getProperty(p0.get(), "id"));
                if (ev::isNumber(pId.get())) {
                    uint64_t hid = static_cast<uint64_t>(ev::toDouble(pId.get()));
                    g_list_handlers.erase(
                        std::remove_if(g_list_handlers.begin(), g_list_handlers.end(),
                                       [hid](const HandlerEntry& e) { return e.id == hid; }),
                        g_list_handlers.end());
                    removed = true;
                }
            }
            empty = g_list_handlers.empty();
        }
        if (removed && empty) {
            auto b = activeBackend();
            if (b) b->global_shortcuts().set_list_shortcuts_callback(nullptr);
        }
        return ev::fromBool(removed);
    });

    // bro.portal.onConfigureShortcuts(handler) -> HandlerHandle
    portal.def("onConfigureShortcuts", 1, [](Value, std::span<const Value> args) -> Value {
        ev::Persistent p0(args.size() > 0 ? args[0] : ev::undefined());
        if (!ev::isFunction(p0.get())) {
            return ev::throwTypeError("onConfigureShortcuts: handler must be a function");
        }

        uint64_t hid = 0;
        {
            std::lock_guard lock(g_gs_mu);
            hid = ++g_next_gs_handler_id;
            g_cfg_handlers.push_back(HandlerEntry{
                .id = hid,
                .callback = std::make_shared<ev::Persistent>(p0.get())
            });
        }

        auto b = activeBackend();
        if (b) {
            b->global_shortcuts().set_configure_callback(handleNativeConfigureShortcuts);
        }

        ObjectBuilder handle;
        handle.set("id", static_cast<double>(hid));
        auto removeFn = [hid](Value, std::span<const Value>) -> Value {
            bool empty = false;
            {
                std::lock_guard lock(g_gs_mu);
                g_cfg_handlers.erase(
                    std::remove_if(g_cfg_handlers.begin(), g_cfg_handlers.end(),
                                   [hid](const HandlerEntry& e) { return e.id == hid; }),
                    g_cfg_handlers.end());
                empty = g_cfg_handlers.empty();
            }
            if (empty) {
                auto b2 = activeBackend();
                if (b2) b2->global_shortcuts().set_configure_callback(nullptr);
            }
            return ev::fromBool(true);
        };
        handle.def("dispose", 0, removeFn);
        handle.def("unregister", 0, removeFn);
        return handle.get();
    });

    // bro.portal.offConfigureShortcuts(handlerOrHandle) -> boolean
    portal.def("offConfigureShortcuts", 1, [](Value, std::span<const Value> args) -> Value {
        ev::Persistent p0(args.size() > 0 ? args[0] : ev::undefined());
        bool removed = false;
        bool empty = false;
        {
            std::lock_guard lock(g_gs_mu);
            if (ev::isFunction(p0.get())) {
                g_cfg_handlers.erase(
                    std::remove_if(g_cfg_handlers.begin(), g_cfg_handlers.end(),
                                   [&](const HandlerEntry& e) {
                                       return e.callback && e.callback->get() == p0.get();
                                   }),
                    g_cfg_handlers.end());
                removed = true;
            } else if (ev::isObject(p0.get())) {
                ev::Persistent pId(ev::getProperty(p0.get(), "id"));
                if (ev::isNumber(pId.get())) {
                    uint64_t hid = static_cast<uint64_t>(ev::toDouble(pId.get()));
                    g_cfg_handlers.erase(
                        std::remove_if(g_cfg_handlers.begin(), g_cfg_handlers.end(),
                                       [hid](const HandlerEntry& e) { return e.id == hid; }),
                        g_cfg_handlers.end());
                    removed = true;
                }
            }
            empty = g_cfg_handlers.empty();
        }
        if (removed && empty) {
            auto b = activeBackend();
            if (b) b->global_shortcuts().set_configure_callback(nullptr);
        }
        return ev::fromBool(removed);
    });

    // bro.portal.triggerShortcutActivated(sessionHandle, shortcutId, timestamp?) -> boolean
    auto activateFn = [](Value, std::span<const Value> args) -> Value {
        std::string session, shortcutId;
        uint64_t ts = 0;
        if (!parseTriggerArgs(args, &session, &shortcutId, &ts)) {
            return ev::throwTypeError("triggerShortcutActivated: expected (sessionHandle, shortcutId[, timestamp])");
        }
        auto b = activeBackend();
        if (!b) return ev::fromBool(false);
        return ev::fromBool(b->global_shortcuts().activate_shortcut(ObjectPath{session}, shortcutId, ts));
    };
    portal.def("triggerShortcutActivated", 2, activateFn);
    portal.def("activateShortcut", 2, activateFn);
    portal.def("emitShortcutActivated", 2, activateFn);

    // bro.portal.triggerShortcutDeactivated(sessionHandle, shortcutId, timestamp?) -> boolean
    auto deactivateFn = [](Value, std::span<const Value> args) -> Value {
        std::string session, shortcutId;
        uint64_t ts = 0;
        if (!parseTriggerArgs(args, &session, &shortcutId, &ts)) {
            return ev::throwTypeError("triggerShortcutDeactivated: expected (sessionHandle, shortcutId[, timestamp])");
        }
        auto b = activeBackend();
        if (!b) return ev::fromBool(false);
        return ev::fromBool(b->global_shortcuts().deactivate_shortcut(ObjectPath{session}, shortcutId, ts));
    };
    portal.def("triggerShortcutDeactivated", 2, deactivateFn);
    portal.def("deactivateShortcut", 2, deactivateFn);
    portal.def("emitShortcutDeactivated", 2, deactivateFn);

    // bro.portal.notifyShortcutsChanged(sessionHandle, shortcuts) -> boolean
    auto changedFn = [](Value, std::span<const Value> args) -> Value {
        if (args.size() < 2 || !ev::isString(args[0]) || !ev::isObject(args[1])) {
            return ev::throwTypeError("notifyShortcutsChanged: expected (sessionHandle, shortcuts)");
        }
        ev::Persistent pSess(args[0]);
        ev::Persistent pArr(args[1]);
        std::string session = ev::toUtf8(pSess.get());
        ShortcutList list = parseShortcutListFromJs(pArr.get());
        auto b = activeBackend();
        if (!b) return ev::fromBool(false);
        return ev::fromBool(b->global_shortcuts().notify_shortcuts_changed(ObjectPath{session}, list));
    };
    portal.def("notifyShortcutsChanged", 2, changedFn);
    portal.def("updateShortcuts", 2, changedFn);
}

} // namespace broportal::api
