#include "broportal/availability.h"

namespace broportal {

bool available(std::string* why) {
#if defined(__linux__)
    (void)why;
    return true;
#else
#if defined(_WIN32)
    const char* os = "Windows";
#elif defined(__APPLE__)
    const char* os = "macOS";
#else
    const char* os = "this platform";
#endif
    if (why) {
        *why = std::string("xdg-desktop-portal backends serve a Linux D-Bus session bus (sd-bus); ") + os +
               " has neither";
    }
    return false;
#endif
}

} // namespace broportal
