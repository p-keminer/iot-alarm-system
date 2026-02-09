// =============================================================================
// hal_time.h - Hardwareabstraktion fuer Zeitfunktionen
// =============================================================================

#ifndef HAL_TIME_H
#define HAL_TIME_H

#include <Arduino.h>

// Aktuelle Zeit in Millisekunden
uint32_t hal_zeitMs();

// Prueft ob Intervall abgelaufen ist
bool hal_zeitAbgelaufen(uint32_t startzeit, uint32_t intervallMs);

#endif
