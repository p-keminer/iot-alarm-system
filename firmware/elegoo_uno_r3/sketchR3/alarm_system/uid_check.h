// =============================================================================
// uid_check.h - Pruefung erlaubter RFID-UIDs
// =============================================================================

#ifndef UID_CHECK_H
#define UID_CHECK_H

#include <Arduino.h>

// Prueft ob UID in der Whitelist ist
bool uid_istErlaubt(const byte* uid, byte laenge);

#endif
