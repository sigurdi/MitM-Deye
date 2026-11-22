#pragma once
// Builds mitm::Limits from the ESPHome entities (included after the entity ids exist).
#include "esphome.h"
inline mitm::Limits mitm_limits() {
  return mitm::Limits{
      id(mitm_active).state,          id(voltage_limit_on).state,
      (float) id(max_charge_voltage).state, (float) id(max_charge_current).state,
      (float) id(max_discharge_current).state, id(charging_allowed).state,
      id(discharging_allowed).state,  id(cell_guard_on).state,
      (float) id(guard_slow_v).state, (float) id(guard_stop_v).state,
      (float) id(guard_resume_v).state, (float) id(guard_slow_a).state,
      id(low_guard_on).state,
      (float) id(low_step1_v).state,  (float) id(low_step1_a).state,
      (float) id(low_slow_v).state,   (float) id(low_slow_a).state,
      (float) id(low_stop_v).state,   (float) id(low_resume_v).state};
}
