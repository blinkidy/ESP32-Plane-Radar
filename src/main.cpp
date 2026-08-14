/**
 * Plane Radar — WiFi setup, then radar UI on the round GC9A01 display.
 */

#include <Arduino.h>
#include <WiFi.h>

#include "config.h"
#include "hardware/display.h"
#include "services/adsb_client.h"
#include "services/radar_location.h"
#include "services/wifi_setup.h"
#include "ui/radar_display.h"
#include "ui/radar_range.h"
#include "ui/status_screens.h"

namespace {

bool g_radar_visible = false;
unsigned long g_wifi_down_since = 0;
unsigned long g_last_reconnect_ms = 0;
unsigned long g_last_adsb_fetch_ms = 0;
unsigned long g_adsb_fresh_since_ms = 0;
unsigned long g_last_adsb_success_ms = 0;
unsigned int g_adsb_failure_count = 0;

void resetAdsbFreshnessCheck() {
  g_adsb_fresh_since_ms = millis();
  g_last_adsb_success_ms = 0;
  g_adsb_failure_count = 0;
}

void restartIfAdsbStale() {
  // A fetch can outlive the Wi-Fi connection that started it. Let the outer
  // loop record the disconnect and use the normal reconnect grace instead of
  // rebooting from the failed request's return path.
  if (WiFi.status() != WL_CONNECTED) {
    return;
  }

  const unsigned long freshness_ms =
      g_last_adsb_success_ms != 0 ? g_last_adsb_success_ms
                                 : g_adsb_fresh_since_ms;
  const unsigned long stale_ms = millis() - freshness_ms;
  if (stale_ms < config::kAdsbStaleRestartMs) {
    return;
  }

  Serial.printf("adsb: no successful update for %lu s (%u failures); restarting\n",
                stale_ms / 1000UL, g_adsb_failure_count);
  Serial.flush();
  delay(100);
  ESP.restart();
}

void showRadarIfConnected() {
  if (WiFi.status() != WL_CONNECTED) {
    g_radar_visible = false;
    return;
  }
  ui::radarDisplayDraw();
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
  }
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
    ++g_adsb_failure_count;
    restartIfAdsbStale();
    handleBootButton();
    return;
  }
  if (g_adsb_failure_count > 0) {
    Serial.printf("adsb: fresh data restored after %u failed pulls\n",
                  g_adsb_failure_count);
  }
  g_last_adsb_success_ms = millis();
  g_adsb_failure_count = 0;
  ui::radarDisplayRefreshAircraft();
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
  ui::radar::rangeInit();
  services::adsb::setPollFn(wifiLoop);

  if (wifiSetupConnect()) {
    resetAdsbFreshnessCheck();
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
      g_adsb_fresh_since_ms = 0;
      g_last_adsb_success_ms = 0;
      g_adsb_failure_count = 0;
    }

    const unsigned long down_ms = millis() - g_wifi_down_since;
    if (down_ms >= config::kWifiDownGraceMs &&
        millis() - g_last_reconnect_ms >= config::kWifiReconnectIntervalMs) {
      g_last_reconnect_ms = millis();
      if (wifiReconnect()) {
        g_wifi_down_since = 0;
        resetAdsbFreshnessCheck();
        showRadarIfConnected();
      }
    }
  } else {
    g_wifi_down_since = 0;
    if (g_adsb_fresh_since_ms == 0) {
      resetAdsbFreshnessCheck();
    }
    if (!g_radar_visible) {
      showRadarIfConnected();
    } else if (millis() - g_last_adsb_fetch_ms >= config::kAdsbFetchIntervalMs) {
      g_last_adsb_fetch_ms = millis();
      fetchAndDrawAircraft();
    }
    // Also catches a scheduler/state regression that stops fetches entirely,
    // not just HTTP requests that return an explicit failure.
    restartIfAdsbStale();
  }

  delay(10);
}
