#pragma once
/*
 * SmartRepeat – kollisionsarme Flood-Weiterleitung für Repeater
 *
 * Aktiv mit Build-Flag -D SMART_REPEAT. Reine Logik, MyMesh setzt sie um.
 *
 *  1. SNR-gewichtete Wartezeit: Wer ein Flood-Paket SCHWACH empfangen hat, steht
 *     weit vom Absender weg und bringt mit seiner Weiterleitung die meiste neue
 *     Reichweite -> sendet zuerst. Wer es STARK empfangen hat, wartet länger.
 *     Repeater senden dadurch nicht mehr zufällig gleichzeitig.
 *  2. Unterdrückung (Kanal entlasten): Während unser Paket noch in der Warteschlange
 *     liegt, hören wir mit. Hat schon ein anderer Repeater derselben Stufe das Paket
 *     stark (= nah bei uns, deckt unser Gebiet ab) oder mehrfach weitergeleitet,
 *     verwerfen wir unsere Kopie. Direkt geroutete Pakete werden NIE unterdrückt.
 *  3. Akku-Sparstufe (Solar): eigene Adverts aussetzen und bei Unterspannung abschalten.
 *
 * Laufzeit-Befehle (CLI): smart | set smart on|off | set smart.snr <dB> | set smart.count <n>
 */

#include <stdint.h>
#include <string.h>

#ifndef SMART_STRONG_SNR
  #define SMART_STRONG_SNR       3     // dB: Echo so stark -> 1 Kopie reicht zum Unterdrücken
#endif
#ifndef SMART_ECHO_COUNT
  #define SMART_ECHO_COUNT       2     // so viele Echos (beliebiges SNR) -> unterdrücken
#endif
#ifndef SMART_DELAY_SNR_SPAN
  #define SMART_DELAY_SNR_SPAN   13.0f // dB-Spanne von "schwach" bis "stark"
#endif

#ifndef SOLAR_SAVE_MV
  #define SOLAR_SAVE_MV          3500  // darunter: keine eigenen Adverts
#endif
#ifndef SOLAR_CRIT_MV
  #define SOLAR_CRIT_MV          3300  // darunter (3x in Folge): Tiefschlaf bis Sonne nachlädt
#endif
#ifndef SOLAR_CHECK_MS
  #define SOLAR_CHECK_MS         60000UL
#endif

class SmartRepeat {
public:
  bool enabled = true;
  float strong_snr = SMART_STRONG_SNR;
  uint8_t echo_count = SMART_ECHO_COUNT;

  // Statistik
  uint32_t n_suppressed = 0, n_echoes = 0;

  // ---------- 1. Wartezeit ----------
  // window = bisheriges Zufallsfenster (5 * Airtime * txdelay), rnd = Zufall 0..1
  uint32_t calcDelay(float rx_snr, uint8_t sf, uint32_t window, float rnd) const {
    float weak = snrFloor(sf) + 3.0f;   // knapp über der Empfangsgrenze
    float f = (rx_snr - weak) / SMART_DELAY_SNR_SPAN;
    if (f < 0) f = 0;
    if (f > 1) f = 1;
    // 60 % des Fensters nach Empfangsstärke, 40 % Zufall (trennt gleich starke Repeater)
    return (uint32_t)(f * 0.6f * window + rnd * 0.4f * window);
  }

  // ---------- 2. Unterdrückung ----------
  // Wird aufgerufen, wenn ein Echo zu einem noch wartenden eigenen Paket gehört wurde.
  // Rückgabe true = eigene Kopie verwerfen.
  bool onEcho(const uint8_t* hash, float snr) {
    n_echoes++;
    Entry* e = find(hash);
    if (!e) {
      e = &_tab[_next];
      _next = (_next + 1) % TAB_SIZE;
      memcpy(e->hash, hash, HASH_LEN);
      e->count = 0;
    }
    if (e->count < 255) e->count++;
    if (!enabled) return false;
    if (snr >= strong_snr || e->count >= echo_count) {
      n_suppressed++;
      return true;
    }
    return false;
  }

  // ---------- 3. Akku ----------
  enum BattState : uint8_t { BATT_OK = 0, BATT_SAVE = 1, BATT_CRITICAL = 2 };

  bool battCheckDue(uint32_t now) {
    if (!_batt_init || (uint32_t)(now - _last_batt) >= SOLAR_CHECK_MS) {
      _batt_init = true;
      _last_batt = now;
      return true;
    }
    return false;
  }
  BattState onBattery(uint16_t mv, bool external) {
    if (external || mv == 0) { _crit = 0; _batt = BATT_OK; return _batt; }
    if (mv < SOLAR_CRIT_MV) {
      if (++_crit >= 3) _batt = BATT_CRITICAL; else if (_batt == BATT_OK) _batt = BATT_SAVE;
    } else {
      _crit = 0;
      if (mv < SOLAR_SAVE_MV) _batt = BATT_SAVE;
      else if (mv > SOLAR_SAVE_MV + 100) _batt = BATT_OK;   // Hysterese
    }
    return _batt;
  }
  BattState battState() const { return _batt; }

  static float snrFloor(uint8_t sf) { return -2.5f * ((float)sf - 4.0f); }

private:
  static const int HASH_LEN = 6;
  static const int TAB_SIZE = 12;
  struct Entry { uint8_t hash[HASH_LEN]; uint8_t count; };
  Entry _tab[TAB_SIZE] = {};
  int _next = 0;

  Entry* find(const uint8_t* hash) {
    for (int i = 0; i < TAB_SIZE; i++) {
      if (memcmp(_tab[i].hash, hash, HASH_LEN) == 0) return &_tab[i];
    }
    return nullptr;
  }

  BattState _batt = BATT_OK;
  uint8_t _crit = 0;
  uint32_t _last_batt = 0;
  bool _batt_init = false;
};
