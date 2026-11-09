#pragma once

// "ac" anti-collision fork for the MeshCore repeater.
//
//  dup.suppress N    - drop own queued flood retransmit after hearing N copies of it from other nodes
//  txdelay.snr on    - order flood retransmit slots by received SNR (weak first, strong last)
//  advert.throttle M - forward flood adverts of the same node at most once per M minutes
//
// All options default to OFF, so the firmware behaves like upstream until configured.
// Settings live in their own file (/fork_prefs) and never touch /prefs.json.

#include <Arduino.h>
#include <Mesh.h>
#include <string.h>

#define AC_PREFS_FILE      "/fork_prefs"
#define AC_PREFS_MAGIC     0x41433031UL   // "AC01"
#define AC_PENDING_SLOTS   16
#define AC_ADVERT_SLOTS    48
#define AC_ADVERT_KEY_LEN  6

struct ACForkPrefs {
  uint8_t  dup_suppress;     // 0 = off, 1..5 = copies needed to cancel own retransmit
  uint8_t  txdelay_snr;      // 0 = off, 1 = on
  uint16_t advert_throttle;  // minutes, 0 = off
};

struct ACForkStats {
  uint32_t dup_cancelled;     // own retransmits cancelled by dup.suppress
  uint32_t dup_copies_heard;  // copies heard of packets still queued
  uint32_t adverts_throttled; // flood adverts not forwarded
  uint32_t snr_ordered;       // retransmits scheduled with SNR ordering
};

class ACFork {
  struct Pending {
    uint8_t hash[MAX_HASH_SIZE];
    uint8_t copies;
    bool used;
  };
  struct AdvertSeen {
    uint8_t key[AC_ADVERT_KEY_LEN];
    uint32_t last_ms;
    bool used;
  };

  Pending _pending[AC_PENDING_SLOTS];
  uint8_t _pending_next;
  AdvertSeen _adverts[AC_ADVERT_SLOTS];

public:
  ACForkPrefs prefs;
  ACForkStats stats;

  ACFork() {
    prefs.dup_suppress = 0;
    prefs.txdelay_snr = 0;
    prefs.advert_throttle = 0;
    resetStats();
    memset(_pending, 0, sizeof(_pending));
    _pending_next = 0;
    memset(_adverts, 0, sizeof(_adverts));
  }

  void resetStats() { memset(&stats, 0, sizeof(stats)); }

  void sanitise() {
    if (prefs.dup_suppress > 5) prefs.dup_suppress = 5;
    if (prefs.txdelay_snr > 1) prefs.txdelay_snr = 1;
    if (prefs.advert_throttle > 10080) prefs.advert_throttle = 10080;  // max 1 week
  }

  // ---------- persistence ----------

  template <class FS>
  void load(FS* fs) {
    if (!fs->exists(AC_PREFS_FILE)) return;
#if defined(RP2040_PLATFORM)
    auto file = fs->open(AC_PREFS_FILE, "r");
#else
    auto file = fs->open(AC_PREFS_FILE);
#endif
    if (!file) return;
    uint32_t magic = 0;
    ACForkPrefs tmp;
    if (file.read((uint8_t*)&magic, sizeof(magic)) == sizeof(magic) && magic == AC_PREFS_MAGIC
        && file.read((uint8_t*)&tmp, sizeof(tmp)) == sizeof(tmp)) {
      prefs = tmp;
      sanitise();
    }
    file.close();
  }

  template <class FS>
  bool save(FS* fs) {
#if defined(NRF52_PLATFORM) || defined(STM32_PLATFORM)
    fs->remove(AC_PREFS_FILE);
    auto file = fs->open(AC_PREFS_FILE, FILE_O_WRITE);
#elif defined(RP2040_PLATFORM)
    auto file = fs->open(AC_PREFS_FILE, "w");
#else
    auto file = fs->open(AC_PREFS_FILE, "w", true);
#endif
    if (!file) return false;
    uint32_t magic = AC_PREFS_MAGIC;
    bool ok = file.write((const uint8_t*)&magic, sizeof(magic)) == sizeof(magic)
           && file.write((const uint8_t*)&prefs, sizeof(prefs)) == sizeof(prefs);
    file.close();
    return ok;
  }

  // ---------- dup.suppress ----------

  // called when this node schedules a FLOOD retransmit of 'pkt'
  void notePendingFlood(const mesh::Packet* pkt) {
    if (prefs.dup_suppress == 0) return;
    Pending& p = _pending[_pending_next];
    pkt->calculatePacketHash(p.hash);
    p.copies = 0;
    p.used = true;
    _pending_next = (_pending_next + 1) % AC_PENDING_SLOTS;
  }

  // called for every received FLOOD packet (before any rx delay).
  // returns true when the own queued copy should now be cancelled; 'hash' receives the packet hash.
  bool onFloodHeard(const mesh::Packet* pkt, uint8_t* hash) {
    if (prefs.dup_suppress == 0) return false;
    pkt->calculatePacketHash(hash);
    for (int i = 0; i < AC_PENDING_SLOTS; i++) {
      Pending& p = _pending[i];
      if (p.used && memcmp(p.hash, hash, MAX_HASH_SIZE) == 0) {
        if (p.copies < 255) p.copies++;
        stats.dup_copies_heard++;
        if (p.copies >= prefs.dup_suppress) {
          p.used = false;
          return true;
        }
        return false;
      }
    }
    return false;
  }

  // ---------- txdelay.snr ----------

  // window = random upper bound used by upstream (0 .. window). Keeps the same total window,
  // but places weak packets early and strong packets late.
  uint32_t snrDelay(float snr, uint32_t window, uint32_t rnd_0_to_quarter) {
    stats.snr_ordered++;
    const float lo = -10.0f, hi = 12.0f;
    float frac = (snr - lo) / (hi - lo);
    if (frac < 0) frac = 0;
    if (frac > 1) frac = 1;
    uint32_t span = (window * 3) / 4;
    return (uint32_t)(frac * span) + rnd_0_to_quarter;
  }

  // ---------- advert.throttle ----------

  // returns true if this flood advert may be forwarded now
  bool allowAdvert(const mesh::Packet* pkt, uint32_t now_ms) {
    if (prefs.advert_throttle == 0) return true;
    if (pkt->payload_len < AC_ADVERT_KEY_LEN) return true;
    const uint8_t* key = pkt->payload;   // advert payload starts with the sender's public key
    uint32_t window = (uint32_t)prefs.advert_throttle * 60000UL;

    int free_idx = -1, oldest_idx = 0;
    uint32_t oldest_age = 0;
    for (int i = 0; i < AC_ADVERT_SLOTS; i++) {
      AdvertSeen& a = _adverts[i];
      if (!a.used) { if (free_idx < 0) free_idx = i; continue; }
      uint32_t age = now_ms - a.last_ms;
      if (memcmp(a.key, key, AC_ADVERT_KEY_LEN) == 0) {
        if (age < window) {
          stats.adverts_throttled++;
          return false;
        }
        a.last_ms = now_ms;
        return true;
      }
      if (age >= oldest_age) { oldest_age = age; oldest_idx = i; }
    }
    int idx = free_idx >= 0 ? free_idx : oldest_idx;
    memcpy(_adverts[idx].key, key, AC_ADVERT_KEY_LEN);
    _adverts[idx].last_ms = now_ms;
    _adverts[idx].used = true;
    return true;
  }

  // ---------- CLI ----------

  // returns true if the command was handled
  bool handleCommand(const char* command, char* reply, bool& dirty) {
    dirty = false;
    if (strcmp(command, "ac") == 0) {
      sprintf(reply, "ac: dup.suppress=%d txdelay.snr=%s advert.throttle=%dmin",
              prefs.dup_suppress, prefs.txdelay_snr ? "on" : "off", prefs.advert_throttle);
      return true;
    }
    if (strcmp(command, "ac stats") == 0) {
      sprintf(reply, "ac: cancelled=%lu copies=%lu adv_throttled=%lu snr_sched=%lu",
              (unsigned long)stats.dup_cancelled, (unsigned long)stats.dup_copies_heard,
              (unsigned long)stats.adverts_throttled, (unsigned long)stats.snr_ordered);
      return true;
    }
    if (strcmp(command, "ac reset") == 0) {
      resetStats();
      strcpy(reply, "OK - ac stats cleared");
      return true;
    }
    if (strcmp(command, "get dup.suppress") == 0) {
      sprintf(reply, "> %d", prefs.dup_suppress);
      return true;
    }
    if (strcmp(command, "get txdelay.snr") == 0) {
      sprintf(reply, "> %s", prefs.txdelay_snr ? "on" : "off");
      return true;
    }
    if (strcmp(command, "get advert.throttle") == 0) {
      sprintf(reply, "> %d", prefs.advert_throttle);
      return true;
    }
    if (memcmp(command, "set dup.suppress ", 17) == 0) {
      const char* v = &command[17];
      int n = (strcmp(v, "off") == 0) ? 0 : atoi(v);
      if (n < 0 || n > 5) { strcpy(reply, "Err - range 0..5 (0=off)"); return true; }
      prefs.dup_suppress = n;
      dirty = true;
      strcpy(reply, "OK");
      return true;
    }
    if (memcmp(command, "set txdelay.snr ", 16) == 0) {
      const char* v = &command[16];
      if (strcmp(v, "on") == 0 || strcmp(v, "1") == 0) prefs.txdelay_snr = 1;
      else if (strcmp(v, "off") == 0 || strcmp(v, "0") == 0) prefs.txdelay_snr = 0;
      else { strcpy(reply, "Err - on|off"); return true; }
      dirty = true;
      strcpy(reply, "OK");
      return true;
    }
    if (memcmp(command, "set advert.throttle ", 20) == 0) {
      const char* v = &command[20];
      long m = (strcmp(v, "off") == 0) ? 0 : atol(v);
      if (m < 0 || m > 10080) { strcpy(reply, "Err - range 0..10080 min (0=off)"); return true; }
      prefs.advert_throttle = (uint16_t)m;
      memset(_adverts, 0, sizeof(_adverts));
      dirty = true;
      strcpy(reply, "OK");
      return true;
    }
    return false;
  }
};
