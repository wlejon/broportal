#pragma once

#include <memory>
#include <string>

namespace broportal {
class PortalBackend;
}

namespace broportal::api {

/// Mounts `bro.portal` in the current Bronze realm.
void installPortal();

/// Pumps async portal requests, dispatches JS handlers, and settles responses.
void tickPortalAsync();

/// Stops the backend and cancels pending dialog requests.
void shutdownPortalAsync();

/// Sets the backend used by the API (defaults to user bus on Linux).
void setBackend(std::shared_ptr<broportal::PortalBackend> backend);
void setBackend(broportal::PortalBackend* backend);

/// Gets the backend currently used by the API.
std::shared_ptr<broportal::PortalBackend> getBackend();

} // namespace broportal::api

using broportal::api::installPortal;
using broportal::api::tickPortalAsync;
using broportal::api::shutdownPortalAsync;
using broportal::api::setBackend;
using broportal::api::getBackend;
