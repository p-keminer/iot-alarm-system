// =============================================================================
// uid_check.cpp - Pruefung erlaubter RFID-UIDs
// =============================================================================

#include "uid_check.h"
#include "hal_rfid.h"

// Lokale UIDs gehoeren nicht ins Repository. Eine private Datei neben diesem
// Sketch kann ALARM_UID_WHITELIST_ENTRIES definieren; ohne sie bleibt das
// System absichtlich geschlossen.
#if defined(__has_include)
#  if __has_include("uid_whitelist.local.h")
#    include "uid_whitelist.local.h"
#  endif
#endif

// Erlaubte UIDs hier eintragen
typedef struct {
  byte uid[HAL_RFID_MAX_UID_LEN];
  byte laenge;
} ErlaubteUID;

#ifndef ALARM_UID_WHITELIST_ENTRIES
// Fail-safe-Standardeintrag: Laenge 0 kann keiner gelesenen Karte
// entsprechen. Insbesondere ist 00:00:00:00 nicht mehr freigeschaltet.
#define ALARM_UID_WHITELIST_ENTRIES { {{0}, 0} }
#endif

static const ErlaubteUID WHITELIST[] = ALARM_UID_WHITELIST_ENTRIES;

static const uint8_t WHITELIST_GROESSE = sizeof(WHITELIST) / sizeof(WHITELIST[0]);


bool uid_istErlaubt(const byte* uid, byte laenge) {
  if (uid == NULL || laenge == 0 || laenge > HAL_RFID_MAX_UID_LEN) {
    return false;
  }

  for (uint8_t i = 0; i < WHITELIST_GROESSE; i++) {
    // Laenge muss stimmen
    if (WHITELIST[i].laenge == 0 ||
        WHITELIST[i].laenge > HAL_RFID_MAX_UID_LEN ||
        laenge != WHITELIST[i].laenge) continue;
    
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
