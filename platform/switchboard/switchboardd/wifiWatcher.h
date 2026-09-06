/**
 * File: wifiWatcher.h
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

#pragma once

#include "anki-wifi/wifi.h"
#include "ev++.h"

#include <functional>
#include <utility>

namespace Anki {
namespace Switchboard {

class WifiWatcher {
public:
  WifiWatcher(struct ev_loop* loop);
  ~WifiWatcher();

  void ConnectIfNoWifi();
  void Enable();
  void Disable();
  void SetStartBlePairingFn(std::function<void()> fn) { _startBlePairing = std::move(fn); }

private:
  static void WatcherTick(struct ev_loop* loop, struct ev_timer* w, int revents);

  bool HasKnownWifiConfigurations();
  void MaybeStartSetupAccessPoint();

  struct ev_loop* _loop;

  struct ev_WifiTimerStruct {
    ev_timer timer;
    WifiWatcher* self;
  } _timer;

  const uint8_t kWifiTick_s = 15;
  const uint8_t kMaxErrorBeforeRestart = 5;
  const uint8_t kOfflineTicksBeforeAp = 3;       // no saved WiFi ~45s → setup hotspot
  // Saved WiFi: wait longer (~2.5 min) for assoc+DHCP before open AP.
  const uint8_t kOfflineTicksBeforeApSaved = 10;

  uint8_t _connectErrorCount = 0;
  uint8_t _offlineTicks = 0;
  bool _enabled = true;
  bool _blePairingOffered = false;
  std::function<void()> _startBlePairing;
};

} // Switchboard
} // Anki