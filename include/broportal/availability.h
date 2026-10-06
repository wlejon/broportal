#pragma once

// Whether this build can serve portals at all. broportal is an
// xdg-desktop-portal backend: it lives on a Linux D-Bus session bus (sd-bus).
// On Windows and macOS only the value types (types.h) and this query are
// built; every other header refuses to compile there.

#include <string>

namespace broportal {

// True on Linux. Otherwise false, with the reason in *why.
bool available(std::string* why = nullptr);

} // namespace broportal
