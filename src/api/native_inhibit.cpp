#include "host_portal_internal.h"

#include <algorithm>
#include <cmath>
#include <mutex>
#include <vector>

namespace broportal::api {

namespace {

struct QueuedInhibitEvent {
    bool added = false;
    InhibitEntry entry;
};

struct QueuedQueryEndEvent {
    ObjectPath session_handle;
};

struct QueuedStateChangedEvent {
    bool screensaver_active = false;
    SessionState state = SessionState::Running;
};

struct HandlerEntry {
    uint64_t id = 0;
    std::shared_ptr<ev::Persistent> callback;
};

std::mutex g_inh_mu;
uint64_t g_next_inh_handler_id = 1;
std::vector<HandlerEntry> g_inh_handlers;
std::vector<HandlerEntry> g_qe_handlers;
std::vector<HandlerEntry> g_state_handlers;
std::vector<QueuedInhibitEvent> g_unhandled_inh_events;
std::vector<QueuedQueryEndEvent> g_unhandled_qe_events;
std::vector<QueuedStateChangedEvent> g_unhandled_state_events;

Value buildInhibitEntryJs(const InhibitEntry& entry, std::optional<bool> added = std::nullopt) {
    ObjectBuilder b;
    if (added.has_value()) {
        b.set("added", *added);
        b.set("active", *added);
        b.set("removed", !*added);
    }
    b.set("handle", entry.handle.path);
    b.set("appId", entry.app_id);
    b.set("app_id", entry.app_id);
    b.set("window", entry.window);
    b.set("flags", static_cast<double>(entry.flags));
    b.set("reason", entry.reason);
    b.set("logout", (entry.flags & static_cast<uint32_t>(InhibitFlag::Logout)) != 0);
    b.set("userSwitch", (entry.flags & static_cast<uint32_t>(InhibitFlag::UserSwitch)) != 0);
    b.set("user_switch", (entry.flags & static_cast<uint32_t>(InhibitFlag::UserSwitch)) != 0);
    b.set("suspend", (entry.flags & static_cast<uint32_t>(InhibitFlag::Suspend)) != 0);
    b.set("idle", (entry.flags & static_cast<uint32_t>(InhibitFlag::Idle)) != 0);
    return b.get();
}

bool parseSessionState(Value val, SessionState* out_state) {
    ev::Persistent p(val);
    if (ev::isUndefined(p.get()) || ev::isNull(p.get())) {
        *out_state = SessionState::Running;
        return true;
    }
    if (ev::isNumber(p.get())) {
        double d = ev::toDouble(p.get());
        if (d == 1.0) { *out_state = SessionState::Running; return true; }
        if (d == 2.0) { *out_state = SessionState::QueryEnd; return true; }
        if (d == 3.0) { *out_state = SessionState::Ending; return true; }
        return false;
    }
    if (ev::isString(p.get())) {
        std::string s = ev::toUtf8(p.get());
        if (s == "running" || s == "Running" || s == "1") {
            *out_state = SessionState::Running;
            return true;
        }
        if (s == "query-end" || s == "query_end" || s == "queryEnd" || s == "QueryEnd" || s == "2") {
            *out_state = SessionState::QueryEnd;
            return true;
        }
        if (s == "ending" || s == "Ending" || s == "3") {
            *out_state = SessionState::Ending;
            return true;
        }
    }
    return false;
}

bool parseInhibitFlag(Value val, InhibitFlag* out_flag) {
    ev::Persistent p(val);
    if (ev::isNumber(p.get())) {
        uint32_t f = static_cast<uint32_t>(ev::toDouble(p.get()));
        if (f == 1 || f == 2 || f == 4 || f == 8) {
            *out_flag = static_cast<InhibitFlag>(f);
            return true;
        }
        return false;
    }
    if (ev::isString(p.get())) {
        std::string s = ev::toUtf8(p.get());
        if (s == "logout" || s == "Logout") { *out_flag = InhibitFlag::Logout; return true; }
        if (s == "user-switch" || s == "user_switch" || s == "userSwitch" || s == "UserSwitch") {
            *out_flag = InhibitFlag::UserSwitch;
            return true;
        }
        if (s == "suspend" || s == "Suspend") { *out_flag = InhibitFlag::Suspend; return true; }
        if (s == "idle" || s == "Idle") { *out_flag = InhibitFlag::Idle; return true; }
    }
    return false;
}

Value registerHandlerList(std::vector<HandlerEntry>& list, Value fnVal) {
    ev::Persistent p0(fnVal);
    uint64_t hid = 0;
    {
        std::lock_guard lock(g_inh_mu);
        hid = ++g_next_inh_handler_id;
        list.push_back(HandlerEntry{
            .id = hid,
            .callback = std::make_shared<ev::Persistent>(p0.get())
        });
    }

    auto b = activeBackend();
    if (b) {
        attachInhibitBackend(*b);
    }

    ObjectBuilder handle;
    handle.set("id", static_cast<double>(hid));
    auto removeFn = [&list, hid](Value, std::span<const Value>) -> Value {
        std::lock_guard lock(g_inh_mu);
        list.erase(
            std::remove_if(list.begin(), list.end(),
                           [hid](const HandlerEntry& e) { return e.id == hid; }),
            list.end());
        return ev::fromBool(true);
    };
    handle.def("dispose", 0, removeFn);
    handle.def("unregister", 0, removeFn);
    return handle.get();
}

Value unregisterFromHandlerList(std::vector<HandlerEntry>& list, Value arg) {
    ev::Persistent p0(arg);
    std::lock_guard lock(g_inh_mu);
    if (ev::isFunction(p0.get())) {
        list.erase(
            std::remove_if(list.begin(), list.end(),
                           [&](const HandlerEntry& e) {
                               return e.callback && e.callback->get() == p0.get();
                           }),
            list.end());
        return ev::fromBool(true);
    }
    if (ev::isObject(p0.get())) {
        ev::Persistent pId(ev::getProperty(p0.get(), "id"));
        if (ev::isNumber(pId.get())) {
            uint64_t hid = static_cast<uint64_t>(ev::toDouble(pId.get()));
            list.erase(
                std::remove_if(list.begin(), list.end(),
                               [hid](const HandlerEntry& e) { return e.id == hid; }),
                list.end());
            return ev::fromBool(true);
        }
    }
    return ev::fromBool(false);
}

} // namespace

void handleNativeInhibitChange(bool added, const InhibitEntry& entry) {
    std::lock_guard lock(g_inh_mu);
    if (g_inh_handlers.empty()) return;
    g_unhandled_inh_events.push_back(QueuedInhibitEvent{added, entry});
}

void handleNativeQueryEndResponse(const ObjectPath& session_handle) {
    std::lock_guard lock(g_inh_mu);
    if (g_qe_handlers.empty()) return;
    g_unhandled_qe_events.push_back(QueuedQueryEndEvent{session_handle});
}

void attachInhibitBackend(broportal::PortalBackend& backend) {
    backend.inhibit().set_inhibit_change_listener(handleNativeInhibitChange);
    backend.inhibit().set_query_end_listener(handleNativeQueryEndResponse);
}

void drainInhibit() {
    // 1. Inhibit added/removed events
    {
        std::vector<QueuedInhibitEvent> toDispatch;
        std::vector<std::shared_ptr<ev::Persistent>> callbacks;
        {
            std::lock_guard lock(g_inh_mu);
            toDispatch.swap(g_unhandled_inh_events);
            for (const auto& entry : g_inh_handlers) {
                if (entry.callback) callbacks.push_back(entry.callback);
            }
        }
        if (!toDispatch.empty() && !callbacks.empty()) {
            for (const auto& item : toDispatch) {
                ev::Persistent evObj(buildInhibitEntryJs(item.entry, item.added));
                for (const auto& cb : callbacks) {
                    if (!cb || !ev::isFunction(cb->get())) continue;
                    const Value arg = evObj.get();
                    ev::call(cb->get(), ev::undefined(), std::span<const Value>(&arg, 1));
                }
            }
        }
    }

    // 2. QueryEndResponse events
    {
        std::vector<QueuedQueryEndEvent> toDispatch;
        std::vector<std::shared_ptr<ev::Persistent>> callbacks;
        {
            std::lock_guard lock(g_inh_mu);
            toDispatch.swap(g_unhandled_qe_events);
            for (const auto& entry : g_qe_handlers) {
                if (entry.callback) callbacks.push_back(entry.callback);
            }
        }
        if (!toDispatch.empty() && !callbacks.empty()) {
            for (const auto& item : toDispatch) {
                ObjectBuilder b;
                b.set("sessionHandle", item.session_handle.path);
                b.set("session_handle", item.session_handle.path);
                b.set("handle", item.session_handle.path);
                ev::Persistent evObj(b.get());
                ev::Persistent pathStr(ev::fromUtf8(item.session_handle.path));
                for (const auto& cb : callbacks) {
                    if (!cb || !ev::isFunction(cb->get())) continue;
                    const Value callArgs[2] = {evObj.get(), pathStr.get()};
                    ev::call(cb->get(), ev::undefined(), std::span<const Value>(callArgs, 2));
                }
            }
        }
    }

    // 3. StateChanged events
    {
        std::vector<QueuedStateChangedEvent> toDispatch;
        std::vector<std::shared_ptr<ev::Persistent>> callbacks;
        {
            std::lock_guard lock(g_inh_mu);
            toDispatch.swap(g_unhandled_state_events);
            for (const auto& entry : g_state_handlers) {
                if (entry.callback) callbacks.push_back(entry.callback);
            }
        }
        if (!toDispatch.empty() && !callbacks.empty()) {
            for (const auto& item : toDispatch) {
                ObjectBuilder b;
                b.set("screensaverActive", item.screensaver_active);
                b.set("screensaver_active", item.screensaver_active);
                b.set("sessionState", static_cast<double>(item.state));
                b.set("session_state", static_cast<double>(item.state));
                b.set("state", static_cast<double>(item.state));
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

void shutdownInhibit() {
    std::lock_guard lock(g_inh_mu);
    g_unhandled_inh_events.clear();
    g_unhandled_qe_events.clear();
    g_unhandled_state_events.clear();
    g_inh_handlers.clear();
    g_qe_handlers.clear();
    g_state_handlers.clear();
}

void installInhibitOnto(Value portalObj) {
    ObjectBuilder portal(portalObj);

    // bro.portal.onInhibit(handler) -> HandlerHandle
    portal.def("onInhibit", 1, [](Value, std::span<const Value> args) -> Value {
        ev::Persistent p0(args.size() > 0 ? args[0] : ev::undefined());
        if (!ev::isFunction(p0.get())) {
            return ev::throwTypeError("onInhibit: handler must be a function");
        }
        return registerHandlerList(g_inh_handlers, p0.get());
    });

    // bro.portal.offInhibit(handlerOrHandle) -> boolean
    portal.def("offInhibit", 1, [](Value, std::span<const Value> args) -> Value {
        ev::Persistent p0(args.size() > 0 ? args[0] : ev::undefined());
        return unregisterFromHandlerList(g_inh_handlers, p0.get());
    });

    // bro.portal.onQueryEndResponse(handler) -> HandlerHandle
    portal.def("onQueryEndResponse", 1, [](Value, std::span<const Value> args) -> Value {
        ev::Persistent p0(args.size() > 0 ? args[0] : ev::undefined());
        if (!ev::isFunction(p0.get())) {
            return ev::throwTypeError("onQueryEndResponse: handler must be a function");
        }
        return registerHandlerList(g_qe_handlers, p0.get());
    });

    // bro.portal.offQueryEndResponse(handlerOrHandle) -> boolean
    portal.def("offQueryEndResponse", 1, [](Value, std::span<const Value> args) -> Value {
        ev::Persistent p0(args.size() > 0 ? args[0] : ev::undefined());
        return unregisterFromHandlerList(g_qe_handlers, p0.get());
    });

    // bro.portal.queryEndResponse(sessionHandleOrHandler)
    portal.def("queryEndResponse", 1, [](Value, std::span<const Value> args) -> Value {
        if (args.empty()) {
            return ev::throwTypeError("queryEndResponse: expected session handle string or listener function");
        }
        ev::Persistent p0(args[0]);
        if (ev::isFunction(p0.get())) {
            return registerHandlerList(g_qe_handlers, p0.get());
        }
        std::string sessionPath;
        if (ev::isString(p0.get())) {
            sessionPath = ev::toUtf8(p0.get());
        } else if (ev::isObject(p0.get())) {
            ev::Persistent pSh(ev::getProperty(p0.get(), "sessionHandle"));
            if (!ev::isString(pSh.get())) pSh.set(ev::getProperty(p0.get(), "session_handle"));
            if (!ev::isString(pSh.get())) pSh.set(ev::getProperty(p0.get(), "handle"));
            if (ev::isString(pSh.get())) sessionPath = ev::toUtf8(pSh.get());
        }
        if (sessionPath.empty()) {
            return ev::throwTypeError("queryEndResponse: invalid session handle");
        }
        auto b = activeBackend();
        if (!b) return ev::fromBool(false);
        b->inhibit().query_end_response(ObjectPath{sessionPath});
        return ev::fromBool(true);
    });

    // bro.portal.onStateChanged(handler) -> HandlerHandle
    portal.def("onStateChanged", 1, [](Value, std::span<const Value> args) -> Value {
        ev::Persistent p0(args.size() > 0 ? args[0] : ev::undefined());
        if (!ev::isFunction(p0.get())) {
            return ev::throwTypeError("onStateChanged: handler must be a function");
        }
        return registerHandlerList(g_state_handlers, p0.get());
    });

    // bro.portal.offStateChanged(handlerOrHandle) -> boolean
    portal.def("offStateChanged", 1, [](Value, std::span<const Value> args) -> Value {
        ev::Persistent p0(args.size() > 0 ? args[0] : ev::undefined());
        return unregisterFromHandlerList(g_state_handlers, p0.get());
    });

    // bro.portal.notifyStateChanged(screensaverActive, sessionState?) -> boolean
    auto notifyStateFn = [](Value, std::span<const Value> args) -> Value {
        bool screensaver_active = false;
        SessionState state = SessionState::Running;

        if (args.size() >= 1 && ev::isObject(args[0])) {
            ev::Persistent p0(args[0]);
            ev::Persistent pSc(ev::getProperty(p0.get(), "screensaverActive"));
            if (ev::isUndefined(pSc.get())) pSc.set(ev::getProperty(p0.get(), "screensaver_active"));
            if (!ev::isUndefined(pSc.get())) screensaver_active = ev::toBool(pSc.get());

            ev::Persistent pSt(ev::getProperty(p0.get(), "sessionState"));
            if (ev::isUndefined(pSt.get())) pSt.set(ev::getProperty(p0.get(), "session_state"));
            if (ev::isUndefined(pSt.get())) pSt.set(ev::getProperty(p0.get(), "state"));
            if (!parseSessionState(pSt.get(), &state)) {
                return ev::throwTypeError("notifyStateChanged: invalid sessionState");
            }
        } else {
            if (args.size() >= 1) {
                screensaver_active = ev::toBool(args[0]);
            }
            if (args.size() >= 2) {
                ev::Persistent p1(args[1]);
                if (!parseSessionState(p1.get(), &state)) {
                    return ev::throwTypeError("notifyStateChanged: invalid sessionState");
                }
            }
        }

        auto b = activeBackend();
        if (!b) return ev::fromBool(false);
        b->inhibit().notify_state_changed(screensaver_active, state);
        {
            std::lock_guard lock(g_inh_mu);
            if (!g_state_handlers.empty()) {
                g_unhandled_state_events.push_back(QueuedStateChangedEvent{screensaver_active, state});
            }
        }
        return ev::fromBool(true);
    };
    portal.def("notifyStateChanged", 2, notifyStateFn);
    portal.def("emitStateChanged", 2, notifyStateFn);
    portal.def("setSessionState", 2, notifyStateFn);

    // bro.portal.getActiveInhibitions() -> InhibitEntry[]
    auto getActiveFn = [](Value, std::span<const Value>) -> Value {
        auto b = activeBackend();
        if (!b) return ev::makeArray(0);
        auto entries = b->inhibit().active_inhibitions();
        ev::Persistent arr(ev::makeArray(static_cast<uint32_t>(entries.size())));
        for (uint32_t i = 0; i < entries.size(); ++i) {
            ev::Persistent elem(buildInhibitEntryJs(entries[i]));
            arr.set(ev::setElement(arr.get(), i, elem.get()));
        }
        return arr.get();
    };
    portal.def("getActiveInhibitions", 0, getActiveFn);
    portal.def("activeInhibitions", 0, getActiveFn);

    // bro.portal.isInhibited([flag]) -> boolean
    portal.def("isInhibited", 0, [](Value, std::span<const Value> args) -> Value {
        auto b = activeBackend();
        if (!b) return ev::fromBool(false);
        if (args.empty() || ev::isUndefined(args[0]) || ev::isNull(args[0])) {
            return ev::fromBool(!b->inhibit().active_inhibitions().empty());
        }
        ev::Persistent p0(args[0]);
        InhibitFlag flag = InhibitFlag::Idle;
        if (!parseInhibitFlag(p0.get(), &flag)) {
            if (ev::isNumber(p0.get())) {
                uint32_t mask = static_cast<uint32_t>(ev::toDouble(p0.get()));
                for (const auto& entry : b->inhibit().active_inhibitions()) {
                    if ((entry.flags & mask) != 0) return ev::fromBool(true);
                }
                return ev::fromBool(false);
            }
            return ev::throwTypeError("isInhibited: expected 'logout'|'user-switch'|'suspend'|'idle' or flag bitmask");
        }
        return ev::fromBool(b->inhibit().is_inhibited(flag));
    });
}

} // namespace broportal::api
