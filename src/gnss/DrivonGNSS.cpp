/*
 * DrivonGNSS.cpp — NMEA parsing (GGA + RMC) with checksum validation.
 * Part of Drivon Core — MIT licensed.
 */
#include "DrivonGNSS.h"

#include <stdlib.h>
#include <string.h>

bool DrivonGNSS::begin(DrivonHAL& hal) {
  m_hal = &hal;
  m_hal->gnssPower(true);
  return m_hal->gnssBegin();
}

void DrivonGNSS::tick() {
  if (!m_hal) return;
  while (m_hal->gnssAvailable() > 0) {
    const int ci = m_hal->gnssRead();
    if (ci < 0) break;
    const char c = (char)ci;
    if (m_echo) m_echo->write(c);
    if (c == '\n') {
      m_line[m_lineLen] = 0;
      if (m_lineLen > 6) parse(m_line);
      m_lineLen = 0;
    } else if (c != '\r' && m_lineLen < (int)sizeof(m_line) - 1) {
      m_line[m_lineLen++] = c;
    } else if (c != '\r') {
      m_lineLen = 0;  // oversized garbage line — restart
    }
  }
}

// "4903.50,N" -> 49.0583333 (ddmm.mmmm to decimal degrees)
double DrivonGNSS::coord(const char* v, char hemi) {
  if (!v || strlen(v) < 3) return 0;
  const double raw = atof(v);
  const int deg = (int)(raw / 100);
  double dec = deg + (raw - deg * 100) / 60.0;
  if (hemi == 'S' || hemi == 'W') dec = -dec;
  return dec;
}

void DrivonGNSS::parse(char* s) {
  if (s[0] != '$') return;

  // Validate the checksum: XOR of everything between '$' and '*'.
  char* star = strchr(s, '*');
  if (!star || strlen(star) < 3) return;
  uint8_t sum = 0;
  for (char* p = s + 1; p < star; p++) sum ^= (uint8_t)*p;
  const uint8_t want = (uint8_t)strtol(star + 1, nullptr, 16);
  if (sum != want) return;

  // Any checksum-valid sentence proves the module is alive and talking.
  m_alive = true;
  m_lastSentenceMs = millis();
  if (m_cb) m_cb(s);  // hand over the intact sentence before we split it

  *star = 0;  // drop "*hh" — fields end here

  // Split on commas into field pointers (empty fields stay valid "").
  char* f[20];
  int n = 0;
  f[n++] = s;
  for (char* p = s; *p && n < 20; p++) {
    if (*p == ',') { *p = 0; f[n++] = p + 1; }
  }

  const char* type = (strlen(f[0]) >= 6) ? f[0] + 3 : "";  // $GNGGA -> GGA

  if (strcmp(type, "GGA") == 0 && n > 9) {
    m_fix = atoi(f[6]) > 0;
    if (f[2][0]) m_lat = coord(f[2], f[3][0] ? f[3][0] : 'N');
    if (f[4][0]) m_lng = coord(f[4], f[5][0] ? f[5][0] : 'E');
    m_sats = atoi(f[7]);
    m_hdop = atof(f[8]);
    m_alt  = atof(f[9]);
  } else if (strcmp(type, "RMC") == 0 && n > 8) {
    m_fix = (f[2][0] == 'A');
    if (f[3][0]) m_lat = coord(f[3], f[4][0] ? f[4][0] : 'N');
    if (f[5][0]) m_lng = coord(f[5], f[6][0] ? f[6][0] : 'E');
    m_speedKph = atof(f[7]) * 1.852f;  // knots -> km/h
    m_course   = atof(f[8]);
  }
}

DrivonGPS DrivonGNSS::data() const {
  DrivonGPS g;
  g.fix = m_fix;
  g.lat = m_lat;
  g.lng = m_lng;
  g.altM = m_alt;
  g.speedKph = m_speedKph;
  g.course = m_course;
  g.hdop = m_hdop;
  g.sats = m_sats;
  g.ageMs = m_lastSentenceMs ? (millis() - m_lastSentenceMs) : 0xFFFFFFFF;
  return g;
}
