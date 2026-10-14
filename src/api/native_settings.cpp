#include "host_portal_internal.h"

#include <algorithm>
#include <cmath>
#include <mutex>
#include <set>
#include <vector>

namespace broportal::api {

namespace {

struct QueuedSettingEvent {
    std::string ns;
    std::string key;
    Variant value;
};

struct HandlerEntry {
    uint64_t id = 0;
    std::shared_ptr<ev::Persistent> callback;
};

std::mutex g_set_mu;
uint64_t g_next_set_handler_id = 1;
std::vector<HandlerEntry> g_set_handlers;
std::vector<QueuedSettingEvent> g_unhandled_set_events;
std::set<const broportal::PortalBackend*> g_observed_backends;

bool parseColorScheme(Value val, uint32_t* out_scheme) {
    ev::Persistent p(val);
    if (ev::isNumber(p.get())) {
        double d = ev::toDouble(p.get());
        if (d >= 0.0 && d <= 2.0 && std::floor(d) == d) {
            *out_scheme = static_cast<uint32_t>(d);
            return true;
        }
        return false;
    }
    if (ev::isString(p.get())) {
        std::string s = ev::toUtf8(p.get());
        if (s == "default" || s == "none" || s == "no-preference" || s == "0") {
            *out_scheme = 0;
            return true;
        }
        if (s == "dark" || s == "prefer-dark" || s == "1") {
            *out_scheme = 1;
            return true;
        }
        if (s == "light" || s == "prefer-light" || s == "2") {
            *out_scheme = 2;
            return true;
        }
    }
    return false;
}

bool parseContrast(Value val, uint32_t* out_contrast) {
    ev::Persistent p(val);
    if (ev::isBool(p.get())) {
        *out_contrast = ev::toBool(p.get()) ? 1u : 0u;
        return true;
    }
    if (ev::isNumber(p.get())) {
        double d = ev::toDouble(p.get());
        if ((d == 0.0 || d == 1.0)) {
            *out_contrast = static_cast<uint32_t>(d);
            return true;
        }
        return false;
    }
    if (ev::isString(p.get())) {
        std::string s = ev::toUtf8(p.get());
        if (s == "normal" || s == "default" || s == "none" || s == "no-preference" || s == "0") {
            *out_contrast = 0;
            return true;
        }
        if (s == "high" || s == "more" || s == "high-contrast" || s == "1") {
            *out_contrast = 1;
            return true;
        }
    }
    return false;
}

bool parseReducedMotion(Value val, uint32_t* out_motion) {
    ev::Persistent p(val);
    if (ev::isBool(p.get())) {
        *out_motion = ev::toBool(p.get()) ? 1u : 0u;
        return true;
    }
    if (ev::isNumber(p.get())) {
        double d = ev::toDouble(p.get());
        if (d == 0.0 || d == 1.0) {
            *out_motion = static_cast<uint32_t>(d);
            return true;
        }
        return false;
    }
    if (ev::isString(p.get())) {
        std::string s = ev::toUtf8(p.get());
        if (s == "normal" || s == "default" || s == "none" || s == "no-preference" || s == "0") {
            *out_motion = 0;
            return true;
        }
        if (s == "reduced" || s == "reduce" || s == "reduced-motion" || s == "1") {
            *out_motion = 1;
            return true;
        }
    }
    return false;
}

bool parseRgbColorFromValue(Value val, RgbColor* out_color) {
    ev::Persistent p(val);
    if (!ev::isObject(p.get())) return false;

    ev::Persistent pr(ev::getProperty(p.get(), "r"));
    ev::Persistent pg(ev::getProperty(p.get(), "g"));
    ev::Persistent pb(ev::getProperty(p.get(), "b"));
    if (ev::isNumber(pr.get()) && ev::isNumber(pg.get()) && ev::isNumber(pb.get())) {
        out_color->r = ev::toDouble(pr.get());
        out_color->g = ev::toDouble(pg.get());
        out_color->b = ev::toDouble(pb.get());
        return true;
    }

    ev::Persistent pLen(ev::getProperty(p.get(), "length"));
    if (ev::isNumber(pLen.get()) && ev::toDouble(pLen.get()) >= 3.0) {
        ev::Persistent e0(ev::getElement(p.get(), 0));
        ev::Persistent e1(ev::getElement(p.get(), 1));
        ev::Persistent e2(ev::getElement(p.get(), 2));
        if (ev::isNumber(e0.get()) && ev::isNumber(e1.get()) && ev::isNumber(e2.get())) {
            out_color->r = ev::toDouble(e0.get());
            out_color->g = ev::toDouble(e1.get());
            out_color->b = ev::toDouble(e2.get());
            return true;
        }
    }
    return false;
}

bool jsToVariant(const std::string& ns, const std::string& key, Value val, Variant* out_var) {
    ev::Persistent p(val);
    if (ns == "org.freedesktop.appearance") {
        if (key == "color-scheme") {
            uint32_t s = 0;
            if (!parseColorScheme(p.get(), &s)) return false;
            *out_var = Variant(s);
            return true;
        }
        if (key == "contrast") {
            uint32_t c = 0;
            if (!parseContrast(p.get(), &c)) return false;
            *out_var = Variant(c);
            return true;
        }
        if (key == "reduced-motion") {
            uint32_t m = 0;
            if (!parseReducedMotion(p.get(), &m)) return false;
            *out_var = Variant(m);
            return true;
        }
        if (key == "accent-color") {
            RgbColor col;
            if (!parseRgbColorFromValue(p.get(), &col)) return false;
            *out_var = Variant(col);
            return true;
        }
    }

    if (ev::isBool(p.get())) {
        *out_var = Variant(ev::toBool(p.get()));
        return true;
    }
    if (ev::isNumber(p.get())) {
        double d = ev::toDouble(p.get());
        if (std::floor(d) == d && d >= 0.0 && d <= 4294967295.0) {
            *out_var = Variant(static_cast<uint32_t>(d));
        } else if (std::floor(d) == d && d >= -2147483648.0 && d <= 2147483647.0) {
            *out_var = Variant(static_cast<int32_t>(d));
        } else {
            *out_var = Variant(d);
        }
        return true;
    }
    if (ev::isString(p.get())) {
        *out_var = Variant(ev::toUtf8(p.get()));
        return true;
    }
    if (ev::isObject(p.get())) {
        RgbColor col;
        if (parseRgbColorFromValue(p.get(), &col)) {
            *out_var = Variant(col);
            return true;
        }
        ev::Persistent pLen(ev::getProperty(p.get(), "length"));
        if (ev::isNumber(pLen.get())) {
            uint32_t len = static_cast<uint32_t>(ev::toDouble(pLen.get()));
            std::vector<std::string> strs;
            for (uint32_t i = 0; i < len; ++i) {
                ev::Persistent elem(ev::getElement(p.get(), i));
                if (ev::isString(elem.get())) {
                    strs.push_back(ev::toUtf8(elem.get()));
                }
            }
            *out_var = Variant(std::move(strs));
            return true;
        }
    }
    return false;
}

} // namespace

void handleNativeSettingChanged(
    const std::string& ns,
    const std::string& key,
    const Variant& value) {
    std::lock_guard lock(g_set_mu);
    if (g_set_handlers.empty()) return;
    g_unhandled_set_events.push_back(QueuedSettingEvent{ns, key, value});
}

void attachSettingsBackend(broportal::PortalBackend& backend) {
    std::lock_guard lock(g_set_mu);
    if (g_observed_backends.contains(&backend)) return;
    g_observed_backends.insert(&backend);
    backend.settings().add_change_observer(handleNativeSettingChanged);
}

void drainSettings() {
    std::vector<QueuedSettingEvent> toDispatch;
    std::vector<std::shared_ptr<ev::Persistent>> callbacks;
    {
        std::lock_guard lock(g_set_mu);
        toDispatch.swap(g_unhandled_set_events);
        for (const auto& entry : g_set_handlers) {
            if (entry.callback) callbacks.push_back(entry.callback);
        }
    }

    if (toDispatch.empty() || callbacks.empty()) return;

    for (const auto& evItem : toDispatch) {
        ObjectBuilder b;
        b.set("namespace", evItem.ns);
        b.set("ns", evItem.ns);
        b.set("key", evItem.key);
        b.set("value", variantToJs(evItem.value));
        ev::Persistent evObj(b.get());
        ev::Persistent nsVal(ev::fromUtf8(evItem.ns));
        ev::Persistent keyVal(ev::fromUtf8(evItem.key));
        ev::Persistent vVal(variantToJs(evItem.value));

        for (const auto& cb : callbacks) {
            if (!cb || !ev::isFunction(cb->get())) continue;
            const Value callArgs[4] = {evObj.get(), nsVal.get(), keyVal.get(), vVal.get()};
            ev::call(cb->get(), ev::undefined(), std::span<const Value>(callArgs, 4));
        }
    }
}

void shutdownSettings() {
    std::lock_guard lock(g_set_mu);
    g_unhandled_set_events.clear();
    g_set_handlers.clear();
    g_observed_backends.clear();
}

void installSettingsOnto(Value portalObj) {
    ObjectBuilder portal(portalObj);

    // bro.portal.setColorScheme(scheme) -> boolean
    portal.def("setColorScheme", 1, [](Value, std::span<const Value> args) -> Value {
        ev::Persistent p0(args.size() > 0 ? args[0] : ev::undefined());
        uint32_t scheme = 0;
        if (!parseColorScheme(p0.get(), &scheme)) {
            return ev::throwTypeError("setColorScheme: expected 'default'|'dark'|'light' or 0|1|2");
        }
        auto b = activeBackend();
        if (!b) return ev::fromBool(false);
        b->settings().set_color_scheme(scheme);
        return ev::fromBool(true);
    });

    // bro.portal.getColorScheme() -> number
    portal.def("getColorScheme", 0, [](Value, std::span<const Value>) -> Value {
        auto b = activeBackend();
        if (!b) return ev::fromDouble(0);
        auto v = b->settings().get_setting("org.freedesktop.appearance", "color-scheme");
        if (!v.has_value()) return ev::fromDouble(0);
        return variantToJs(*v);
    });

    // bro.portal.setAccentColor([r, g, b] | { r, g, b } | (r, g, b)) -> boolean
    portal.def("setAccentColor", 1, [](Value, std::span<const Value> args) -> Value {
        RgbColor col;
        if (args.size() >= 3 && ev::isNumber(args[0]) && ev::isNumber(args[1]) && ev::isNumber(args[2])) {
            col.r = ev::toDouble(args[0]);
            col.g = ev::toDouble(args[1]);
            col.b = ev::toDouble(args[2]);
        } else if (args.size() >= 1) {
            ev::Persistent p0(args[0]);
            if (!parseRgbColorFromValue(p0.get(), &col)) {
                return ev::throwTypeError("setAccentColor: expected [r, g, b], { r, g, b }, or (r, g, b)");
            }
        } else {
            return ev::throwTypeError("setAccentColor: expected [r, g, b], { r, g, b }, or (r, g, b)");
        }
        auto b = activeBackend();
        if (!b) return ev::fromBool(false);
        b->settings().set_accent_color(col);
        return ev::fromBool(true);
    });

    // bro.portal.getAccentColor() -> [r, g, b] | undefined
    portal.def("getAccentColor", 0, [](Value, std::span<const Value>) -> Value {
        auto b = activeBackend();
        if (!b) return ev::undefined();
        auto v = b->settings().get_setting("org.freedesktop.appearance", "accent-color");
        if (!v.has_value()) return ev::undefined();
        return variantToJs(*v);
    });

    // bro.portal.setContrast(contrast) -> boolean
    portal.def("setContrast", 1, [](Value, std::span<const Value> args) -> Value {
        ev::Persistent p0(args.size() > 0 ? args[0] : ev::undefined());
        uint32_t contrast = 0;
        if (!parseContrast(p0.get(), &contrast)) {
            return ev::throwTypeError("setContrast: expected 'normal'|'high' or 0|1");
        }
        auto b = activeBackend();
        if (!b) return ev::fromBool(false);
        b->settings().set_contrast(contrast);
        return ev::fromBool(true);
    });

    // bro.portal.getContrast() -> number
    portal.def("getContrast", 0, [](Value, std::span<const Value>) -> Value {
        auto b = activeBackend();
        if (!b) return ev::fromDouble(0);
        auto v = b->settings().get_setting("org.freedesktop.appearance", "contrast");
        if (!v.has_value()) return ev::fromDouble(0);
        return variantToJs(*v);
    });

    // bro.portal.setReducedMotion(motion) -> boolean
    portal.def("setReducedMotion", 1, [](Value, std::span<const Value> args) -> Value {
        ev::Persistent p0(args.size() > 0 ? args[0] : ev::undefined());
        uint32_t motion = 0;
        if (!parseReducedMotion(p0.get(), &motion)) {
            return ev::throwTypeError("setReducedMotion: expected 'normal'|'reduced' or 0|1");
        }
        auto b = activeBackend();
        if (!b) return ev::fromBool(false);
        b->settings().set_reduced_motion(motion);
        return ev::fromBool(true);
    });

    // bro.portal.getReducedMotion() -> number
    portal.def("getReducedMotion", 0, [](Value, std::span<const Value>) -> Value {
        auto b = activeBackend();
        if (!b) return ev::fromDouble(0);
        auto v = b->settings().get_setting("org.freedesktop.appearance", "reduced-motion");
        if (!v.has_value()) return ev::fromDouble(0);
        return variantToJs(*v);
    });

    // bro.portal.setSetting(ns, key, value) -> boolean
    portal.def("setSetting", 3, [](Value, std::span<const Value> args) -> Value {
        if (args.size() < 3 || !ev::isString(args[0]) || !ev::isString(args[1])) {
            return ev::throwTypeError("setSetting: expected (namespace, key, value)");
        }
        ev::Persistent pNs(args[0]);
        ev::Persistent pKey(args[1]);
        ev::Persistent pVal(args[2]);
        std::string ns = ev::toUtf8(pNs.get());
        std::string key = ev::toUtf8(pKey.get());
        Variant var;
        if (!jsToVariant(ns, key, pVal.get(), &var)) {
            return ev::throwTypeError("setSetting: unsupported value for " + ns + "." + key);
        }
        auto b = activeBackend();
        if (!b) return ev::fromBool(false);
        b->settings().set_setting(ns, key, var);
        return ev::fromBool(true);
    });

    // bro.portal.getSetting(ns, key) -> any
    portal.def("getSetting", 2, [](Value, std::span<const Value> args) -> Value {
        auto b = activeBackend();
        if (!b) return ev::undefined();
        std::string ns = "org.freedesktop.appearance";
        std::string key;
        if (args.size() == 1 && ev::isString(args[0])) {
            ev::Persistent p0(args[0]);
            key = ev::toUtf8(p0.get());
        } else if (args.size() >= 2 && ev::isString(args[0]) && ev::isString(args[1])) {
            ev::Persistent p0(args[0]);
            ev::Persistent p1(args[1]);
            ns = ev::toUtf8(p0.get());
            key = ev::toUtf8(p1.get());
        } else {
            return ev::throwTypeError("getSetting: expected (namespace, key) or (key)");
        }
        auto v = b->settings().get_setting(ns, key);
        if (!v.has_value()) return ev::undefined();
        return variantToJs(*v);
    });

    // bro.portal.getSettings([namespaces]) -> object
    auto getSettingsFn = [](Value, std::span<const Value> args) -> Value {
        auto b = activeBackend();
        if (!b) return ObjectBuilder{}.get();

        std::vector<std::string> namespaces;
        if (args.size() >= 1 && !ev::isUndefined(args[0]) && !ev::isNull(args[0])) {
            ev::Persistent p0(args[0]);
            if (ev::isString(p0.get())) {
                namespaces.push_back(ev::toUtf8(p0.get()));
            } else if (ev::isObject(p0.get())) {
                ev::Persistent pLen(ev::getProperty(p0.get(), "length"));
                if (ev::isNumber(pLen.get())) {
                    uint32_t len = static_cast<uint32_t>(ev::toDouble(pLen.get()));
                    for (uint32_t i = 0; i < len; ++i) {
                        ev::Persistent elem(ev::getElement(p0.get(), i));
                        if (ev::isString(elem.get())) {
                            namespaces.push_back(ev::toUtf8(elem.get()));
                        }
                    }
                }
            }
        }

        SettingsMap map = b->settings().read_all(namespaces);
        ObjectBuilder outer;
        for (const auto& [ns, dict] : map) {
            ObjectBuilder inner;
            for (const auto& [k, v] : dict) {
                inner.set(k, variantToJs(v));
                if (ns == "org.freedesktop.appearance") {
                    if (k == "color-scheme") inner.set("colorScheme", variantToJs(v));
                    else if (k == "accent-color") inner.set("accentColor", variantToJs(v));
                    else if (k == "reduced-motion") inner.set("reducedMotion", variantToJs(v));
                }
            }
            outer.set(ns, inner.get());
        }
        if (namespaces.empty()) {
            auto itApp = map.find("org.freedesktop.appearance");
            if (itApp != map.end()) {
                for (const auto& [k, v] : itApp->second) {
                    outer.set(k, variantToJs(v));
                    if (k == "color-scheme") outer.set("colorScheme", variantToJs(v));
                    else if (k == "accent-color") outer.set("accentColor", variantToJs(v));
                    else if (k == "reduced-motion") outer.set("reducedMotion", variantToJs(v));
                }
            }
        }
        return outer.get();
    };
    portal.def("getSettings", 0, getSettingsFn);
    portal.def("readAllSettings", 0, getSettingsFn);

    // bro.portal.onSettingChanged(handler) -> HandlerHandle
    portal.def("onSettingChanged", 1, [](Value, std::span<const Value> args) -> Value {
        ev::Persistent p0(args.size() > 0 ? args[0] : ev::undefined());
        if (!ev::isFunction(p0.get())) {
            return ev::throwTypeError("onSettingChanged: handler must be a function");
        }

        uint64_t hid = 0;
        {
            std::lock_guard lock(g_set_mu);
            hid = ++g_next_set_handler_id;
            g_set_handlers.push_back(HandlerEntry{
                .id = hid,
                .callback = std::make_shared<ev::Persistent>(p0.get())
            });
        }

        auto b = activeBackend();
        if (b) {
            attachSettingsBackend(*b);
        }

        ObjectBuilder handle;
        handle.set("id", static_cast<double>(hid));
        auto removeFn = [hid](Value, std::span<const Value>) -> Value {
            std::lock_guard lock(g_set_mu);
            g_set_handlers.erase(
                std::remove_if(g_set_handlers.begin(), g_set_handlers.end(),
                               [hid](const HandlerEntry& e) { return e.id == hid; }),
                g_set_handlers.end());
            return ev::fromBool(true);
        };
        handle.def("dispose", 0, removeFn);
        handle.def("unregister", 0, removeFn);
        return handle.get();
    });

    // bro.portal.offSettingChanged(handlerOrHandle) -> boolean
    portal.def("offSettingChanged", 1, [](Value, std::span<const Value> args) -> Value {
        ev::Persistent p0(args.size() > 0 ? args[0] : ev::undefined());
        std::lock_guard lock(g_set_mu);
        if (ev::isFunction(p0.get())) {
            g_set_handlers.erase(
                std::remove_if(g_set_handlers.begin(), g_set_handlers.end(),
                               [&](const HandlerEntry& e) {
                                   return e.callback && e.callback->get() == p0.get();
                               }),
                g_set_handlers.end());
            return ev::fromBool(true);
        }
        if (ev::isObject(p0.get())) {
            ev::Persistent pId(ev::getProperty(p0.get(), "id"));
            if (ev::isNumber(pId.get())) {
                uint64_t hid = static_cast<uint64_t>(ev::toDouble(pId.get()));
                g_set_handlers.erase(
                    std::remove_if(g_set_handlers.begin(), g_set_handlers.end(),
                                   [hid](const HandlerEntry& e) { return e.id == hid; }),
                    g_set_handlers.end());
                return ev::fromBool(true);
            }
        }
        return ev::fromBool(false);
    });
}

} // namespace broportal::api
