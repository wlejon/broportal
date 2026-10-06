#include "host_portal_internal.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <vector>

namespace broportal::api {

namespace {

struct QueuedFileChooserRequest {
    uint64_t id = 0;
    ObjectPath handle;
    std::string app_id;
    std::string title;
    FileChooserOptions options;

    std::mutex mu;
    std::condition_variable cv;
    bool answered = false;
    ResponseCode response_code = ResponseCode::OtherError;
    std::vector<std::string> out_uris;
    VariantMap out_results;
};

struct HandlerEntry {
    uint64_t id = 0;
    std::shared_ptr<ev::Persistent> callback;
};

std::mutex g_fc_mu;
uint64_t g_next_handler_id = 1;
uint64_t g_next_fc_req_id = 1;
std::vector<HandlerEntry> g_fc_handlers;
std::vector<std::shared_ptr<QueuedFileChooserRequest>> g_active_fc_requests;
std::vector<std::shared_ptr<QueuedFileChooserRequest>> g_unhandled_fc_requests;

void fulfillFileChooser(const std::shared_ptr<QueuedFileChooserRequest>& req,
                        ResponseCode code,
                        std::vector<std::string> uris,
                        VariantMap results = {}) {
    std::lock_guard lock(req->mu);
    if (req->answered) return;
    req->response_code = code;
    req->out_uris = std::move(uris);
    req->out_results = std::move(results);
    if (!req->out_uris.empty() && !req->out_results.contains("uris")) {
        req->out_results["uris"] = Variant(req->out_uris);
    }
    req->answered = true;
    req->cv.notify_all();
}

void parseAndFulfill(const std::shared_ptr<QueuedFileChooserRequest>& req, Value resVal) {
    ev::Persistent val(resVal);
    if (!ev::isObject(val.get())) return;

    ev::Persistent pCancelled(ev::getProperty(val.get(), "cancelled"));
    if (ev::isBool(pCancelled.get()) && ev::toBool(pCancelled.get())) {
        fulfillFileChooser(req, ResponseCode::Cancelled, {});
        return;
    }

    std::vector<std::string> uris;
    ev::Persistent pUris(ev::getProperty(val.get(), "uris"));
    if (ev::isObject(pUris.get())) {
        ev::Persistent pLen(ev::getProperty(pUris.get(), "length"));
        if (ev::isNumber(pLen.get())) {
            uint32_t len = static_cast<uint32_t>(ev::toDouble(pLen.get()));
            for (uint32_t i = 0; i < len; ++i) {
                ev::Persistent elem(ev::getElement(pUris.get(), i));
                if (ev::isString(elem.get())) {
                    uris.push_back(ev::toUtf8(elem.get()));
                }
            }
        }
    } else if (ev::isString(pUris.get())) {
        uris.push_back(ev::toUtf8(pUris.get()));
    }

    VariantMap results;
    ev::Persistent pResults(ev::getProperty(val.get(), "results"));
    if (ev::isObject(pResults.get())) {
        // Can read custom result map if provided
    }

    fulfillFileChooser(req, ResponseCode::Success, std::move(uris), std::move(results));
}

Value buildFileChooserRequest(const std::shared_ptr<QueuedFileChooserRequest>& req) {
    ObjectBuilder b;
    b.set("handle", req->handle.path);
    b.set("appId", req->app_id);
    b.set("app_id", req->app_id);
    b.set("title", req->title);

    ObjectBuilder opts;
    opts.set("multiple", req->options.multiple);
    opts.set("directory", req->options.directory);
    opts.set("modal", req->options.modal);
    opts.set("acceptLabel", req->options.accept_label);
    opts.set("accept_label", req->options.accept_label);
    opts.set("currentName", req->options.current_name);
    opts.set("current_name", req->options.current_name);
    opts.set("currentFolder", req->options.current_folder);
    opts.set("current_folder", req->options.current_folder);
    opts.set("currentFile", req->options.current_file);
    opts.set("current_file", req->options.current_file);
    opts.set("extraOptions", variantMapToJs(req->options.extra_options));
    opts.set("extra_options", variantMapToJs(req->options.extra_options));
    b.set("options", opts.get());

    // req.respond({ uris: string[], cancelled?: boolean })
    b.def("respond", 1, [req](Value, std::span<const Value> args) -> Value {
        ev::Persistent p0(args.size() > 0 ? args[0] : ev::undefined());
        parseAndFulfill(req, p0.get());
        return ev::fromBool(true);
    });

    // req.cancel([reason])
    b.def("cancel", 0, [req](Value, std::span<const Value>) -> Value {
        fulfillFileChooser(req, ResponseCode::Cancelled, {});
        return ev::fromBool(true);
    });

    return b.get();
}

} // namespace

ResponseCode handleNativeFilePicker(
    const ObjectPath& handle,
    const std::string& app_id,
    const std::string& title,
    const FileChooserOptions& options,
    std::vector<std::string>& out_uris,
    VariantMap& out_results) {
    auto req = std::make_shared<QueuedFileChooserRequest>();
    req->handle = handle;
    req->app_id = app_id;
    req->title = title;
    req->options = options;

    {
        std::lock_guard lock(g_fc_mu);
        if (g_fc_handlers.empty()) {
            return ResponseCode::OtherError;
        }
        req->id = ++g_next_fc_req_id;
        g_active_fc_requests.push_back(req);
        g_unhandled_fc_requests.push_back(req);
    }

    std::unique_lock ulock(req->mu);
    bool ok = req->cv.wait_for(ulock, std::chrono::seconds(60), [&]() {
        return req->answered;
    });

    {
        std::lock_guard lock(g_fc_mu);
        for (auto it = g_active_fc_requests.begin(); it != g_active_fc_requests.end(); ++it) {
            if ((*it)->id == req->id) {
                g_active_fc_requests.erase(it);
                break;
            }
        }
    }

    if (!ok || !req->answered) {
        return ResponseCode::OtherError;
    }

    out_uris = std::move(req->out_uris);
    out_results = std::move(req->out_results);
    return req->response_code;
}

void drainFileChooser() {
    std::vector<std::shared_ptr<QueuedFileChooserRequest>> toDispatch;
    std::vector<std::shared_ptr<ev::Persistent>> callbacks;
    {
        std::lock_guard lock(g_fc_mu);
        toDispatch.swap(g_unhandled_fc_requests);
        for (const auto& entry : g_fc_handlers) {
            if (entry.callback) callbacks.push_back(entry.callback);
        }
    }

    if (toDispatch.empty() || callbacks.empty()) return;

    for (const auto& req : toDispatch) {
        ev::Persistent reqObj(buildFileChooserRequest(req));
        for (const auto& cb : callbacks) {
            if (!cb || !ev::isFunction(cb->get())) continue;
            const Value arg = reqObj.get();
            ev::Persistent retVal(ev::call(cb->get(), ev::undefined(), std::span<const Value>(&arg, 1)).value);

            if (ev::isPromise(retVal.get())) {
                attachPromiseReaction(
                    retVal.get(),
                    [req](Value val) { parseAndFulfill(req, val); },
                    [req](Value) { fulfillFileChooser(req, ResponseCode::Cancelled, {}); });
            } else if (ev::isObject(retVal.get())) {
                parseAndFulfill(req, retVal.get());
            }
        }
    }
}

void shutdownFileChooser() {
    std::vector<std::shared_ptr<QueuedFileChooserRequest>> toCancel;
    {
        std::lock_guard lock(g_fc_mu);
        toCancel.swap(g_active_fc_requests);
        g_unhandled_fc_requests.clear();
        g_fc_handlers.clear();
    }

    for (const auto& req : toCancel) {
        std::lock_guard lock(req->mu);
        req->response_code = ResponseCode::Cancelled;
        req->answered = true;
        req->cv.notify_all();
    }
}

void installFileChooserOnto(Value portalObj) {
    ObjectBuilder portal(portalObj);

    // bro.portal.onFileChooser(handler) -> HandlerHandle
    portal.def("onFileChooser", 1, [](Value, std::span<const Value> args) -> Value {
        ev::Persistent p0(args.size() > 0 ? args[0] : ev::undefined());
        if (!ev::isFunction(p0.get())) {
            return ev::throwTypeError("onFileChooser: handler must be a function");
        }

        uint64_t hid = 0;
        {
            std::lock_guard lock(g_fc_mu);
            hid = ++g_next_handler_id;
            g_fc_handlers.push_back(HandlerEntry{
                .id = hid,
                .callback = std::make_shared<ev::Persistent>(p0.get())
            });
        }

#if defined(__linux__)
        auto b = activeBackend();
        if (b) {
            b->set_file_chooser_handler(handleNativeFilePicker);
        }
#endif

        ObjectBuilder handle;
        handle.set("id", static_cast<double>(hid));
        handle.def("dispose", 0, [hid](Value, std::span<const Value>) -> Value {
            std::lock_guard lock(g_fc_mu);
            g_fc_handlers.erase(
                std::remove_if(g_fc_handlers.begin(), g_fc_handlers.end(),
                               [hid](const HandlerEntry& e) { return e.id == hid; }),
                g_fc_handlers.end());
            return ev::fromBool(true);
        });
        handle.def("unregister", 0, [hid](Value, std::span<const Value>) -> Value {
            std::lock_guard lock(g_fc_mu);
            g_fc_handlers.erase(
                std::remove_if(g_fc_handlers.begin(), g_fc_handlers.end(),
                               [hid](const HandlerEntry& e) { return e.id == hid; }),
                g_fc_handlers.end());
            return ev::fromBool(true);
        });

        return handle.get();
    });

    // bro.portal.offFileChooser(handlerOrHandle) -> boolean
    portal.def("offFileChooser", 1, [](Value, std::span<const Value> args) -> Value {
        ev::Persistent p0(args.size() > 0 ? args[0] : ev::undefined());
        std::lock_guard lock(g_fc_mu);
        if (ev::isFunction(p0.get())) {
            g_fc_handlers.erase(
                std::remove_if(g_fc_handlers.begin(), g_fc_handlers.end(),
                               [&](const HandlerEntry& e) {
                                   return e.callback && e.callback->get() == p0.get();
                               }),
                g_fc_handlers.end());
            return ev::fromBool(true);
        }
        if (ev::isObject(p0.get())) {
            ev::Persistent pId(ev::getProperty(p0.get(), "id"));
            if (ev::isNumber(pId.get())) {
                uint64_t hid = static_cast<uint64_t>(ev::toDouble(pId.get()));
                g_fc_handlers.erase(
                    std::remove_if(g_fc_handlers.begin(), g_fc_handlers.end(),
                                   [hid](const HandlerEntry& e) { return e.id == hid; }),
                    g_fc_handlers.end());
                return ev::fromBool(true);
            }
        }
        return ev::fromBool(false);
    });
}

} // namespace broportal::api
