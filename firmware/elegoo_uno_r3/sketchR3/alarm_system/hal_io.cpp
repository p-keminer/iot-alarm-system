// =============================================================================
// hal_io.cpp - Hardwareabstraktion fuer Ein-/Ausgaenge
// =============================================================================

#include "hal_io.h"

// Pin-Belegung (nur hier definiert)
static const uint8_t PIN_REED1 = 2;
static const uint8_t PIN_REED2 = 3;
static const uint8_t PIN_BUZZER = 5;
static const uint8_t PIN_LED_ALARM = 6;
static const uint8_t PIN_LED_SENS1 = 7;
static const uint8_t PIN_LED_SENS2 = 8;


void hal_io_init() {
  // Eingaenge mit Pull-Up
  pinMode(PIN_REED1, INPUT_PULLUP);
  pinMode(PIN_REED2, INPUT_PULLUP);
  
  // Ausgaenge
  pinMode(PIN_BUZZER, OUTPUT);
  pinMode(PIN_LED_ALARM, OUTPUT);
  pinMode(PIN_LED_SENS1, OUTPUT);
  pinMode(PIN_LED_SENS2, OUTPUT);
  pinMode(LED_BUILTIN, OUTPUT);
  
  // Alles aus
  digitalWrite(PIN_BUZZER, LOW);
  digitalWrite(PIN_LED_ALARM, LOW);
  digitalWrite(PIN_LED_SENS1, LOW);
  digitalWrite(PIN_LED_SENS2, LOW);
  digitalWrite(LED_BUILTIN, LOW);
}


// Tuersensoren: HIGH = offen (kein Magnet), LOW = zu (Magnet)
bool hal_istTuerOffen1() {
  return digitalRead(PIN_REED1) == HIGH;
}

bool hal_istTuerOffen2() {
  return digitalRead(PIN_REED2) == HIGH;
}


// Buzzer
void hal_buzzerEin() {
  digitalWrite(PIN_BUZZER, HIGH);
}

void hal_buzzerAus() {
  digitalWrite(PIN_BUZZER, LOW);
}

void hal_setBuzzer(bool ein) {
  digitalWrite(PIN_BUZZER, ein ? HIGH : LOW);
}


// LEDs
void hal_setAlarmLED(bool ein) {
  digitalWrite(PIN_LED_ALARM, ein ? HIGH : LOW);
}

void hal_setSensor1LED(bool ein) {
  digitalWrite(PIN_LED_SENS1, ein ? HIGH : LOW);
}

void hal_setSensor2LED(bool ein) {
  digitalWrite(PIN_LED_SENS2, ein ? HIGH : LOW);
}

void hal_setStatusLED(bool ein) {
  digitalWrite(LED_BUILTIN, ein ? HIGH : LOW);
}
