#ifndef Pins_Arduino_h
#define Pins_Arduino_h

#include <stdint.h>

/*
 * AutoTLM One — variant pin map.
 *
 * The AUTOTLM_PIN_* macros are the board's telemetry wiring. AutoTLM Core's
 * BoardAutoTLMOne picks them up automatically, so a sketch built for the
 * "AutoTLM One" board needs no pin configuration at all.
 */
#define AUTOTLM_ONE_VARIANT 1

#define AUTOTLM_PIN_CAN_TX 5    // -> CAN transceiver D
#define AUTOTLM_PIN_CAN_RX 4    // -> CAN transceiver R
#define AUTOTLM_PIN_GNSS_RX 16  // UART2 RX  <- GNSS TX
#define AUTOTLM_PIN_GNSS_TX 17  // UART2 TX  -> GNSS RX
#define AUTOTLM_GNSS_BAUD 9600
#define AUTOTLM_PIN_IMU_SDA 21
#define AUTOTLM_PIN_IMU_SCL 22
#define AUTOTLM_PIN_LED 2

static const uint8_t TX = 1;
static const uint8_t RX = 3;

static const uint8_t SDA = 21;
static const uint8_t SCL = 22;

static const uint8_t SS = 15;
static const uint8_t MOSI = 23;
static const uint8_t MISO = 19;
static const uint8_t SCK = 18;

static const uint8_t A0 = 36;
static const uint8_t A3 = 39;
static const uint8_t A4 = 32;
static const uint8_t A5 = 33;
static const uint8_t A6 = 34;
static const uint8_t A7 = 35;
static const uint8_t A10 = 4;
static const uint8_t A11 = 0;
static const uint8_t A12 = 2;
static const uint8_t A13 = 15;
static const uint8_t A14 = 13;
static const uint8_t A15 = 12;
static const uint8_t A16 = 14;
static const uint8_t A17 = 27;
static const uint8_t A18 = 25;
static const uint8_t A19 = 26;

static const uint8_t T0 = 4;
static const uint8_t T1 = 0;
static const uint8_t T2 = 2;
static const uint8_t T3 = 15;
static const uint8_t T4 = 13;
static const uint8_t T5 = 12;
static const uint8_t T6 = 14;
static const uint8_t T7 = 27;
static const uint8_t T8 = 33;
static const uint8_t T9 = 32;

static const uint8_t DAC1 = 25;
static const uint8_t DAC2 = 26;

#endif /* Pins_Arduino_h */
