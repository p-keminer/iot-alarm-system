// =============================================================================
// hal_time.cpp - Hardwareabstraktion fuer Zeitfunktionen
// =============================================================================

#include "hal_time.h"

uint32_t hal_zeitMs() {
  return millis();
}

bool hal_zeitAbgelaufen(uint32_t startzeit, uint32_t intervallMs) {
  return (millis() - startzeit) >= intervallMs;
}
