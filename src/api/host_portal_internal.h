#pragma once

#include "embed/embed.h"
#include "arg_reader.h"
#include "object_builder.h"
#include "host_class.h"
#include "broportal/types.h"
#include "broportal/availability.h"
#include "broportal/event_queue.h"

#if defined(__linux__)
#include "broportal/backend.h"
#endif

#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace broportal {
#if !defined(__linux__)
class PortalBackend;
#endif
}

namespace broportal::api {

namespace ev = bronze::embed;
using Value = bronze::Value;

Value ensureBroPortal();

#if defined(__linux__)
std::shared_ptr<broportal::PortalBackend> activeBackend();
void attachBackendHandlers(broportal::PortalBackend& backend);

ResponseCode handleNativeFilePicker(
    const ObjectPath& handle,
    const std::string& app_id,
    const std::string& title,
    const FileChooserOptions& options,
    std::vector<std::string>& out_uris,
    VariantMap& out_results);

ResponseCode handleNativeScreenshot(
    const ObjectPath& handle,
    const std::string& app_id,
    const ScreenshotOptions& options,
    std::string& out_uri,
    VariantMap& out_results);

ResponseCode handleNativePickColor(
    const ObjectPath& handle,
    const std::string& app_id,
    const VariantMap& options,
    RgbColor& out_color,
    VariantMap& out_results);

ResponseCode handleNativeScreenCast(
    const ObjectPath& handle,
    const ObjectPath& session_handle,
    const std::string& app_id,
    const ScreenCastSourceOptions& options,
    StreamList& out_streams,
    VariantMap& out_results);

ResponseCode handleNativeOpenUri(
    const ObjectPath& handle,
    const std::string& app_id,
    const std::string& uri,
    const VariantMap& options,
    VariantMap& out_results);

ResponseCode handleNativeOpenFile(
    const ObjectPath& handle,
    const std::string& app_id,
    int fd,
    const VariantMap& options,
    VariantMap& out_results);
#endif

void installFileChooserOnto(Value portalObj);
void installScreenshotOnto(Value portalObj);
void installScreenCastOnto(Value portalObj);
void installOpenUriOnto(Value portalObj);

void drainFileChooser();
void drainScreenshot();
void drainScreenCast();
void drainOpenUri();

void shutdownFileChooser();
void shutdownScreenshot();
void shutdownScreenCast();
void shutdownOpenUri();

inline Value variantToJs(const Variant& var) {
    if (var.is_null()) return ev::null();
    if (const auto* b = var.get_if<bool>()) return ev::fromBool(*b);
    if (const auto* u8 = var.get_if<uint8_t>()) return ev::fromDouble(*u8);
    if (const auto* i16 = var.get_if<int16_t>()) return ev::fromDouble(*i16);
    if (const auto* u16 = var.get_if<uint16_t>()) return ev::fromDouble(*u16);
    if (const auto* i32 = var.get_if<int32_t>()) return ev::fromDouble(*i32);
    if (const auto* u32 = var.get_if<uint32_t>()) return ev::fromDouble(*u32);
    if (const auto* i64 = var.get_if<int64_t>()) return ev::fromDouble(static_cast<double>(*i64));
    if (const auto* u64 = var.get_if<uint64_t>()) return ev::fromDouble(static_cast<double>(*u64));
    if (const auto* d = var.get_if<double>()) return ev::fromDouble(*d);
    if (const auto* s = var.get_if<std::string>()) return ev::fromUtf8(*s);
    if (const auto* p = var.get_if<ObjectPath>()) return ev::fromUtf8(p->path);
    if (const auto* fd = var.get_if<UnixFd>()) return ev::fromDouble(fd->fd);
    if (const auto* vs = var.get_if<std::vector<std::string>>()) {
        ev::Persistent arr(ev::makeArray(static_cast<uint32_t>(vs->size())));
        for (uint32_t i = 0; i < vs->size(); ++i) {
            ev::Persistent elem(ev::fromUtf8((*vs)[i]));
            arr.set(ev::setElement(arr.get(), i, elem.get()));
        }
        return arr.get();
    }
    if (const auto* vb = var.get_if<std::vector<uint8_t>>()) {
        ev::Persistent arr(ev::makeArray(static_cast<uint32_t>(vb->size())));
        for (uint32_t i = 0; i < vb->size(); ++i) {
            arr.set(ev::setElement(arr.get(), i, ev::fromDouble((*vb)[i])));
        }
        return arr.get();
    }
    if (const auto* c = var.get_if<RgbColor>()) {
        ObjectBuilder col;
        col.set("r", c->r);
        col.set("g", c->g);
        col.set("b", c->b);
        return col.get();
    }
    if (const auto* m = var.get_if<VariantMap>()) {
        ObjectBuilder obj;
        for (const auto& [k, v] : *m) {
            obj.set(k, variantToJs(v));
        }
        return obj.get();
    }
    return ev::undefined();
}

inline Value variantMapToJs(const VariantMap& map) {
    ObjectBuilder obj;
    for (const auto& [k, v] : map) {
        obj.set(k, variantToJs(v));
    }
    return obj.get();
}

inline void attachPromiseReaction(Value promiseVal,
                                  std::function<void(Value)> onFulfilled,
                                  std::function<void(Value)> onRejected) {
    ev::Persistent promiseP(promiseVal);
    if (!ev::isPromise(promiseP.get())) return;
    ev::Persistent thenFn(ev::getProperty(promiseP.get(), "then"));
    if (!ev::isFunction(thenFn.get())) return;

    auto fulfilledCb = [onFulfilled = std::move(onFulfilled)](Value, std::span<const Value> args) -> Value {
        ev::Persistent val(args.size() > 0 ? args[0] : ev::undefined());
        if (onFulfilled) onFulfilled(val.get());
        return ev::undefined();
    };
    auto rejectedCb = [onRejected = std::move(onRejected)](Value, std::span<const Value> args) -> Value {
        ev::Persistent err(args.size() > 0 ? args[0] : ev::undefined());
        if (onRejected) onRejected(err.get());
        return ev::undefined();
    };

    ev::Persistent fn1(ev::makeFunction(std::move(fulfilledCb), 1, "onFulfilled"));
    ev::Persistent fn2(ev::makeFunction(std::move(rejectedCb), 1, "onRejected"));
    const Value args[2] = {fn1.get(), fn2.get()};
    ev::call(thenFn.get(), promiseP.get(), std::span<const Value>(args, 2));
}

} // namespace broportal::api
