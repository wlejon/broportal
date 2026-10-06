#include "host_portal_internal.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <vector>

namespace broportal::api {

namespace {

struct QueuedOpenUriRequest {
    uint64_t id = 0;
    ObjectPath handle;
    std::string app_id;
    std::string uri;
    int fd = -1;
    VariantMap options;

    std::mutex mu;
    std::condition_variable cv;
    bool answered = false;
    ResponseCode response_code = ResponseCode::OtherError;
    VariantMap out_results;
};

struct HandlerEntry {
    uint64_t id = 0;
    std::shared_ptr<ev::Persistent> callback;
};

std::mutex g_uri_mu;
uint64_t g_next_uri_handler_id = 1;
uint64_t g_next_uri_req_id = 1;
std::vector<HandlerEntry> g_uri_handlers;
std::vector<std::shared_ptr<QueuedOpenUriRequest>> g_active_uri_requests;
std::vector<std::shared_ptr<QueuedOpenUriRequest>> g_unhandled_uri_requests;

void fulfillOpenUri(const std::shared_ptr<QueuedOpenUriRequest>& req,
                    ResponseCode code,
                    VariantMap results = {}) {
    std::lock_guard lock(req->mu);
    if (req->answered) return;
    req->response_code = code;
    req->out_results = std::move(results);
    req->answered = true;
    req->cv.notify_all();
}

void parseAndFulfillOpenUri(const std::shared_ptr<QueuedOpenUriRequest>& req, Value resVal) {
    ev::Persistent val(resVal);
    if (!ev::isObject(val.get())) return;

    ev::Persistent pCancelled(ev::getProperty(val.get(), "cancelled"));
    if (ev::isBool(pCancelled.get()) && ev::toBool(pCancelled.get())) {
        fulfillOpenUri(req, ResponseCode::Cancelled, {});
        return;
    }

    ev::Persistent pSuccess(ev::getProperty(val.get(), "success"));
    bool success = true;
    if (ev::isBool(pSuccess.get())) {
        success = ev::toBool(pSuccess.get());
    }

    fulfillOpenUri(req, success ? ResponseCode::Success : ResponseCode::OtherError, {});
}

Value buildOpenUriRequest(const std::shared_ptr<QueuedOpenUriRequest>& req) {
    ObjectBuilder b;
    b.set("handle", req->handle.path);
    b.set("appId", req->app_id);
    b.set("app_id", req->app_id);
    b.set("uri", req->uri);
    b.set("fd", static_cast<double>(req->fd));
    b.set("options", variantMapToJs(req->options));

    b.def("respond", 1, [req](Value, std::span<const Value> args) -> Value {
        ev::Persistent p0(args.size() > 0 ? args[0] : ev::undefined());
        parseAndFulfillOpenUri(req, p0.get());
        return ev::fromBool(true);
    });

    b.def("cancel", 0, [req](Value, std::span<const Value>) -> Value {
        fulfillOpenUri(req, ResponseCode::Cancelled, {});
        return ev::fromBool(true);
    });

    return b.get();
}

} // namespace

ResponseCode handleNativeOpenUri(
    const ObjectPath& handle,
    const std::string& app_id,
    const std::string& uri,
    const VariantMap& options,
    VariantMap& out_results) {
    auto req = std::make_shared<QueuedOpenUriRequest>();
    req->handle = handle;
    req->app_id = app_id;
    req->uri = uri;
    req->fd = -1;
    req->options = options;

    {
        std::lock_guard lock(g_uri_mu);
        if (g_uri_handlers.empty()) {
            return ResponseCode::OtherError;
        }
        req->id = ++g_next_uri_req_id;
        g_active_uri_requests.push_back(req);
        g_unhandled_uri_requests.push_back(req);
    }

    std::unique_lock ulock(req->mu);
    bool ok = req->cv.wait_for(ulock, std::chrono::seconds(60), [&]() {
        return req->answered;
    });

    {
        std::lock_guard lock(g_uri_mu);
        for (auto it = g_active_uri_requests.begin(); it != g_active_uri_requests.end(); ++it) {
            if ((*it)->id == req->id) {
                g_active_uri_requests.erase(it);
                break;
            }
        }
    }

    if (!ok || !req->answered) {
        return ResponseCode::OtherError;
    }

    out_results = std::move(req->out_results);
    return req->response_code;
}

ResponseCode handleNativeOpenFile(
    const ObjectPath& handle,
    const std::string& app_id,
    int fd,
    const VariantMap& options,
    VariantMap& out_results) {
    auto req = std::make_shared<QueuedOpenUriRequest>();
    req->handle = handle;
    req->app_id = app_id;
    req->uri = "";
    req->fd = fd;
    req->options = options;

    {
        std::lock_guard lock(g_uri_mu);
        if (g_uri_handlers.empty()) {
            return ResponseCode::OtherError;
        }
        req->id = ++g_next_uri_req_id;
        g_active_uri_requests.push_back(req);
        g_unhandled_uri_requests.push_back(req);
    }

    std::unique_lock ulock(req->mu);
    bool ok = req->cv.wait_for(ulock, std::chrono::seconds(60), [&]() {
        return req->answered;
    });

    {
        std::lock_guard lock(g_uri_mu);
        for (auto it = g_active_uri_requests.begin(); it != g_active_uri_requests.end(); ++it) {
            if ((*it)->id == req->id) {
                g_active_uri_requests.erase(it);
                break;
            }
        }
    }

    if (!ok || !req->answered) {
        return ResponseCode::OtherError;
    }

    out_results = std::move(req->out_results);
    return req->response_code;
}

void drainOpenUri() {
    std::vector<std::shared_ptr<QueuedOpenUriRequest>> toDispatch;
    std::vector<std::shared_ptr<ev::Persistent>> callbacks;
    {
        std::lock_guard lock(g_uri_mu);
        toDispatch.swap(g_unhandled_uri_requests);
        for (const auto& entry : g_uri_handlers) {
            if (entry.callback) callbacks.push_back(entry.callback);
        }
    }

    if (toDispatch.empty() || callbacks.empty()) return;

    for (const auto& req : toDispatch) {
        ev::Persistent reqObj(buildOpenUriRequest(req));
        for (const auto& cb : callbacks) {
            if (!cb || !ev::isFunction(cb->get())) continue;
            const Value arg = reqObj.get();
            ev::Persistent retVal(ev::call(cb->get(), ev::undefined(), std::span<const Value>(&arg, 1)).value);

            if (ev::isPromise(retVal.get())) {
                attachPromiseReaction(
                    retVal.get(),
                    [req](Value val) { parseAndFulfillOpenUri(req, val); },
                    [req](Value) { fulfillOpenUri(req, ResponseCode::Cancelled, {}); });
            } else if (ev::isObject(retVal.get())) {
                parseAndFulfillOpenUri(req, retVal.get());
            }
        }
    }
}

void shutdownOpenUri() {
    std::vector<std::shared_ptr<QueuedOpenUriRequest>> toCancel;
    {
        std::lock_guard lock(g_uri_mu);
        toCancel.swap(g_active_uri_requests);
        g_unhandled_uri_requests.clear();
        g_uri_handlers.clear();
    }

    for (const auto& req : toCancel) {
        std::lock_guard lock(req->mu);
        req->response_code = ResponseCode::Cancelled;
        req->answered = true;
        req->cv.notify_all();
    }
}

void installOpenUriOnto(Value portalObj) {
    ObjectBuilder portal(portalObj);

    // bro.portal.onOpenUri(handler) -> HandlerHandle
    portal.def("onOpenUri", 1, [](Value, std::span<const Value> args) -> Value {
        ev::Persistent p0(args.size() > 0 ? args[0] : ev::undefined());
        if (!ev::isFunction(p0.get())) {
            return ev::throwTypeError("onOpenUri: handler must be a function");
        }

        uint64_t hid = 0;
        {
            std::lock_guard lock(g_uri_mu);
            hid = ++g_next_uri_handler_id;
            g_uri_handlers.push_back(HandlerEntry{
                .id = hid,
                .callback = std::make_shared<ev::Persistent>(p0.get())
            });
        }

#if defined(__linux__)
        auto b = activeBackend();
        if (b) {
            b->set_open_uri_handler(handleNativeOpenUri);
            b->open_uri().set_open_file_callback(handleNativeOpenFile);
        }
#endif

        ObjectBuilder handle;
        handle.set("id", static_cast<double>(hid));
        handle.def("dispose", 0, [hid](Value, std::span<const Value>) -> Value {
            std::lock_guard lock(g_uri_mu);
            g_uri_handlers.erase(
                std::remove_if(g_uri_handlers.begin(), g_uri_handlers.end(),
                               [hid](const HandlerEntry& e) { return e.id == hid; }),
                g_uri_handlers.end());
            return ev::fromBool(true);
        });
        handle.def("unregister", 0, [hid](Value, std::span<const Value>) -> Value {
            std::lock_guard lock(g_uri_mu);
            g_uri_handlers.erase(
                std::remove_if(g_uri_handlers.begin(), g_uri_handlers.end(),
                               [hid](const HandlerEntry& e) { return e.id == hid; }),
                g_uri_handlers.end());
            return ev::fromBool(true);
        });

        return handle.get();
    });

    // bro.portal.offOpenUri(handlerOrHandle) -> boolean
    portal.def("offOpenUri", 1, [](Value, std::span<const Value> args) -> Value {
        ev::Persistent p0(args.size() > 0 ? args[0] : ev::undefined());
        std::lock_guard lock(g_uri_mu);
        if (ev::isFunction(p0.get())) {
            g_uri_handlers.erase(
                std::remove_if(g_uri_handlers.begin(), g_uri_handlers.end(),
                               [&](const HandlerEntry& e) {
                                   return e.callback && e.callback->get() == p0.get();
                               }),
                g_uri_handlers.end());
            return ev::fromBool(true);
        }
        if (ev::isObject(p0.get())) {
            ev::Persistent pId(ev::getProperty(p0.get(), "id"));
            if (ev::isNumber(pId.get())) {
                uint64_t hid = static_cast<uint64_t>(ev::toDouble(pId.get()));
                g_uri_handlers.erase(
                    std::remove_if(g_uri_handlers.begin(), g_uri_handlers.end(),
                                   [hid](const HandlerEntry& e) { return e.id == hid; }),
                    g_uri_handlers.end());
                return ev::fromBool(true);
            }
        }
        return ev::fromBool(false);
    });
}

} // namespace broportal::api
