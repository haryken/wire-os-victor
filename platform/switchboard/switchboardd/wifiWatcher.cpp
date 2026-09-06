/**
 * File: wifiWatcher.cpp
 *
 * Author: paluri
 * Created: 8/28/2018
 *
 * Description: A watchdog to ensure robot is connected to robot (if possible) and to
 * try to connect if it is not.
 *
 * Copyright: Anki, Inc. 2018
 *
 **/

#include "log.h"
#include "util/fileUtils/fileUtils.h"
#include "wifiWatcher.h"
#include "exec_command.h"

#include <cutils/properties.h>
#include <cstring>
#include <string>
#include <vector>

namespace Anki {
namespace Switchboard {

namespace {
const char* kPreferBleFlag = "/run/wireos-prefer-ble";
const char* kWifiSetupModeFile = "/data/wired/wifi-setup-mode";
const char* kWifiSetupModePersist = "/persist/wired/wifi-setup-mode";
const char* kWifiSetupModeRun = "/run/wireos-wifi-setup-mode";

bool ModeFileIs(const std::string& raw, const char* want, size_t n) {
  size_t i = 0;
  while (i < raw.size() && (raw[i] == ' ' || raw[i] == '\t' || raw[i] == '\n' || raw[i] == '\r')) {
    ++i;
  }
  return raw.compare(i, n, want) == 0 &&
         (i + n >= raw.size() || raw[i + n] == '\n' || raw[i + n] == '\r' || raw[i + n] == ' ' || raw[i + n] == '\t');
}

bool PreferBleWifi() {
  // Match wired/face: /run → /data → /persist (avoid stale RO /persist).
  const char* paths[] = {kWifiSetupModeRun, kWifiSetupModeFile, kWifiSetupModePersist};
  for (const char* p : paths) {
    const std::string raw = Anki::Util::FileUtils::ReadFile(p);
    if (ModeFileIs(raw, "hotspot", 7)) {
      return false;
    }
    if (ModeFileIs(raw, "ble", 3)) {
      return true;
    }
  }
  if (Anki::Util::FileUtils::FileExists(kPreferBleFlag)) {
    return true;
  }
  return true;
}
}

WifiWatcher::WifiWatcher(struct ev_loop* loop)
: _loop(loop),
_enabled(true) {
  _timer.self = this;
  ev_timer_init(&_timer.timer, WatcherTick, 0, kWifiTick_s);
  ev_timer_start(_loop, &_timer.timer);
}

WifiWatcher::~WifiWatcher() {
  ev_timer_stop(_loop, &_timer.timer);
}

void WifiWatcher::Enable() {
  _enabled = true;
}

void WifiWatcher::Disable() {
  _enabled = false;
}

void WifiWatcher::ConnectIfNoWifi() {
  if(!_enabled) {
    Log::Write("WifiWatcher: disabled, doing nothing this tick.");
    return;
  }

  // User-triggered join in progress (wired) — do not fight ConnMan.
  // Hold-ap mirrors BLE wifiWatcher->Disable() across post-join grace.
  if(Anki::Util::FileUtils::FileExists("/run/wireos-wifi-trying") ||
     Anki::Util::FileUtils::FileExists("/run/wireos-wifi-hold-ap")) {
    Log::Write("WifiWatcher: home WiFi join in progress, skip recover/AP");
    return;
  }

  if(Wifi::IsAccessPointMode()) {
    Log::Write("WifiWatcher: access point mode enabled, doing nothing this tick.");
    _offlineTicks = 0;
    return;
  }

  Anki::Wifi::WiFiState wifiState = Anki::Wifi::GetWiFiState();

  if((wifiState.connState == Anki::Wifi::WiFiConnState::CONNECTED) ||
    (wifiState.connState == Anki::Wifi::WiFiConnState::ONLINE)) {
    _offlineTicks = 0;
    _blePairingOffered = false;
    return;
  }

  if(!HasKnownWifiConfigurations()) {
    Log::Write("WifiWatcher: no known wifi networks.");
    MaybeStartSetupAccessPoint();
    return;
  }

  Log::Write("WifiWatcher: detected no wifi. Scanning for networks...");

  // Scan async ... this won't benefit us this tick, but it will populate the list for next time!
  Anki::Wifi::ScanForWiFiAccessPointsAsync();

  std::vector<Anki::Wifi::WiFiScanResult> results;
  bool shouldScan = false;
  Anki::Wifi::WifiScanErrorCode code = Anki::Wifi::GetWiFiServices(results, shouldScan);

  if(code != Anki::Wifi::WifiScanErrorCode::SUCCESS) {
    Log::Write("WifiWatcher: wifi scan failed, maybe setup AP");
    MaybeStartSetupAccessPoint();
    return;
  }

  for(const Anki::Wifi::WiFiScanResult& result : results) {
    if(result.provisioned) {
      Anki::Wifi::ConnectWifiResult connectResult = 
        Anki::Wifi::ConnectWiFiBySsid(result.ssid, 
                                      "", // don't need pw since already provisioned
                                      (uint8_t)result.auth,
                                      result.hidden,
                                      nullptr,
                                      nullptr);

      if(connectResult == Anki::Wifi::ConnectWifiResult::CONNECT_SUCCESS) {
        // reset error counter
        _connectErrorCount = 0;
        _offlineTicks = 0;

        Log::Write("WifiWatcher: Switchboard autoconnected to wifi successfully.");
        return;
      } else {
        if(++_connectErrorCount >= kMaxErrorBeforeRestart) {
          // restart wpa_supplicant/connman
          Anki::Wifi::RecoverNetworkServices();

          // reset error counter
          _connectErrorCount = 0;
        }
        
        Log::Write("WifiWatcher: Switchboard failed to autoconnect.");
      }
      break;
    }
  }

  MaybeStartSetupAccessPoint();
}

void WifiWatcher::MaybeStartSetupAccessPoint() {
  // Boot: wired waits for ConnMan to rejoin saved home WiFi before raising AP.
  if(Anki::Util::FileUtils::FileExists("/run/wireos-wifi-boot-wait")) {
    Log::Write("WifiWatcher: boot wait for saved WiFi, skip setup AP");
    return;
  }
  if(Anki::Util::FileUtils::FileExists("/run/wireos-wifi-trying") ||
     Anki::Util::FileUtils::FileExists("/run/wireos-wifi-hold-ap")) {
    Log::Write("WifiWatcher: home WiFi join in progress, skip setup AP");
    return;
  }
  if(PreferBleWifi()) {
    if(Anki::Util::FileUtils::FileExists("/run/wireos-setup-ap")) {
      Log::Write("WifiWatcher: BLE wifi chosen, stopping setup AP");
      Anki::ExecCommand({"/usr/bin/vic-setup-ap", "off"});
    }
    if(_offlineTicks < 255) {
      ++_offlineTicks;
    }
    const uint8_t needTicks = HasKnownWifiConfigurations()
                                ? kOfflineTicksBeforeApSaved
                                : kOfflineTicksBeforeAp;
    if(_offlineTicks >= needTicks && !_blePairingOffered) {
      _blePairingOffered = true;
      if(_startBlePairing) {
        Log::Write("WifiWatcher: starting BLE pairing (remembered mode)");
        _startBlePairing();
      }
    }
    return;
  }
  if(Wifi::IsAccessPointMode()) {
    return;
  }
  if(_offlineTicks < 255) {
    ++_offlineTicks;
  }
  const uint8_t needTicks = HasKnownWifiConfigurations()
                              ? kOfflineTicksBeforeApSaved
                              : kOfflineTicksBeforeAp;
  if(_offlineTicks < needTicks) {
    Log::Write("WifiWatcher: offline tick %u/%u (setup AP soon)",
               (unsigned)_offlineTicks, (unsigned)needTicks);
    return;
  }

  char vicName[PROPERTY_VALUE_MAX] = {0};
  (void)property_get("anki.robot.name", vicName, "Vector");
  if(vicName[0] == '\0') {
    std::strncpy(vicName, "Vector", PROPERTY_VALUE_MAX - 1);
  }
  // ConnMan tethering cannot do an open AP — use wpa_supplicant AP (no password).
  Log::Write("WifiWatcher: starting open setup AP ssid=%s", vicName);
  int rc = Anki::ExecCommand({"/usr/bin/vic-setup-ap", "on", vicName});
  if(rc != 0) {
    rc = Anki::ExecCommand({"/anki/bin/vic-setup-ap", "on", vicName});
  }
  if(rc != 0) {
    Log::Write("WifiWatcher: open AP failed");
    return;
  }
  _offlineTicks = 0;
}

bool WifiWatcher::HasKnownWifiConfigurations() {
  if (Anki::Util::FileUtils::FileExists("/data/lib/connman/wireos-wifi.config")) {
    return true;
  }
  std::vector<std::string> configs;
  Anki::Util::FileUtils::ListAllDirectories("/data/lib/connman", configs);
  for (const auto& name : configs) {
    if (name.size() >= 4 && name.compare(0, 4, "wifi") == 0) {
      return true;
    }
  }
  return false;
}

void WifiWatcher::WatcherTick(struct ev_loop* loop, struct ev_timer* w, int revents) {
  struct ev_WifiTimerStruct* timerStruct = (struct ev_WifiTimerStruct*)w;
  timerStruct->self->ConnectIfNoWifi();
}

} // Switchboard
} // Anki