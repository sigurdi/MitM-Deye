#pragma once
// Deye PCS CAN man-in-the-middle: logic for the frames that may be changed.
// Rule: user settings can only LOWER what the BMS asks for, never raise it.
// Everything not handled here is forwarded byte-for-byte.
#include "esphome.h"
#include <cmath>

namespace mitm {

struct Limits {
  bool enabled;          // master switch: false = pure pass-through
  bool voltage_limit;    // lower the charge voltage (CVL)
  float max_voltage;     // V
  float max_charge_a;    // A
  float max_discharge_a; // A
  bool charge_allowed;
  bool discharge_allowed;
  // Cell overvoltage guard (uses the highest cell of all packs from 0x361)
  bool guard;            // guard switch
  float guard_slow_v;    // above this: charge current down to guard_slow_a
  float guard_stop_v;    // above this: charging stopped
  float guard_resume_v;  // after a stop, back to slow charging when below this (and the pause is over)
  float guard_slow_a;    // A, total for all packs
  // Low cell guard (uses the lowest cell of all packs from 0x361)
  bool low_guard;        // low guard switch
  float low_step1_v;     // below this: discharge current down to low_step1_a (mild)
  float low_step1_a;     // A, total for all packs
  float low_slow_v;      // step 2, below this: discharge current down to low_slow_a
  float low_slow_a;      // A, total for all packs
  float low_stop_v;      // below this: discharging stopped
  float low_resume_v;    // back up from a stop / back to normal once the lowest cell is above this (= charged a bit)
};

// Guard states. NORMAL -> SLOW -> STOP -> (pause, cell < resume) -> SLOW -> (cell < RELEASE_V for RELEASE_MS) -> NORMAL
enum class Guard : uint8_t { OFF, NORMAL, SLOW, STOP, NO_DATA };
inline const char *guard_name(Guard g) {
  switch (g) {
    case Guard::OFF: return "Off";
    case Guard::NORMAL: return "Normal";
    case Guard::SLOW: return "Slow charge";
    case Guard::STOP: return "Charge stopped";
    default: return "No cell data";
  }
}
constexpr float GUARD_RELEASE_V = 3.40f;            // all cells below this ...
constexpr uint32_t GUARD_RELEASE_MS = 30 * 60000;   // ... for 30 min -> back to normal
constexpr uint32_t GUARD_PAUSE_MS = 15 * 60000;     // minimum pause after a stop
constexpr uint32_t GUARD_STALE_MS = 30000;          // no 0x361 for 30 s -> guard does nothing

// Low cell guard states, stepwise: NORMAL -> STEP1 (min < step1_v) -> SLOW (= step 2, min < slow_v)
// -> STOP (min < stop_v). It only steps down while low; it never steps back up between steps.
// STOP -> SLOW once min >= resume_v; STEP1/SLOW -> NORMAL once min >= resume_v for LOW_RELEASE_MS.
// Going down needs LOW_TRIP_MS below the threshold, so a short sag from a load spike does not trip it.
// resume_v is above the rest voltage of a low battery, so release needs a charge (no pumping at the bottom).
enum class LowGuard : uint8_t { OFF, NORMAL, STEP1, SLOW, STOP, NO_DATA };
inline const char *low_guard_name(LowGuard g) {
  switch (g) {
    case LowGuard::OFF: return "Off";
    case LowGuard::NORMAL: return "Normal";
    case LowGuard::STEP1: return "Reduced discharge step 1";
    case LowGuard::SLOW: return "Reduced discharge step 2";
    case LowGuard::STOP: return "Discharge stopped";
    default: return "No cell data";
  }
}
constexpr uint32_t LOW_TRIP_MS = 10000;             // below a threshold for 10 s before acting
constexpr uint32_t LOW_RELEASE_MS = 10 * 60000;     // above resume_v for 10 min -> back to normal
constexpr float CELL_VALID_MIN = 2.0f, CELL_VALID_MAX = 4.0f;  // outside this a 0x361 value is ignored (both guards)

struct State {
  float bms_cvl = NAN, bms_ccl = NAN, bms_dcl = NAN;     // what the BMS asked for (0x351)
  float sent_cvl = NAN, sent_ccl = NAN, sent_dcl = NAN;  // what the inverter got
  bool bms_force_charge = false;                         // 0x35C bit 5/4 from the BMS
  float cell_max = NAN, cell_min = NAN;                  // 0x361, highest/lowest cell of all packs
  uint32_t last_cell_ms = 0;
  Guard guard = Guard::OFF;
  uint32_t guard_since_ms = 0;                           // when the current guard state started
  uint32_t below_release_ms = 0;                         // since when all cells < GUARD_RELEASE_V (0 = not)
  uint32_t guard_stops = 0;                              // how many times the guard stopped charging
  LowGuard low = LowGuard::OFF;
  uint32_t low_below_step1_ms = 0;                       // since when min cell < step1_v (0 = not)
  uint32_t low_below_slow_ms = 0, low_below_stop_ms = 0; // since when min cell < slow_v / stop_v (0 = not)
  uint32_t low_above_resume_ms = 0;                      // since when min cell >= resume_v (0 = not)
  uint32_t low_stops = 0;                                // how many times the low guard stopped discharging
  uint32_t last_bms_ms = 0, last_inv_ms = 0;
  uint32_t from_bms = 0, from_inv = 0, modified = 0, tx_errors = 0;
};
inline State &state() {
  static State s;
  return s;
}

inline uint16_t rd_u16(const std::vector<uint8_t> &d, int o) { return (uint16_t) (d[o] | (d[o + 1] << 8)); }
inline int16_t rd_s16(const std::vector<uint8_t> &d, int o) { return (int16_t) rd_u16(d, o); }
inline void wr_u16(std::vector<uint8_t> &d, int o, uint16_t v) {
  d[o] = v & 0xFF;
  d[o + 1] = v >> 8;
}

// Lower a 0.1 A current field to `limit_a`; never raises it.
inline bool clamp_current(std::vector<uint8_t> &d, int o, float limit_a) {
  const int16_t bms = rd_s16(d, o);
  const int16_t lim = (int16_t) std::lround(std::max(0.0f, limit_a) * 10.0f);
  if (lim < bms) {
    wr_u16(d, o, (uint16_t) lim);
    return true;
  }
  return false;
}

// "since" timer helper: start when cond becomes true, clear when false. Returns ms held (0 = not held).
// 0 means "not started", so a start at millis() == 0 is stored as 1; wrap-around safe (unsigned math).
inline uint32_t held(uint32_t &since, bool cond, uint32_t now) {
  if (!cond) { since = 0; return 0; }
  if (!since) since = now ? now : 1;
  return now - since + 1;
}

inline void set_guard(State &s, Guard g, uint32_t now) {
  if (s.guard == g) return;
  ESP_LOGW("guard", "%s -> %s (cell max %.3f V)", guard_name(s.guard), guard_name(g), s.cell_max);
  if (g == Guard::STOP) s.guard_stops++;
  s.guard = g;
  s.guard_since_ms = now;
}

// Runs on every new 0x361 and from the status interval (to catch stale data).
inline void update_guard(const Limits &L) {
  State &s = state();
  const uint32_t now = millis();
  if (!L.enabled || !L.guard) { set_guard(s, Guard::OFF, now); return; }
  const float v = s.cell_max;
  if (s.last_cell_ms == 0 || now - s.last_cell_ms > GUARD_STALE_MS || std::isnan(v) ||
      v < CELL_VALID_MIN || v > CELL_VALID_MAX) {
    s.below_release_ms = 0;
    set_guard(s, Guard::NO_DATA, now);   // fail-open: the BMS still protects itself
    return;
  }
  const uint32_t t_rel = held(s.below_release_ms, v < GUARD_RELEASE_V, now);

  Guard g = s.guard;
  if (g == Guard::OFF || g == Guard::NO_DATA) g = Guard::NORMAL;
  if (v >= L.guard_stop_v) g = Guard::STOP;
  else if (g == Guard::STOP) {
    if (now - s.guard_since_ms >= GUARD_PAUSE_MS && v < L.guard_resume_v) g = Guard::SLOW;
  } else if (v >= L.guard_slow_v) g = Guard::SLOW;
  else if (g == Guard::SLOW && t_rel >= GUARD_RELEASE_MS) g = Guard::NORMAL;
  set_guard(s, g, now);
}

inline void set_low(State &s, LowGuard g) {
  if (s.low == g) return;
  ESP_LOGW("lowguard", "%s -> %s (cell min %.3f V)", low_guard_name(s.low), low_guard_name(g), s.cell_min);
  if (g == LowGuard::STOP) s.low_stops++;
  s.low = g;
}

// Runs on every new 0x361 and from the status interval (to catch stale data).
inline void update_low_guard(const Limits &L) {
  State &s = state();
  const uint32_t now = millis();
  if (!L.enabled || !L.low_guard) {
    s.low_below_step1_ms = s.low_below_slow_ms = s.low_below_stop_ms = s.low_above_resume_ms = 0;
    set_low(s, LowGuard::OFF);
    return;
  }
  const float v = s.cell_min;
  if (s.last_cell_ms == 0 || now - s.last_cell_ms > GUARD_STALE_MS || std::isnan(v) ||
      v < CELL_VALID_MIN || v > CELL_VALID_MAX) {
    s.low_below_step1_ms = s.low_below_slow_ms = s.low_below_stop_ms = s.low_above_resume_ms = 0;
    set_low(s, LowGuard::NO_DATA);   // fail-open: the BMS still protects itself
    return;
  }
  const uint32_t t_step1 = held(s.low_below_step1_ms, v < L.low_step1_v, now);
  const uint32_t t_slow = held(s.low_below_slow_ms, v < L.low_slow_v, now);
  const uint32_t t_stop = held(s.low_below_stop_ms, v < L.low_stop_v, now);
  const uint32_t t_res = held(s.low_above_resume_ms, v >= L.low_resume_v, now);

  LowGuard g = s.low;
  if (g == LowGuard::OFF || g == LowGuard::NO_DATA) g = LowGuard::NORMAL;
  if (t_stop >= LOW_TRIP_MS) g = LowGuard::STOP;
  else if (g == LowGuard::STOP) { if (t_res) g = LowGuard::SLOW; }
  else if (t_slow >= LOW_TRIP_MS) g = LowGuard::SLOW;
  else if (g == LowGuard::NORMAL && t_step1 >= LOW_TRIP_MS) g = LowGuard::STEP1;
  else if ((g == LowGuard::STEP1 || g == LowGuard::SLOW) && t_res >= LOW_RELEASE_MS) g = LowGuard::NORMAL;
  set_low(s, g);
}

// Called for every 11-bit frame from the BMS. Returns true if the frame was changed.
inline bool apply(uint32_t id, std::vector<uint8_t> &d, const Limits &L) {
  State &s = state();
  if (d.size() < 8) return false;

  if (id == 0x35C) s.bms_force_charge = d[0] & 0x30;   // always track, even when disabled
  if (id == 0x361) {                                   // cell max / min in mV
    s.cell_max = rd_u16(d, 0) / 1000.0f;
    s.cell_min = rd_u16(d, 2) / 1000.0f;
    s.last_cell_ms = millis();
    update_guard(L);
    update_low_guard(L);
  }
  if (id == 0x351) {
    s.bms_cvl = rd_u16(d, 0) / 10.0f;
    s.bms_ccl = rd_s16(d, 2) / 10.0f;
    s.bms_dcl = rd_s16(d, 4) / 10.0f;
  }
  if (!L.enabled) {
    if (id == 0x351) { s.sent_cvl = s.bms_cvl; s.sent_ccl = s.bms_ccl; s.sent_dcl = s.bms_dcl; }
    return false;
  }

  // A force-charge request from the BMS (cells critically low) always wins over
  // the user's charge settings: charging is then left fully to the BMS.
  const bool bms_controls_charge = s.bms_force_charge;
  float charge_limit = L.charge_allowed ? L.max_charge_a : 0.0f;
  bool charge_ok = L.charge_allowed;
  if (s.guard == Guard::SLOW) charge_limit = std::min(charge_limit, L.guard_slow_a);
  if (s.guard == Guard::STOP) { charge_limit = 0.0f; charge_ok = false; }
  float discharge_limit = L.discharge_allowed ? L.max_discharge_a : 0.0f;
  bool discharge_ok = L.discharge_allowed;
  if (s.low == LowGuard::STEP1) discharge_limit = std::min(discharge_limit, L.low_step1_a);
  if (s.low == LowGuard::SLOW) discharge_limit = std::min(discharge_limit, L.low_slow_a);
  if (s.low == LowGuard::STOP) { discharge_limit = 0.0f; discharge_ok = false; }
  bool mod = false;

  switch (id) {
    case 0x351: {  // CVL, CCL, DCL, DVL
      if (L.voltage_limit) {
        const uint16_t bms = rd_u16(d, 0);
        const uint16_t lim = (uint16_t) std::lround(L.max_voltage * 10.0f);
        if (lim < bms) { wr_u16(d, 0, lim); mod = true; }
      }
      if (!bms_controls_charge) mod |= clamp_current(d, 2, charge_limit);
      mod |= clamp_current(d, 4, discharge_limit);
      s.sent_cvl = rd_u16(d, 0) / 10.0f;
      s.sent_ccl = rd_s16(d, 2) / 10.0f;
      s.sent_dcl = rd_s16(d, 4) / 10.0f;
      break;
    }
    case 0x371: {  // charge / discharge current limit in grid state
      if (!bms_controls_charge) mod |= clamp_current(d, 0, charge_limit);
      mod |= clamp_current(d, 2, discharge_limit);
      break;
    }
    case 0x35C: {  // request flags: only clear enable bits, never set any bit
      uint8_t f = d[0];
      if (!charge_ok && !bms_controls_charge) f &= ~0x80;
      if (!discharge_ok) f &= ~0x40;
      if (f != d[0]) { d[0] = f; mod = true; }
      break;
    }
    default:
      break;
  }
  return mod;
}

}  // namespace mitm
