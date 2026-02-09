// =============================================================================
// hal_io.h - Hardwareabstraktion fuer Ein-/Ausgaenge
// =============================================================================

#ifndef HAL_IO_H
#define HAL_IO_H

#include <Arduino.h>

// Initialisierung aller Pins
void hal_io_init();

// Tuersensoren abfragen (true = offen)
bool hal_istTuerOffen1();
bool hal_istTuerOffen2();

// Buzzer steuern
void hal_buzzerEin();
void hal_buzzerAus();
void hal_setBuzzer(bool ein);

// LEDs steuern
void hal_setAlarmLED(bool ein);
void hal_setSensor1LED(bool ein);
void hal_setSensor2LED(bool ein);
void hal_setStatusLED(bool ein);

#endif
