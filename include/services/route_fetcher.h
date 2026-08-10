/*
 * Origin→destination lookup for callsigns, via api.adsbdb.com.
 * Adapted from upstream PR #33 (Igor Boguslavsky).
 */
#pragma once

#include <cstddef>

namespace services::route {

/** Start the background lookup task. Safe to call once, after WiFi is up. */
void init();

/**
 * Copy the cached route for a callsign, e.g. "PHX-DEN".
 *
 * Never blocks on the network: returns true only when the answer is already
 * cached, otherwise queues a background lookup and returns false so the caller
 * picks it up on a later poll. A known-routeless callsign resolves to an empty
 * string (still true) so it isn't queried again.
 */
bool getRoute(const char* callsign, char* out_route, size_t out_len);

}  // namespace services::route
