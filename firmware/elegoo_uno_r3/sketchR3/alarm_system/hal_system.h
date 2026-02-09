// =============================================================================
// hal_system.h - Hardwareabstraktion fuer Systemfunktionen
// =============================================================================

#ifndef HAL_SYSTEM_H
#define HAL_SYSTEM_H

#include <Arduino.h>

// Software-Reset ausfuehren
void hal_reboot();

// Aufraeumen vor Reset (intern verwendet)
void hal_vorReboot();

#endif
