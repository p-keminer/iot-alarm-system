// =============================================================================
// hal_rfid.h - Hardwareabstraktion fuer RFID-Leser
// =============================================================================

#ifndef HAL_RFID_H
#define HAL_RFID_H

#include <Arduino.h>

// Max. UID-Laenge (4 oder 7 Byte je nach Kartentyp)
#define HAL_RFID_MAX_UID_LEN 7

// Rueckgabewerte beim Kartenlesen
typedef enum {
  RFID_KEINE_KARTE,
  RFID_KARTE_GELESEN,
  RFID_LESEFEHLER
} RfidStatus;

// Initialisierung
void hal_rfid_init();

// Karte lesen, UID in Puffer schreiben
RfidStatus hal_rfid_leseKarte(byte* uidPuffer, byte* uidLaenge);

// Kommunikation mit Karte beenden
void hal_rfid_beenden();

#endif
