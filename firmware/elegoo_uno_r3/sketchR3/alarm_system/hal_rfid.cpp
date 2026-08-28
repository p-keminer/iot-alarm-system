// =============================================================================
// hal_rfid.cpp - Hardwareabstraktion fuer RFID-Leser
// =============================================================================

#include "hal_rfid.h"
#include <SPI.h>
#include <MFRC522.h>

// Pin-Belegung
static const uint8_t PIN_RST = 9;
static const uint8_t PIN_SS = 10;

// RFID-Objekt (intern)
static MFRC522 rfidLeser(PIN_SS, PIN_RST);


void hal_rfid_init() {
  SPI.begin();
  rfidLeser.PCD_Init();
}


RfidStatus hal_rfid_leseKarte(byte* uidPuffer, byte* uidLaenge) {
  if (uidPuffer == NULL || uidLaenge == NULL) {
    return RFID_LESEFEHLER;
  }

  *uidLaenge = 0;

  // Keine Karte in Reichweite
  if (!rfidLeser.PICC_IsNewCardPresent()) {
    return RFID_KEINE_KARTE;
  }
  
  // Karte da aber nicht lesbar
  if (!rfidLeser.PICC_ReadCardSerial()) {
    return RFID_LESEFEHLER;
  }
  
  // Eine ungueltige Laenge nie an die Whitelist-Pruefung weiterreichen. Das
  // verhindert insbesondere Zugriffe hinter das Ende des UID-Puffers.
  if (rfidLeser.uid.size == 0 || rfidLeser.uid.size > HAL_RFID_MAX_UID_LEN) {
    hal_rfid_beenden();
    return RFID_LESEFEHLER;
  }

  // UID auslesen
  *uidLaenge = rfidLeser.uid.size;
  for (byte i = 0; i < rfidLeser.uid.size; i++) {
    uidPuffer[i] = rfidLeser.uid.uidByte[i];
  }
  
  return RFID_KARTE_GELESEN;
}


void hal_rfid_beenden() {
  rfidLeser.PICC_HaltA();
  rfidLeser.PCD_StopCrypto1();
}
