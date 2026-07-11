/*
 * AutoTLMGNSS.h — GNSS module: NMEA (GGA + RMC) → position, speed, course.
 *
 * The HAL supplies raw NMEA bytes (each board knows its own UART wiring — on
 * the Freematics ONE+ that's the hard-won RX=GPIO26 @ 38400 with TX/RX
 * swapped vs the factory default); this module parses them. Sentences are
 * checksum-validated, so serial garbage can't corrupt a fix.
 *
 * Part of AutoTLM Core — MIT licensed.
 */
#ifndef AUTOTLM_GNSS_H
#define AUTOTLM_GNSS_H

#include <Arduino.h>
#include "../hal/AutoTLMHAL.h"

/** A GNSS snapshot, the struct returned by car.gps(). */
struct AutoTLMGPS {
  bool   fix;       ///< true when the position is valid
  double lat;       ///< latitude, decimal degrees
  double lng;       ///< longitude, decimal degrees
  float  altM;      ///< altitude above MSL, meters
  float  speedKph;  ///< ground speed, km/h
  float  course;    ///< course over ground, degrees true
  float  hdop;      ///< horizontal dilution of precision
  int    sats;      ///< satellites used in solution
  uint32_t ageMs;   ///< milliseconds since the last valid sentence
};

/** Raw-sentence hook for power users (loggers, extra parsers). */
typedef void (*AutoTLMNmeaCallback)(const char* sentence);

class AutoTLMGNSS {
 public:
  /** Power the module and open its UART. */
  bool begin(AutoTLMHAL& hal);

  /** Drain the UART and parse complete sentences. Called from AutoTLM::update(). */
  void tick();

  /** Latest fix data (valid whenever .fix is true). */
  AutoTLMGPS data() const;

  /** True once at least one checksum-valid sentence has arrived. */
  bool alive() const { return m_alive; }

  /** Echo every raw NMEA byte to a stream (like the bench firmware did over USB). */
  void echoTo(Stream* s) { m_echo = s; }

  /** Get each complete, checksum-valid sentence (without CR/LF). */
  void onSentence(AutoTLMNmeaCallback cb) { m_cb = cb; }

 private:
  void parse(char* s);
  static double coord(const char* v, char hemi);

  AutoTLMHAL* m_hal = nullptr;
  Stream* m_echo = nullptr;
  AutoTLMNmeaCallback m_cb = nullptr;

  char m_line[120];
  int m_lineLen = 0;

  bool m_alive = false;
  bool m_fix = false;
  double m_lat = 0, m_lng = 0;
  float m_alt = 0, m_speedKph = 0, m_course = 0, m_hdop = 0;
  int m_sats = 0;
  uint32_t m_lastSentenceMs = 0;
};

#endif // AUTOTLM_GNSS_H
