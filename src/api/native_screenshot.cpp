#include "host_portal_internal.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <vector>

namespace broportal::api {

namespace {

struct QueuedScreenshotRequest {
    uint64_t id = 0;
    ObjectPath handle;
    std::string app_id;
    ScreenshotOptions options;

    std::mutex mu;
    std::condition_variable cv;
    bool answered = false;
    ResponseCode response_code = ResponseCode::OtherError;
    std::string out_uri;
    VariantMap out_results;
};

struct QueuedPickColorRequest {
    uint64_t id = 0;
    ObjectPath handle;
    std::string app_id;
    VariantMap options;

    std::mutex mu;
    std::condition_variable cv;
    bool answered = false;
    ResponseCode response_code = ResponseCode::OtherError;
    RgbColor out_color;
    VariantMap out_results;
};

struct HandlerEntry {
    uint64_t id = 0;
    std::shared_ptr<ev::Persistent> callback;
};

std::mutex g_ss_mu;
uint64_t g_next_ss_handler_id = 1;
uint64_t g_next_ss_req_id = 1;
std::vector<HandlerEntry> g_ss_handlers;
std::vector<std::shared_ptr<QueuedScreenshotRequest>> g_active_ss_requests;
std::vector<std::shared_ptr<QueuedScreenshotRequest>> g_unhandled_ss_requests;

std::mutex g_pc_mu;
uint64_t g_next_pc_handler_id = 1;
uint64_t g_next_pc_req_id = 1;
std::vector<HandlerEntry> g_pc_handlers;
std::vector<std::shared_ptr<QueuedPickColorRequest>> g_active_pc_requests;
std::vector<std::shared_ptr<QueuedPickColorRequest>> g_unhandled_pc_requests;

void fulfillScreenshot(const std::shared_ptr<QueuedScreenshotRequest>& req,
                       ResponseCode code,
                       std::string uri,
                       VariantMap results = {}) {
    std::lock_guard lock(req->mu);
    if (req->answered) return;
    req->response_code = code;
    req->out_uri = std::move(uri);
    req->out_results = std::move(results);
    if (!req->out_uri.empty() && !req->out_results.contains("uri")) {
        req->out_results["uri"] = Variant(req->out_uri);
    }
    req->answered = true;
    req->cv.notify_all();
}

void parseAndFulfillScreenshot(const std::shared_ptr<QueuedScreenshotRequest>& req, Value resVal) {
    ev::Persistent val(resVal);
    if (!ev::isObject(val.get())) return;

    ev::Persistent pCancelled(ev::getProperty(val.get(), "cancelled"));
    if (ev::isBool(pCancelled.get()) && ev::toBool(pCancelled.get())) {
        fulfillScreenshot(req, ResponseCode::Cancelled, "");
        return;
    }

    std::string uri;
    ev::Persistent pUri(ev::getProperty(val.get(), "uri"));
    if (ev::isString(pUri.get())) {
        uri = ev::toUtf8(pUri.get());
    }

    VariantMap results;
    fulfillScreenshot(req, ResponseCode::Success, std::move(uri), std::move(results));
}

Value buildScreenshotRequest(const std::shared_ptr<QueuedScreenshotRequest>& req) {
    ObjectBuilder b;
    b.set("handle", req->handle.path);
    b.set("appId", req->app_id);
    b.set("app_id", req->app_id);

    ObjectBuilder opts;
    opts.set("modal", req->options.modal);
    opts.set("interactive", req->options.interactive);
    opts.set("target", static_cast<double>(req->options.target));
    opts.set("permissionStoreChecked", req->options.permission_store_checked);
    opts.set("permission_store_checked", req->options.permission_store_checked);
    opts.set("extraOptions", variantMapToJs(req->options.extra_options));
    opts.set("extra_options", variantMapToJs(req->options.extra_options));
    b.set("options", opts.get());

    b.def("respond", 1, [req](Value, std::span<const Value> args) -> Value {
        ev::Persistent p0(args.size() > 0 ? args[0] : ev::undefined());
        parseAndFulfillScreenshot(req, p0.get());
        return ev::fromBool(true);
    });

    b.def("cancel", 0, [req](Value, std::span<const Value>) -> Value {
        fulfillScreenshot(req, ResponseCode::Cancelled, "");
        return ev::fromBool(true);
    });

    return b.get();
}

void fulfillPickColor(const std::shared_ptr<QueuedPickColorRequest>& req,
                      ResponseCode code,
                      RgbColor color,
                      VariantMap results = {}) {
    std::lock_guard lock(req->mu);
    if (req->answered) return;
    req->response_code = code;
    req->out_color = color;
    req->out_results = std::move(results);
    if (!req->out_results.contains("color")) {
        req->out_results["color"] = Variant(req->out_color);
    }
    req->answered = true;
    req->cv.notify_all();
}

void parseAndFulfillPickColor(const std::shared_ptr<QueuedPickColorRequest>& req, Value resVal) {
    ev::Persistent val(resVal);
    if (!ev::isObject(val.get())) return;

    ev::Persistent pCancelled(ev::getProperty(val.get(), "cancelled"));
    if (ev::isBool(pCancelled.get()) && ev::toBool(pCancelled.get())) {
        fulfillPickColor(req, ResponseCode::Cancelled, {});
        return;
    }

    RgbColor color;
    ev::Persistent pColor(ev::getProperty(val.get(), "color"));
    if (ev::isObject(pColor.get())) {
        ev::Persistent pr(ev::getProperty(pColor.get(), "r"));
        ev::Persistent pg(ev::getProperty(pColor.get(), "g"));
        ev::Persistent pb(ev::getProperty(pColor.get(), "b"));
        if (ev::isNumber(pr.get())) color.r = ev::toDouble(pr.get());
        if (ev::isNumber(pg.get())) color.g = ev::toDouble(pg.get());
        if (ev::isNumber(pb.get())) color.b = ev::toDouble(pb.get());
    }

    fulfillPickColor(req, ResponseCode::Success, color, {});
}

Value buildPickColorRequest(const std::shared_ptr<QueuedPickColorRequest>& req) {
    ObjectBuilder b;
    b.set("handle", req->handle.path);
    b.set("appId", req->app_id);
    b.set("app_id", req->app_id);
    b.set("options", variantMapToJs(req->options));

    b.def("respond", 1, [req](Value, std::span<const Value> args) -> Value {
        ev::Persistent p0(args.size() > 0 ? args[0] : ev::undefined());
        parseAndFulfillPickColor(req, p0.get());
        return ev::fromBool(true);
    });

    b.def("cancel", 0, [req](Value, std::span<const Value>) -> Value {
        fulfillPickColor(req, ResponseCode::Cancelled, {});
        return ev::fromBool(true);
    });

    return b.get();
}

} // namespace

ResponseCode handleNativeScreenshot(
    const ObjectPath& handle,
    const std::string& app_id,
    const ScreenshotOptions& options,
    std::string& out_uri,
    VariantMap& out_results) {
    auto req = std::make_shared<QueuedScreenshotRequest>();
    req->handle = handle;
    req->app_id = app_id;
    req->options = options;

    {
        std::lock_guard lock(g_ss_mu);
        if (g_ss_handlers.empty()) {
            return ResponseCode::OtherError;
        }
        req->id = ++g_next_ss_req_id;
        g_active_ss_requests.push_back(req);
        g_unhandled_ss_requests.push_back(req);
    }

    std::unique_lock ulock(req->mu);
    bool ok = req->cv.wait_for(ulock, std::chrono::seconds(60), [&]() {
        return req->answered;
    });

    {
        std::lock_guard lock(g_ss_mu);
        for (auto it = g_active_ss_requests.begin(); it != g_active_ss_requests.end(); ++it) {
            if ((*it)->id == req->id) {
                g_active_ss_requests.erase(it);
                break;
            }
        }
    }

    if (!ok || !req->answered) {
        return ResponseCode::OtherError;
    }

    out_uri = std::move(req->out_uri);
    out_results = std::move(req->out_results);
    return req->response_code;
}

ResponseCode handleNativePickColor(
    const ObjectPath& handle,
    const std::string& app_id,
    const VariantMap& options,
    RgbColor& out_color,
    VariantMap& out_results) {
    auto req = std::make_shared<QueuedPickColorRequest>();
    req->handle = handle;
    req->app_id = app_id;
    req->options = options;

    {
        std::lock_guard lock(g_pc_mu);
        if (g_pc_handlers.empty()) {
            return ResponseCode::OtherError;
        }
        req->id = ++g_next_pc_req_id;
        g_active_pc_requests.push_back(req);
        g_unhandled_pc_requests.push_back(req);
    }

    std::unique_lock ulock(req->mu);
    bool ok = req->cv.wait_for(ulock, std::chrono::seconds(60), [&]() {
        return req->answered;
    });

    {
        std::lock_guard lock(g_pc_mu);
        for (auto it = g_active_pc_requests.begin(); it != g_active_pc_requests.end(); ++it) {
            if ((*it)->id == req->id) {
                g_active_pc_requests.erase(it);
                break;
            }
        }
    }

    if (!ok || !req->answered) {
        return ResponseCode::OtherError;
    }

    out_color = req->out_color;
    out_results = std::move(req->out_results);
    return req->response_code;
}

void drainScreenshot() {
    // Screenshot
    {
        std::vector<std::shared_ptr<QueuedScreenshotRequest>> toDispatch;
        std::vector<std::shared_ptr<ev::Persistent>> callbacks;
        {
            std::lock_guard lock(g_ss_mu);
            toDispatch.swap(g_unhandled_ss_requests);
            for (const auto& entry : g_ss_handlers) {
                if (entry.callback) callbacks.push_back(entry.callback);
            }
        }

        if (!toDispatch.empty() && !callbacks.empty()) {
            for (const auto& req : toDispatch) {
                ev::Persistent reqObj(buildScreenshotRequest(req));
                for (const auto& cb : callbacks) {
                    if (!cb || !ev::isFunction(cb->get())) continue;
                    const Value arg = reqObj.get();
                    ev::Persistent retVal(ev::call(cb->get(), ev::undefined(), std::span<const Value>(&arg, 1)).value);

                    if (ev::isPromise(retVal.get())) {
                        attachPromiseReaction(
                            retVal.get(),
                            [req](Value val) { parseAndFulfillScreenshot(req, val); },
                            [req](Value) { fulfillScreenshot(req, ResponseCode::Cancelled, ""); });
                    } else if (ev::isObject(retVal.get())) {
                        parseAndFulfillScreenshot(req, retVal.get());
                    }
                }
            }
        }
    }

    // PickColor
    {
        std::vector<std::shared_ptr<QueuedPickColorRequest>> toDispatch;
        std::vector<std::shared_ptr<ev::Persistent>> callbacks;
        {
            std::lock_guard lock(g_pc_mu);
            toDispatch.swap(g_unhandled_pc_requests);
            for (const auto& entry : g_pc_handlers) {
                if (entry.callback) callbacks.push_back(entry.callback);
            }
        }

        if (!toDispatch.empty() && !callbacks.empty()) {
            for (const auto& req : toDispatch) {
                ev::Persistent reqObj(buildPickColorRequest(req));
                for (const auto& cb : callbacks) {
                    if (!cb || !ev::isFunction(cb->get())) continue;
                    const Value arg = reqObj.get();
                    ev::Persistent retVal(ev::call(cb->get(), ev::undefined(), std::span<const Value>(&arg, 1)).value);

                    if (ev::isPromise(retVal.get())) {
                        attachPromiseReaction(
                            retVal.get(),
                            [req](Value val) { parseAndFulfillPickColor(req, val); },
                            [req](Value) { fulfillPickColor(req, ResponseCode::Cancelled, {}); });
                    } else if (ev::isObject(retVal.get())) {
                        parseAndFulfillPickColor(req, retVal.get());
                    }
                }
            }
        }
    }
}

void shutdownScreenshot() {
    {
        std::vector<std::shared_ptr<QueuedScreenshotRequest>> toCancel;
        {
            std::lock_guard lock(g_ss_mu);
            toCancel.swap(g_active_ss_requests);
            g_unhandled_ss_requests.clear();
            g_ss_handlers.clear();
        }
        for (const auto& req : toCancel) {
            std::lock_guard lock(req->mu);
            req->response_code = ResponseCode::Cancelled;
            req->answered = true;
            req->cv.notify_all();
        }
    }

    {
        std::vector<std::shared_ptr<QueuedPickColorRequest>> toCancel;
        {
            std::lock_guard lock(g_pc_mu);
            toCancel.swap(g_active_pc_requests);
            g_unhandled_pc_requests.clear();
            g_pc_handlers.clear();
        }
        for (const auto& req : toCancel) {
            std::lock_guard lock(req->mu);
            req->response_code = ResponseCode::Cancelled;
            req->answered = true;
            req->cv.notify_all();
        }
    }
}

void installScreenshotOnto(Value portalObj) {
    ObjectBuilder portal(portalObj);

    // bro.portal.onScreenshot(handler) -> HandlerHandle
    portal.def("onScreenshot", 1, [](Value, std::span<const Value> args) -> Value {
        ev::Persistent p0(args.size() > 0 ? args[0] : ev::undefined());
        if (!ev::isFunction(p0.get())) {
            return ev::throwTypeError("onScreenshot: handler must be a function");
        }

        uint64_t hid = 0;
        {
            std::lock_guard lock(g_ss_mu);
            hid = ++g_next_ss_handler_id;
            g_ss_handlers.push_back(HandlerEntry{
                .id = hid,
                .callback = std::make_shared<ev::Persistent>(p0.get())
            });
        }

#if defined(__linux__)
        auto b = activeBackend();
        if (b) {
            b->set_screenshot_handler(handleNativeScreenshot);
        }
#endif

        ObjectBuilder handle;
        handle.set("id", static_cast<double>(hid));
        handle.def("dispose", 0, [hid](Value, std::span<const Value>) -> Value {
            std::lock_guard lock(g_ss_mu);
            g_ss_handlers.erase(
                std::remove_if(g_ss_handlers.begin(), g_ss_handlers.end(),
                               [hid](const HandlerEntry& e) { return e.id == hid; }),
                g_ss_handlers.end());
            return ev::fromBool(true);
        });
        handle.def("unregister", 0, [hid](Value, std::span<const Value>) -> Value {
            std::lock_guard lock(g_ss_mu);
            g_ss_handlers.erase(
                std::remove_if(g_ss_handlers.begin(), g_ss_handlers.end(),
                               [hid](const HandlerEntry& e) { return e.id == hid; }),
                g_ss_handlers.end());
            return ev::fromBool(true);
        });

        return handle.get();
    });

    // bro.portal.offScreenshot(handlerOrHandle) -> boolean
    portal.def("offScreenshot", 1, [](Value, std::span<const Value> args) -> Value {
        ev::Persistent p0(args.size() > 0 ? args[0] : ev::undefined());
        std::lock_guard lock(g_ss_mu);
        if (ev::isFunction(p0.get())) {
            g_ss_handlers.erase(
                std::remove_if(g_ss_handlers.begin(), g_ss_handlers.end(),
                               [&](const HandlerEntry& e) {
                                   return e.callback && e.callback->get() == p0.get();
                               }),
                g_ss_handlers.end());
            return ev::fromBool(true);
        }
        if (ev::isObject(p0.get())) {
            ev::Persistent pId(ev::getProperty(p0.get(), "id"));
            if (ev::isNumber(pId.get())) {
                uint64_t hid = static_cast<uint64_t>(ev::toDouble(pId.get()));
                g_ss_handlers.erase(
                    std::remove_if(g_ss_handlers.begin(), g_ss_handlers.end(),
                                   [hid](const HandlerEntry& e) { return e.id == hid; }),
                    g_ss_handlers.end());
                return ev::fromBool(true);
            }
        }
        return ev::fromBool(false);
    });

    // bro.portal.onPickColor(handler) -> HandlerHandle
    portal.def("onPickColor", 1, [](Value, std::span<const Value> args) -> Value {
        ev::Persistent p0(args.size() > 0 ? args[0] : ev::undefined());
        if (!ev::isFunction(p0.get())) {
            return ev::throwTypeError("onPickColor: handler must be a function");
        }

        uint64_t hid = 0;
        {
            std::lock_guard lock(g_pc_mu);
            hid = ++g_next_pc_handler_id;
            g_pc_handlers.push_back(HandlerEntry{
                .id = hid,
                .callback = std::make_shared<ev::Persistent>(p0.get())
            });
        }

#if defined(__linux__)
        auto b = activeBackend();
        if (b) {
            b->screenshot().set_pick_color_callback(handleNativePickColor);
        }
#endif

        ObjectBuilder handle;
        handle.set("id", static_cast<double>(hid));
        handle.def("dispose", 0, [hid](Value, std::span<const Value>) -> Value {
            std::lock_guard lock(g_pc_mu);
            g_pc_handlers.erase(
                std::remove_if(g_pc_handlers.begin(), g_pc_handlers.end(),
                               [hid](const HandlerEntry& e) { return e.id == hid; }),
                g_pc_handlers.end());
            return ev::fromBool(true);
        });
        handle.def("unregister", 0, [hid](Value, std::span<const Value>) -> Value {
            std::lock_guard lock(g_pc_mu);
            g_pc_handlers.erase(
                std::remove_if(g_pc_handlers.begin(), g_pc_handlers.end(),
                               [hid](const HandlerEntry& e) { return e.id == hid; }),
                g_pc_handlers.end());
            return ev::fromBool(true);
        });

        return handle.get();
    });

    // bro.portal.offPickColor(handlerOrHandle) -> boolean
    portal.def("offPickColor", 1, [](Value, std::span<const Value> args) -> Value {
        ev::Persistent p0(args.size() > 0 ? args[0] : ev::undefined());
        std::lock_guard lock(g_pc_mu);
        if (ev::isFunction(p0.get())) {
            g_pc_handlers.erase(
                std::remove_if(g_pc_handlers.begin(), g_pc_handlers.end(),
                               [&](const HandlerEntry& e) {
                                   return e.callback && e.callback->get() == p0.get();
                               }),
                g_pc_handlers.end());
            return ev::fromBool(true);
        }
        if (ev::isObject(p0.get())) {
            ev::Persistent pId(ev::getProperty(p0.get(), "id"));
            if (ev::isNumber(pId.get())) {
                uint64_t hid = static_cast<uint64_t>(ev::toDouble(pId.get()));
                g_pc_handlers.erase(
                    std::remove_if(g_pc_handlers.begin(), g_pc_handlers.end(),
                                   [hid](const HandlerEntry& e) { return e.id == hid; }),
                    g_pc_handlers.end());
                return ev::fromBool(true);
            }
        }
        return ev::fromBool(false);
    });
}

} // namespace broportal::api
