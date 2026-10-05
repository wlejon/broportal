#include "broportal/types.h"

#include <cassert>
#include <iostream>

int main() {
    using namespace broportal;

    // Primitives
    Variant v_bool(true);
    assert(v_bool.holds<bool>());
    assert(v_bool.get_value_or<bool>(false) == true);
    assert(v_bool.signature() == "b");

    Variant v_u32(static_cast<uint32_t>(42));
    assert(v_u32.holds<uint32_t>());
    assert(v_u32.get_value_or<uint32_t>(0) == 42);
    assert(v_u32.signature() == "u");

    Variant v_str("hello world");
    assert(v_str.holds<std::string>());
    assert(v_str.get_value_or<std::string>("") == "hello world");
    assert(v_str.signature() == "s");

    Variant v_path(ObjectPath{"/org/test/path"});
    assert(v_path.holds<ObjectPath>());
    assert(v_path.signature() == "o");

    Variant v_fd(UnixFd{3});
    assert(v_fd.holds<UnixFd>());
    assert(v_fd.signature() == "h");

    Variant v_rgb(RgbColor{0.1, 0.5, 0.9});
    assert(v_rgb.holds<RgbColor>());
    assert(v_rgb.signature() == "(ddd)");

    Variant v_coord(Coord2D{1920, 1080});
    assert(v_coord.holds<Coord2D>());
    assert(v_coord.signature() == "(ii)");

    // Containers
    std::vector<std::string> list{"alpha", "beta", "gamma"};
    Variant v_list(list);
    assert(v_list.holds<std::vector<std::string>>());
    assert(v_list.signature() == "as");

    StringPairList pairs{{"key1", "val1"}, {"key2", "val2"}};
    Variant v_pairs(pairs);
    assert(v_pairs.holds<StringPairList>());
    assert(v_pairs.signature() == "a(ss)");

    VariantMap vmap;
    vmap["title"] = Variant("My Test");
    vmap["count"] = Variant(static_cast<uint32_t>(10));
    Variant v_map(vmap);
    assert(v_map.holds<VariantMap>());
    assert(v_map.signature() == "a{sv}");

    StreamList streams;
    VariantMap sprops;
    sprops["size"] = Variant(Coord2D{1280, 720});
    streams.emplace_back(100, sprops);
    Variant v_streams(streams);
    assert(v_streams.holds<StreamList>());
    assert(v_streams.signature() == "a(ua{sv})");

    ShortcutList shortcuts;
    VariantMap scprops;
    scprops["description"] = Variant("Take Screenshot");
    shortcuts.emplace_back("shortcut-1", scprops);
    Variant v_shortcuts(shortcuts);
    assert(v_shortcuts.holds<ShortcutList>());
    assert(v_shortcuts.signature() == "a(sa{sv})");

    SettingsMap settings;
    settings["org.freedesktop.appearance"]["color-scheme"] = Variant(static_cast<uint32_t>(1));
    Variant v_settings(settings);
    assert(v_settings.holds<SettingsMap>());
    assert(v_settings.signature() == "a{sa{sv}}");

    // Equality checks
    Variant v_bool_copy(true);
    assert(v_bool == v_bool_copy);
    assert(!(v_bool == v_u32));

    Variant v_map_copy(vmap);
    assert(v_map == v_map_copy);

    std::cout << "test_types PASSED\n";
    return 0;
}
