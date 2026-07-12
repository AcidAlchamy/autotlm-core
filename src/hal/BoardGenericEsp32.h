/*
 * BoardGenericEsp32.h — DEPRECATED name, kept as a compatibility alias.
 *
 * The "generic ESP32" board grew up: it IS the AutoTLM One. The class now
 * lives in BoardAutoTLMOne.h; this header remains so old sketches that
 * mention BoardGenericEsp32 (or define AUTOTLM_BOARD_GENERIC_ESP32) keep
 * compiling. New code should use BoardAutoTLMOne.
 *
 * Part of AutoTLM Core — MIT licensed.
 */
#ifndef AUTOTLM_BOARD_GENERIC_ESP32_H
#define AUTOTLM_BOARD_GENERIC_ESP32_H

#if defined(ESP32)

#warning "BoardGenericEsp32 is deprecated — it is now BoardAutoTLMOne (same hardware, same pins)."

#include "BoardAutoTLMOne.h"

typedef BoardAutoTLMOne BoardGenericEsp32;

#endif // ESP32
#endif // AUTOTLM_BOARD_GENERIC_ESP32_H
