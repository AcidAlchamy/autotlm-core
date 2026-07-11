/*
 * AutoTLMPids.h — OBD-II mode-01 PID names and the shared normalization rules.
 *
 * The normalization here intentionally mirrors the Freematics co-processor's
 * integer conventions (RPM = raw/4, temps = A-40 in °C, percents = A*100/255,
 * module voltage = raw/1000 ...) so that a value in the telemetry frame means
 * the same thing no matter which board produced it.
 *
 * Part of AutoTLM Core — MIT licensed.
 */
#ifndef AUTOTLM_PIDS_H
#define AUTOTLM_PIDS_H

#include <stdint.h>

// Mode-01 PIDs (guarded — the Freematics library defines the same names).
#ifndef PID_ENGINE_LOAD
#define PID_ENGINE_LOAD 0x04
#define PID_COOLANT_TEMP 0x05
#define PID_SHORT_TERM_FUEL_TRIM_1 0x06
#define PID_LONG_TERM_FUEL_TRIM_1 0x07
#define PID_SHORT_TERM_FUEL_TRIM_2 0x08
#define PID_LONG_TERM_FUEL_TRIM_2 0x09
#define PID_FUEL_PRESSURE 0x0A
#define PID_INTAKE_MAP 0x0B
#define PID_RPM 0x0C
#define PID_SPEED 0x0D
#define PID_TIMING_ADVANCE 0x0E
#define PID_INTAKE_TEMP 0x0F
#define PID_MAF_FLOW 0x10
#define PID_THROTTLE 0x11
#define PID_RUNTIME 0x1F
#define PID_DISTANCE_WITH_MIL 0x21
#define PID_COMMANDED_EGR 0x2C
#define PID_EGR_ERROR 0x2D
#define PID_COMMANDED_EVAPORATIVE_PURGE 0x2E
#define PID_FUEL_LEVEL 0x2F
#define PID_WARMS_UPS 0x30
#define PID_DISTANCE 0x31
#define PID_EVAP_SYS_VAPOR_PRESSURE 0x32
#define PID_BAROMETRIC 0x33
#define PID_CATALYST_TEMP_B1S1 0x3C
#define PID_CATALYST_TEMP_B2S1 0x3D
#define PID_CATALYST_TEMP_B1S2 0x3E
#define PID_CATALYST_TEMP_B2S2 0x3F
#define PID_CONTROL_MODULE_VOLTAGE 0x42
#define PID_ABSOLUTE_ENGINE_LOAD 0x43
#define PID_AIR_FUEL_EQUIV_RATIO 0x44
#define PID_RELATIVE_THROTTLE_POS 0x45
#define PID_AMBIENT_TEMP 0x46
#define PID_ABSOLUTE_THROTTLE_POS_B 0x47
#define PID_ABSOLUTE_THROTTLE_POS_C 0x48
#define PID_ACC_PEDAL_POS_D 0x49
#define PID_ACC_PEDAL_POS_E 0x4A
#define PID_ACC_PEDAL_POS_F 0x4B
#define PID_COMMANDED_THROTTLE_ACTUATOR 0x4C
#define PID_TIME_WITH_MIL 0x4D
#define PID_TIME_SINCE_CODES_CLEARED 0x4E
#define PID_ETHANOL_FUEL 0x52
#define PID_FUEL_RAIL_PRESSURE 0x59
#define PID_HYBRID_BATTERY_PERCENTAGE 0x5B
#define PID_ENGINE_OIL_TEMP 0x5C
#define PID_FUEL_INJECTION_TIMING 0x5D
#define PID_ENGINE_FUEL_RATE 0x5E
#define PID_ENGINE_TORQUE_DEMANDED 0x61
#define PID_ENGINE_TORQUE_PERCENTAGE 0x62
#define PID_ENGINE_REF_TORQUE 0x63
#endif // PID_ENGINE_LOAD

namespace autotlm {

/**
 * Normalize a raw mode-01 payload (data bytes A, B — the bytes after
 * "41 <pid>") into the AutoTLM integer value convention.
 */
inline int normalizePid(uint8_t pid, uint8_t A, uint8_t B) {
  const int large = ((int)A << 8) | B;
  switch (pid) {
    case PID_RPM:
    case PID_EVAP_SYS_VAPOR_PRESSURE:
      return large >> 2;
    case PID_FUEL_PRESSURE:
      return (int)A * 3;
    case PID_COOLANT_TEMP:
    case PID_INTAKE_TEMP:
    case PID_AMBIENT_TEMP:
    case PID_ENGINE_OIL_TEMP:
      return (int)A - 40;
    case PID_THROTTLE:
    case PID_COMMANDED_EGR:
    case PID_COMMANDED_EVAPORATIVE_PURGE:
    case PID_FUEL_LEVEL:
    case PID_RELATIVE_THROTTLE_POS:
    case PID_ABSOLUTE_THROTTLE_POS_B:
    case PID_ABSOLUTE_THROTTLE_POS_C:
    case PID_ACC_PEDAL_POS_D:
    case PID_ACC_PEDAL_POS_E:
    case PID_ACC_PEDAL_POS_F:
    case PID_COMMANDED_THROTTLE_ACTUATOR:
    case PID_ENGINE_LOAD:
    case PID_ABSOLUTE_ENGINE_LOAD:
    case PID_ETHANOL_FUEL:
    case PID_HYBRID_BATTERY_PERCENTAGE:
      return (int)A * 100 / 255;
    case PID_MAF_FLOW:
      return large / 100;
    case PID_TIMING_ADVANCE:
      return (int)(A / 2) - 64;
    case PID_DISTANCE:
    case PID_DISTANCE_WITH_MIL:
    case PID_TIME_WITH_MIL:
    case PID_TIME_SINCE_CODES_CLEARED:
    case PID_RUNTIME:
    case PID_FUEL_RAIL_PRESSURE:
    case PID_ENGINE_REF_TORQUE:
      return large;
    case PID_CONTROL_MODULE_VOLTAGE:
      return large / 1000;
    case PID_ENGINE_FUEL_RATE:
      return large / 20;
    case PID_ENGINE_TORQUE_DEMANDED:
    case PID_ENGINE_TORQUE_PERCENTAGE:
      return (int)A - 125;
    case PID_SHORT_TERM_FUEL_TRIM_1:
    case PID_LONG_TERM_FUEL_TRIM_1:
    case PID_SHORT_TERM_FUEL_TRIM_2:
    case PID_LONG_TERM_FUEL_TRIM_2:
    case PID_EGR_ERROR:
      return ((int)A - 128) * 100 / 128;
    case PID_FUEL_INJECTION_TIMING:
      return (large - 26880) / 128;
    case PID_CATALYST_TEMP_B1S1:
    case PID_CATALYST_TEMP_B2S1:
    case PID_CATALYST_TEMP_B1S2:
    case PID_CATALYST_TEMP_B2S2:
      return large / 10 - 40;
    case PID_AIR_FUEL_EQUIV_RATIO:
      return (int)((long)large * 200 / 65536);
    default:
      // single-byte PIDs pass through: speed (km/h), MAP (kPa), baro (kPa)...
      return A;
  }
}

/** Format a raw 16-bit DTC into "P0171" style. `buf` needs >= 6 chars. */
inline void formatDTC(uint16_t code, char* buf) {
  static const char sys[] = "PCBU";
  buf[0] = sys[(code >> 14) & 3];
  const uint16_t d = code & 0x3FFF;
  buf[1] = (char)('0' + ((d >> 12) & 0x3));
  const char* hexd = "0123456789ABCDEF";
  buf[2] = hexd[(d >> 8) & 0xF];
  buf[3] = hexd[(d >> 4) & 0xF];
  buf[4] = hexd[d & 0xF];
  buf[5] = 0;
}

}  // namespace autotlm

#endif // AUTOTLM_PIDS_H
