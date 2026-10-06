#include "host_portal_internal.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <vector>

namespace broportal::api {

namespace {

struct QueuedScreenCastRequest {
    uint64_t id = 0;
    ObjectPath handle;
    ObjectPath session_handle;
    std::string app_id;
    ScreenCastSourceOptions options;

    std::mutex mu;
    std::condition_variable cv;
    bool answered = false;
    ResponseCode response_code = ResponseCode::OtherError;
    StreamList out_streams;
    VariantMap out_results;
};

struct HandlerEntry {
    uint64_t id = 0;
    std::shared_ptr<ev::Persistent> callback;
};

std::mutex g_sc_mu;
uint64_t g_next_sc_handler_id = 1;
uint64_t g_next_sc_req_id = 1;
std::vector<HandlerEntry> g_sc_handlers;
std::vector<std::shared_ptr<QueuedScreenCastRequest>> g_active_sc_requests;
std::vector<std::shared_ptr<QueuedScreenCastRequest>> g_unhandled_sc_requests;

void fulfillScreenCast(const std::shared_ptr<QueuedScreenCastRequest>& req,
                       ResponseCode code,
                       StreamList streams,
                       VariantMap results = {}) {
    std::lock_guard lock(req->mu);
    if (req->answered) return;
    req->response_code = code;
    req->out_streams = std::move(streams);
    req->out_results = std::move(results);
    if (!req->out_streams.empty() && !req->out_results.contains("streams")) {
        req->out_results["streams"] = Variant(req->out_streams);
    }
    req->answered = true;
    req->cv.notify_all();
}

void parseAndFulfillScreenCast(const std::shared_ptr<QueuedScreenCastRequest>& req, Value resVal) {
    ev::Persistent val(resVal);
    if (!ev::isObject(val.get())) return;

    ev::Persistent pCancelled(ev::getProperty(val.get(), "cancelled"));
    if (ev::isBool(pCancelled.get()) && ev::toBool(pCancelled.get())) {
        fulfillScreenCast(req, ResponseCode::Cancelled, {});
        return;
    }

    StreamList stream_list;
    ev::Persistent pStreams(ev::getProperty(val.get(), "streams"));
    if (ev::isObject(pStreams.get())) {
        ev::Persistent pLen(ev::getProperty(pStreams.get(), "length"));
        if (ev::isNumber(pLen.get())) {
            uint32_t len = static_cast<uint32_t>(ev::toDouble(pLen.get()));
            for (uint32_t i = 0; i < len; ++i) {
                ev::Persistent item(ev::getElement(pStreams.get(), i));
                if (ev::isNumber(item.get())) {
                    uint32_t nid = static_cast<uint32_t>(ev::toDouble(item.get()));
                    stream_list.push_back({nid, VariantMap{}});
                } else if (ev::isObject(item.get())) {
                    ev::Persistent pId(ev::getProperty(item.get(), "nodeId"));
                    if (!ev::isNumber(pId.get())) {
                        pId.set(ev::getProperty(item.get(), "node_id"));
                    }
                    if (ev::isNumber(pId.get())) {
                        uint32_t nid = static_cast<uint32_t>(ev::toDouble(pId.get()));
                        VariantMap props;
                        ev::Persistent pProps(ev::getProperty(item.get(), "properties"));
                        if (ev::isObject(pProps.get())) {
                            // Read known properties or extra
                        }
                        stream_list.push_back({nid, props});
                    }
                }
            }
        }
    }

    fulfillScreenCast(req, ResponseCode::Success, std::move(stream_list), {});
}

Value buildScreenCastRequest(const std::shared_ptr<QueuedScreenCastRequest>& req) {
    ObjectBuilder b;
    b.set("handle", req->handle.path);
    b.set("sessionHandle", req->session_handle.path);
    b.set("session_handle", req->session_handle.path);
    b.set("appId", req->app_id);
    b.set("app_id", req->app_id);

    ObjectBuilder opts;
    opts.set("types", static_cast<double>(req->options.types));
    opts.set("multiple", req->options.multiple);
    opts.set("cursorMode", static_cast<double>(req->options.cursor_mode));
    opts.set("cursor_mode", static_cast<double>(req->options.cursor_mode));
    opts.set("persistMode", static_cast<double>(req->options.persist_mode));
    opts.set("persist_mode", static_cast<double>(req->options.persist_mode));
    opts.set("extraOptions", variantMapToJs(req->options.extra_options));
    opts.set("extra_options", variantMapToJs(req->options.extra_options));
    b.set("options", opts.get());

    b.def("respond", 1, [req](Value, std::span<const Value> args) -> Value {
        ev::Persistent p0(args.size() > 0 ? args[0] : ev::undefined());
        parseAndFulfillScreenCast(req, p0.get());
        return ev::fromBool(true);
    });

    b.def("cancel", 0, [req](Value, std::span<const Value>) -> Value {
        fulfillScreenCast(req, ResponseCode::Cancelled, {});
        return ev::fromBool(true);
    });

    return b.get();
}

} // namespace

ResponseCode handleNativeScreenCast(
    const ObjectPath& handle,
    const ObjectPath& session_handle,
    const std::string& app_id,
    const ScreenCastSourceOptions& options,
    StreamList& out_streams,
    VariantMap& out_results) {
    auto req = std::make_shared<QueuedScreenCastRequest>();
    req->handle = handle;
    req->session_handle = session_handle;
    req->app_id = app_id;
    req->options = options;

    {
        std::lock_guard lock(g_sc_mu);
        if (g_sc_handlers.empty()) {
            return ResponseCode::OtherError;
        }
        req->id = ++g_next_sc_req_id;
        g_active_sc_requests.push_back(req);
        g_unhandled_sc_requests.push_back(req);
    }

    std::unique_lock ulock(req->mu);
    bool ok = req->cv.wait_for(ulock, std::chrono::seconds(60), [&]() {
        return req->answered;
    });

    {
        std::lock_guard lock(g_sc_mu);
        for (auto it = g_active_sc_requests.begin(); it != g_active_sc_requests.end(); ++it) {
            if ((*it)->id == req->id) {
                g_active_sc_requests.erase(it);
                break;
            }
        }
    }

    if (!ok || !req->answered) {
        return ResponseCode::OtherError;
    }

    out_streams = std::move(req->out_streams);
    out_results = std::move(req->out_results);
    return req->response_code;
}

void drainScreenCast() {
    std::vector<std::shared_ptr<QueuedScreenCastRequest>> toDispatch;
    std::vector<std::shared_ptr<ev::Persistent>> callbacks;
    {
        std::lock_guard lock(g_sc_mu);
        toDispatch.swap(g_unhandled_sc_requests);
        for (const auto& entry : g_sc_handlers) {
            if (entry.callback) callbacks.push_back(entry.callback);
        }
    }

    if (toDispatch.empty() || callbacks.empty()) return;

    for (const auto& req : toDispatch) {
        ev::Persistent reqObj(buildScreenCastRequest(req));
        for (const auto& cb : callbacks) {
            if (!cb || !ev::isFunction(cb->get())) continue;
            const Value arg = reqObj.get();
            ev::Persistent retVal(ev::call(cb->get(), ev::undefined(), std::span<const Value>(&arg, 1)).value);

            if (ev::isPromise(retVal.get())) {
                attachPromiseReaction(
                    retVal.get(),
                    [req](Value val) { parseAndFulfillScreenCast(req, val); },
                    [req](Value) { fulfillScreenCast(req, ResponseCode::Cancelled, {}); });
            } else if (ev::isObject(retVal.get())) {
                parseAndFulfillScreenCast(req, retVal.get());
            }
        }
    }
}

void shutdownScreenCast() {
    std::vector<std::shared_ptr<QueuedScreenCastRequest>> toCancel;
    {
        std::lock_guard lock(g_sc_mu);
        toCancel.swap(g_active_sc_requests);
        g_unhandled_sc_requests.clear();
        g_sc_handlers.clear();
    }

    for (const auto& req : toCancel) {
        std::lock_guard lock(req->mu);
        req->response_code = ResponseCode::Cancelled;
        req->answered = true;
        req->cv.notify_all();
    }
}

void installScreenCastOnto(Value portalObj) {
    ObjectBuilder portal(portalObj);

    // bro.portal.onScreencast(handler) -> HandlerHandle
    portal.def("onScreencast", 1, [](Value, std::span<const Value> args) -> Value {
        ev::Persistent p0(args.size() > 0 ? args[0] : ev::undefined());
        if (!ev::isFunction(p0.get())) {
            return ev::throwTypeError("onScreencast: handler must be a function");
        }

        uint64_t hid = 0;
        {
            std::lock_guard lock(g_sc_mu);
            hid = ++g_next_sc_handler_id;
            g_sc_handlers.push_back(HandlerEntry{
                .id = hid,
                .callback = std::make_shared<ev::Persistent>(p0.get())
            });
        }

#if defined(__linux__)
        auto b = activeBackend();
        if (b) {
            b->set_screencast_handler(handleNativeScreenCast);
        }
#endif

        ObjectBuilder handle;
        handle.set("id", static_cast<double>(hid));
        handle.def("dispose", 0, [hid](Value, std::span<const Value>) -> Value {
            std::lock_guard lock(g_sc_mu);
            g_sc_handlers.erase(
                std::remove_if(g_sc_handlers.begin(), g_sc_handlers.end(),
                               [hid](const HandlerEntry& e) { return e.id == hid; }),
                g_sc_handlers.end());
            return ev::fromBool(true);
        });
        handle.def("unregister", 0, [hid](Value, std::span<const Value>) -> Value {
            std::lock_guard lock(g_sc_mu);
            g_sc_handlers.erase(
                std::remove_if(g_sc_handlers.begin(), g_sc_handlers.end(),
                               [hid](const HandlerEntry& e) { return e.id == hid; }),
                g_sc_handlers.end());
            return ev::fromBool(true);
        });

        return handle.get();
    });

    // bro.portal.offScreencast(handlerOrHandle) -> boolean
    portal.def("offScreencast", 1, [](Value, std::span<const Value> args) -> Value {
        ev::Persistent p0(args.size() > 0 ? args[0] : ev::undefined());
        std::lock_guard lock(g_sc_mu);
        if (ev::isFunction(p0.get())) {
            g_sc_handlers.erase(
                std::remove_if(g_sc_handlers.begin(), g_sc_handlers.end(),
                               [&](const HandlerEntry& e) {
                                   return e.callback && e.callback->get() == p0.get();
                               }),
                g_sc_handlers.end());
            return ev::fromBool(true);
        }
        if (ev::isObject(p0.get())) {
            ev::Persistent pId(ev::getProperty(p0.get(), "id"));
            if (ev::isNumber(pId.get())) {
                uint64_t hid = static_cast<uint64_t>(ev::toDouble(pId.get()));
                g_sc_handlers.erase(
                    std::remove_if(g_sc_handlers.begin(), g_sc_handlers.end(),
                                   [hid](const HandlerEntry& e) { return e.id == hid; }),
                    g_sc_handlers.end());
                return ev::fromBool(true);
            }
        }
        return ev::fromBool(false);
    });
}

} // namespace broportal::api
