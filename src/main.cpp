/**
 * Plane Radar — WiFi setup, then radar UI on the round GC9A01 display.
 */

#include <Arduino.h>
#include <WiFi.h>

#include "config.h"
#include "hardware/display.h"
#include "services/adsb_client.h"
#include "services/net_session.h"
#include "services/radar_location.h"
#include "services/route_fetcher.h"
#include "services/wifi_setup.h"
#include "ui/radar_animation_policy.h"
#include "ui/radar_display.h"
#include "ui/radar_range.h"
#include "ui/status_screens.h"

namespace {

bool g_radar_visible = false;
unsigned long g_wifi_down_since = 0;
unsigned long g_last_reconnect_ms = 0;
unsigned long g_last_adsb_fetch_ms = 0;
unsigned long g_last_animation_refresh_ms = 0;

void showRadarIfConnected() {
  if (WiFi.status() != WL_CONNECTED) {
    g_radar_visible = false;
    return;
  }
  ui::radarDisplayDraw();
  g_last_animation_refresh_ms = millis();
  g_radar_visible = true;
}

void onRangeTap() {
  ui::radar::rangeNext();
  char range_label[12];
  ui::radar::formatCurrentRing3Label(range_label, sizeof(range_label));
  Serial.printf("Range: %s (outer ~%.0f km)\n", range_label,
                ui::radar::rangeCurrent().outer_km);

  if (g_radar_visible && WiFi.status() == WL_CONNECTED) {
    ui::radarDisplayDraw();
    g_last_animation_refresh_ms = millis();
  }
}

/**
 * Advance the sweep and the dead-reckoned traffic between ADS-B polls.
 *
 * Also runs from the fetch poll hook, so the sweep keeps turning through the
 * seconds-long HTTPS request instead of freezing once per cycle.
 */
void serviceRadarAnimation() {
  if (!g_radar_visible || WiFi.status() != WL_CONNECTED ||
      !ui::radarDisplayCanAnimate()) {
    return;
  }
  if (!ui::radar::animationNeeded(ui::radar::showSweep(),
                                  services::adsb::aircraftCount())) {
    return;
  }

  const unsigned long now_ms = millis();
  if (now_ms - g_last_animation_refresh_ms < ui::radar::kAnimationIntervalMs) {
    return;
  }
  g_last_animation_refresh_ms = now_ms;
  ui::radarDisplayRefreshAircraft();
}

void pollNetwork() {
  wifiLoop();
  serviceRadarAnimation();
}

void handleBootButton() {
  bootButtonPollLongPress();
  if (bootButtonConsumeTap()) {
    onRangeTap();
  }
}

void fetchAndDrawAircraft() {
  const float fetch_km = ui::radar::fetchRadiusKm();
  if (!services::adsb::fetchUpdate(services::location::lat(),
                                   services::location::lon(), fetch_km)) {
    handleBootButton();
    return;
  }
  ui::radarDisplayRefreshAircraft();
  g_last_animation_refresh_ms = millis();
  handleBootButton();
}

}  // namespace

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println();
  Serial.println("Plane Radar");

  bootButtonInit();
  displayInit();
  if (wifiShowsSetupScreenOnBoot()) {
    statusScreenPortal();
  }
  services::location::init();
  services::net::sessionInit();
  ui::radar::rangeInit();
  services::adsb::setPollFn(pollNetwork);
  // Task idles until WiFi is up, so it is safe to start before connecting.
  services::route::init();

  if (wifiSetupConnect()) {
    showRadarIfConnected();
  }
}

void loop() {
  handleBootButton();
  wifiLoop();

  if (WiFi.status() != WL_CONNECTED) {
    if (g_radar_visible) {
      Serial.println("WiFi lost — will reconnect");
      g_radar_visible = false;
    }

    if (g_wifi_down_since == 0) {
      g_wifi_down_since = millis();
    }

    const unsigned long down_ms = millis() - g_wifi_down_since;
    if (down_ms >= config::kWifiDownGraceMs &&
        millis() - g_last_reconnect_ms >= config::kWifiReconnectIntervalMs) {
      g_last_reconnect_ms = millis();
      if (wifiReconnect()) {
        g_wifi_down_since = 0;
        showRadarIfConnected();
      }
    }
  } else {
    g_wifi_down_since = 0;
    if (!g_radar_visible) {
      showRadarIfConnected();
    } else if (millis() - g_last_adsb_fetch_ms >= config::kAdsbFetchIntervalMs &&
               !services::net::sessionBusy()) {
      // Skipping while a route lookup holds the radio leaves the fetch slot
      // unconsumed, so the next iteration claims it as soon as that finishes.
      // fetchUpdate() re-checks under the lock; this is only scheduling.
      g_last_adsb_fetch_ms = millis();
      fetchAndDrawAircraft();
    } else {
      serviceRadarAnimation();
    }
  }

  delay(10);
}
