#pragma once
/*
 * MobileStation – Erweiterungen für eine mobile Companion-Station (z. B. Autodach)
 *
 * Aktiv nur mit Build-Flag -D MOBILE_STATION. Reine Logik ohne Hardwarezugriff;
 * MyMesh ruft die Hooks auf und setzt die Entscheidungen um.
 *
 *  1. Auto-Sendeleistung: Aus dem SNR der empfangenen Pakete (bester Nachbar) wird
 *     die Linkreserve berechnet. Ist mehr Reserve da als nötig, wird die TX-Leistung
 *     schrittweise gesenkt (min. MOBILE_TXP_MIN). Bei Funkstille oder Send-Timeout
 *     sofort zurück auf Maximum (= in der App eingestellte Leistung).
 *  2. Positions-Adverts beim Fahren: Die App schickt die Handy-Position
 *     (CMD_SET_ADVERT_LATLON). Nach Strecke X wird automatisch ein Zero-Hop-Advert
 *     gesendet, selten zusätzlich ein (Region-)Flood-Advert.
 *  3. Akku: Unterspannungsstufen (sparen / kritisch) und gedrosseltes Flash-Schreiben
 *     der Position.
 *  4. Weniger Kollisionen: eigene Auto-Adverts nur bei leerer Sendewarteschlange,
 *     mit Zufallsverzögerung; CAD (Listen-before-Talk) als Default an.
 */

#include <stdint.h>
#include <math.h>

// ---------- Auto-Sendeleistung ----------
#ifndef MOBILE_TXP_MIN
  #define MOBILE_TXP_MIN            10      // dBm, nie darunter
#endif
#ifndef MOBILE_TXP_TARGET_MARGIN
  #define MOBILE_TXP_TARGET_MARGIN  12      // dB Reserve über SNR-Grenze des SF, die erhalten bleibt
#endif
#ifndef MOBILE_TXP_STEP_DOWN
  #define MOBILE_TXP_STEP_DOWN      2       // dB pro Schritt nach unten (nach oben: sofort)
#endif
#ifndef MOBILE_TXP_INTERVAL_MS
  #define MOBILE_TXP_INTERVAL_MS    30000UL // Bewertungsintervall
#endif
#ifndef MOBILE_TXP_WINDOW_MS
  #define MOBILE_TXP_WINDOW_MS      120000UL // SNR-Fenster (bestes SNR der letzten 2 Fenster zählt)
#endif
#ifndef MOBILE_TXP_SILENCE_MS
  #define MOBILE_TXP_SILENCE_MS     180000UL // nichts gehört -> volle Leistung
#endif
#ifndef MOBILE_TXP_HOLD_MS
  #define MOBILE_TXP_HOLD_MS        120000UL // nach Timeout so lange nicht absenken
#endif

// ---------- Positions-Adverts ----------
#ifndef MOBILE_ADV_ZERO_DIST_M
  #define MOBILE_ADV_ZERO_DIST_M    1000    // Zero-Hop-Advert nach x Metern (0 = aus)
#endif
#ifndef MOBILE_ADV_ZERO_MIN_MS
  #define MOBILE_ADV_ZERO_MIN_MS    300000UL // frühestens alle 5 min
#endif
#ifndef MOBILE_ADV_FLOOD_DIST_M
  #define MOBILE_ADV_FLOOD_DIST_M   10000   // Flood-Advert nach x Metern (0 = aus)
#endif
#ifndef MOBILE_ADV_FLOOD_MIN_MS
  #define MOBILE_ADV_FLOOD_MIN_MS   3600000UL // frühestens jede Stunde
#endif
#ifndef MOBILE_POS_MAX_AGE_MS
  #define MOBILE_POS_MAX_AGE_MS     600000UL // ältere Handy-Position nicht mehr adverten
#endif
#ifndef MOBILE_POS_SAVE_MIN_MS
  #define MOBILE_POS_SAVE_MIN_MS    1800000UL // Position max. alle 30 min ins Flash
#endif
#ifndef MOBILE_ADV_JITTER_MS
  #define MOBILE_ADV_JITTER_MS      3000    // Zufallsverzögerung für Auto-Adverts
#endif

// ---------- Akku ----------
#ifndef MOBILE_BATT_SAVE_MV
  #define MOBILE_BATT_SAVE_MV       3550    // darunter: Sparmodus
#endif
#ifndef MOBILE_BATT_CRIT_MV
  #define MOBILE_BATT_CRIT_MV       3350    // darunter (3x in Folge): Abschalten
#endif
#ifndef MOBILE_BATT_SAVE_TXP_MAX
  #define MOBILE_BATT_SAVE_TXP_MAX  17      // dBm-Deckel im Sparmodus
#endif
#ifndef MOBILE_BATT_CHECK_MS
  #define MOBILE_BATT_CHECK_MS      60000UL
#endif

class MobileStation {
public:
  enum BattState : uint8_t { BATT_OK = 0, BATT_SAVE = 1, BATT_CRITICAL = 2 };
  enum AdvertAction : uint8_t { ADV_NONE = 0, ADV_ZERO_HOP = 1, ADV_FLOOD = 2 };

  void begin(int8_t start_power, uint32_t now) {
    _cur_power = start_power;
    _last_txp_eval = now;
    _win_start = now;
    _last_batt_check = now - MOBILE_BATT_CHECK_MS;  // gleich prüfen
  }

  // ---------- Empfang / Senden ----------
  void onRx(float snr, uint8_t sf, uint32_t now) {
    float margin = snr - snrFloor(sf);
    if (!_cur_win_valid || margin > _cur_win_best) {
      _cur_win_best = margin;
      _cur_win_valid = true;
    }
    _last_rx = now;
    _have_rx = true;
  }

  // Nachricht nicht bestätigt -> sofort volle Leistung, eine Weile halten
  void onSendTimeout(uint32_t now) {
    _hold_until = now + MOBILE_TXP_HOLD_MS;
    _hold_active = true;
    _force_max = true;
  }

  /*
   * Liefert die gewünschte TX-Leistung. max_power = in der App eingestellter Wert.
   * Gibt true zurück, wenn sich der Wert geändert hat.
   */
  bool evalTxPower(int8_t max_power, uint32_t now, int8_t& out_power) {
    rollWindow(now);

    int8_t cap = max_power;
    if (_batt == BATT_SAVE && cap > MOBILE_BATT_SAVE_TXP_MAX) cap = MOBILE_BATT_SAVE_TXP_MAX;
    int8_t floor_p = MOBILE_TXP_MIN < cap ? MOBILE_TXP_MIN : cap;

    int8_t target = _cur_power;
    if (_hold_active && (int32_t)(now - _hold_until) >= 0) _hold_active = false;

    if (_force_max) {
      target = cap;
      _force_max = false;
      _last_txp_eval = now;
    } else if ((uint32_t)(now - _last_txp_eval) >= MOBILE_TXP_INTERVAL_MS) {
      _last_txp_eval = now;
      bool silent = !_have_rx || (uint32_t)(now - _last_rx) >= MOBILE_TXP_SILENCE_MS;
      float best;
      if (silent || !bestMargin(best)) {
        target = cap;                                    // nichts gehört -> volle Leistung
      } else {
        int ideal = (int)lroundf((float)cap - (best - (float)MOBILE_TXP_TARGET_MARGIN));
        if (ideal > cap) ideal = cap;
        if (ideal < floor_p) ideal = floor_p;
        if (ideal > _cur_power) {
          target = (int8_t)ideal;                        // hoch: sofort
        } else if (ideal < _cur_power && !_hold_active) {
          int step = _cur_power - ideal;
          if (step > MOBILE_TXP_STEP_DOWN) step = MOBILE_TXP_STEP_DOWN;
          target = _cur_power - step;                    // runter: langsam
        }
      }
    }

    if (target > cap) target = cap;   // z. B. wenn App-Wert gesenkt wurde
    if (target < floor_p) target = floor_p;
    if (target != _cur_power) {
      _cur_power = target;
      out_power = target;
      return true;
    }
    out_power = _cur_power;
    return false;
  }

  int8_t getCurrentPower() const { return _cur_power; }

  // ---------- Position ----------
  void onPositionUpdate(double lat, double lon, uint32_t now) {
    _pos_lat = lat; _pos_lon = lon;
    _pos_time = now;
    _have_pos = true;
    _pos_dirty = true;
  }

  // soll die Position jetzt ins Flash? (gedrosselt)
  bool shouldPersistPosition(uint32_t now) {
    if (!_pos_dirty) return false;
    if (_have_saved && (uint32_t)(now - _last_save) < MOBILE_POS_SAVE_MIN_MS) return false;
    _pos_dirty = false;
    _have_saved = true;
    _last_save = now;
    return true;
  }

  AdvertAction checkAdvert(bool loc_allowed, bool tx_queue_empty, uint32_t now) {
    if (!loc_allowed || !_have_pos || _batt != BATT_OK || !tx_queue_empty) return ADV_NONE;
    if ((uint32_t)(now - _pos_time) >= MOBILE_POS_MAX_AGE_MS) return ADV_NONE;

    if (!_flood_valid) {   // erster Fix: nur Bezugspunkt, kein Flood direkt nach dem Start
      _flood_lat = _pos_lat; _flood_lon = _pos_lon;
      _flood_valid = true;
      _last_flood = now - MOBILE_ADV_FLOOD_MIN_MS;
    }
#if MOBILE_ADV_FLOOD_DIST_M > 0
    if (distM(_flood_lat, _flood_lon, _pos_lat, _pos_lon) >= MOBILE_ADV_FLOOD_DIST_M &&
        (uint32_t)(now - _last_flood) >= MOBILE_ADV_FLOOD_MIN_MS) {
      _flood_lat = _zero_lat = _pos_lat;
      _flood_lon = _zero_lon = _pos_lon;
      _flood_valid = _zero_valid = true;
      _last_flood = _last_zero = now;
      return ADV_FLOOD;
    }
#endif
#if MOBILE_ADV_ZERO_DIST_M > 0
    if (_zero_valid && distM(_zero_lat, _zero_lon, _pos_lat, _pos_lon) >= MOBILE_ADV_ZERO_DIST_M &&
        (uint32_t)(now - _last_zero) >= MOBILE_ADV_ZERO_MIN_MS) {
      _zero_lat = _pos_lat; _zero_lon = _pos_lon;
      _last_zero = now;
      return ADV_ZERO_HOP;
    }
    if (!_zero_valid) {   // erster Fix ohne Flood -> Startpunkt merken + einmal Zero-Hop
      _zero_lat = _pos_lat; _zero_lon = _pos_lon;
      _zero_valid = true;
      _last_zero = now;
      return ADV_ZERO_HOP;
    }
#endif
    return ADV_NONE;
  }

  // ---------- Akku ----------
  bool battCheckDue(uint32_t now) {
    if ((uint32_t)(now - _last_batt_check) < MOBILE_BATT_CHECK_MS) return false;
    _last_batt_check = now;
    return true;
  }

  // mv = 0 -> unbekannt; Rückgabe: neuer Zustand
  BattState onBattery(uint16_t mv, bool external_power) {
    if (external_power || mv == 0) {
      _crit_count = 0;
      _batt = BATT_OK;
      return _batt;
    }
    if (mv < MOBILE_BATT_CRIT_MV) {
      if (++_crit_count >= 3) _batt = BATT_CRITICAL;
      else if (_batt == BATT_OK) _batt = BATT_SAVE;
    } else {
      _crit_count = 0;
      if (mv < MOBILE_BATT_SAVE_MV) _batt = BATT_SAVE;
      else if (mv > MOBILE_BATT_SAVE_MV + 100) _batt = BATT_OK;   // Hysterese
    }
    return _batt;
  }
  BattState getBattState() const { return _batt; }

  // Entfernung in Metern (equirektangulär, reicht für einige km)
  static float distM(double lat1, double lon1, double lat2, double lon2) {
    const double R = 6371000.0;
    double x = (lon2 - lon1) * (M_PI / 180.0) * cos(((lat1 + lat2) * 0.5) * (M_PI / 180.0));
    double y = (lat2 - lat1) * (M_PI / 180.0);
    return (float)(sqrt(x * x + y * y) * R);
  }

  // Demodulationsgrenze (SNR) je Spreading-Factor laut Semtech
  static float snrFloor(uint8_t sf) {
    return -2.5f * ((float)sf - 4.0f);   // SF7 -7.5, SF8 -10 ... SF12 -20
  }

private:
  void rollWindow(uint32_t now) {
    if ((uint32_t)(now - _win_start) >= MOBILE_TXP_WINDOW_MS) {
      _prev_win_best = _cur_win_best;
      _prev_win_valid = _cur_win_valid;
      _cur_win_valid = false;
      _win_start = now;
    }
  }
  bool bestMargin(float& best) const {
    if (_cur_win_valid && _prev_win_valid) { best = _cur_win_best > _prev_win_best ? _cur_win_best : _prev_win_best; return true; }
    if (_cur_win_valid) { best = _cur_win_best; return true; }
    if (_prev_win_valid) { best = _prev_win_best; return true; }
    return false;
  }

  // TX-Leistung
  int8_t _cur_power = 22;
  uint32_t _last_txp_eval = 0, _last_rx = 0, _win_start = 0, _hold_until = 0;
  float _cur_win_best = 0, _prev_win_best = 0;
  bool _cur_win_valid = false, _prev_win_valid = false, _have_rx = false;
  bool _hold_active = false, _force_max = false;

  // Position
  double _pos_lat = 0, _pos_lon = 0, _zero_lat = 0, _zero_lon = 0, _flood_lat = 0, _flood_lon = 0;
  uint32_t _pos_time = 0, _last_zero = 0, _last_flood = 0, _last_save = 0;
  bool _have_pos = false, _pos_dirty = false, _have_saved = false, _zero_valid = false, _flood_valid = false;

  // Akku
  BattState _batt = BATT_OK;
  uint8_t _crit_count = 0;
  uint32_t _last_batt_check = 0;
};
