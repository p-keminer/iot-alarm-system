// =============================================================================
// uid_check.cpp - Pruefung erlaubter RFID-UIDs
// =============================================================================

#include "uid_check.h"

// Erlaubte UIDs hier eintragen
typedef struct {
  byte uid[7];
  byte laenge;
} ErlaubteUID;

static const ErlaubteUID WHITELIST[] = {
  {{0x00, 0x00, 0x00, 0x00}, 4},    // <- UID 1 eintragen
  {{0x00, 0x00, 0x00, 0x00}, 4}     // <- UID 2 eintragen
};

static const uint8_t WHITELIST_GROESSE = sizeof(WHITELIST) / sizeof(WHITELIST[0]);


bool uid_istErlaubt(const byte* uid, byte laenge) {
  for (uint8_t i = 0; i < WHITELIST_GROESSE; i++) {
    // Laenge muss stimmen
    if (laenge != WHITELIST[i].laenge) continue;
    
    // Bytes vergleichen
    bool gleich = true;
    for (byte j = 0; j < laenge; j++) {
      if (uid[j] != WHITELIST[i].uid[j]) {
        gleich = false;
        break;
      }
    }
    if (gleich) return true;
  }
  return false;
}
