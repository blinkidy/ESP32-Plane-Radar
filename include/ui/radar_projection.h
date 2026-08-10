#pragma once

namespace ui::radar {

/**
 * Shared flat-earth projection around the radar centre.
 *
 * Latitude is a constant 111 km/deg; longitude is scaled by cos(centre lat),
 * without which everything east-west is stretched by 1/cos(lat) — 20% at 33°N,
 * 38% at 52°N — and runway headings render at visibly wrong angles.
 */

/** Refresh the cached cos(lat) scale. Cheap; call once per frame before use. */
void projectionSync();

/** Offsets from the radar centre in km (dx = east, dy = north). */
void offsetKmFromCenter(float lat, float lon, float* dx_km, float* dy_km,
                        float* dist_km);

/** Squared distance in km² — same as offsetKmFromCenter without the sqrt. */
float distSqKmFromCenter(float lat, float lon);

/** Display scale for the active range preset. */
float pixelsPerKm();

/** Flat lat/lon to screen pixels, north = screen up. */
void latLonToScreen(float lat, float lon, int* out_x, int* out_y);

}  // namespace ui::radar
