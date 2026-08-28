// =============================================================================
// alarm_fsm.cpp - Zustandsautomat mit fail-secure EEPROM-Persistenz
// =============================================================================

#include "alarm_fsm.h"
#include "hal_io.h"
#include "hal_comm.h"
#include <EEPROM.h>

// Zwei alternierende Records verhindern, dass ein Stromausfall waehrend eines
// EEPROM-Schreibvorgangs den letzten gueltigen Zustand zerstoert. Das
// Commit-Byte wird immer zuletzt geschrieben; EEPROM.update vermeidet
// unnoetigen Verschleiss bei unveraenderten Bytes.
static const uint8_t EEPROM_SLOT_GROESSE = 7;
static const uint8_t EEPROM_SLOT_ANZAHL = 2;
static const uint8_t EEPROM_COMMIT_OFFSET = 6;
static const uint16_t EEPROM_MAGIC = 0xA17E;
static const uint8_t EEPROM_VERSION = 1;
static const uint8_t EEPROM_COMMIT = 0xA5;

static AlarmState zustand = STATE_UNSCHARF;
static uint8_t aktiverSlot = 0xFF;
static uint8_t aktiveSequenz = 0;

typedef enum {
  LADE_FRISCH,
  LADE_GUELTIG,
  LADE_KORRUPT
} LadeErgebnis;


static uint8_t crc8(const uint8_t* daten, uint8_t laenge) {
  uint8_t crc = 0xFF;
  for (uint8_t i = 0; i < laenge; i++) {
    crc ^= daten[i];
    for (uint8_t bit = 0; bit < 8; bit++) {
      crc = (crc & 0x80) ? static_cast<uint8_t>((crc << 1) ^ 0x31)
                         : static_cast<uint8_t>(crc << 1);
    }
  }
  return crc;
}


static int slotAdresse(uint8_t slot) {
  return static_cast<int>(slot) * EEPROM_SLOT_GROESSE;
}


static void leseSlot(uint8_t slot, uint8_t* record) {
  int adresse = slotAdresse(slot);
  for (uint8_t i = 0; i < EEPROM_SLOT_GROESSE; i++) {
    record[i] = EEPROM.read(adresse + i);
  }
}


static bool recordIstLeer(const uint8_t* record) {
  for (uint8_t i = 0; i < EEPROM_SLOT_GROESSE; i++) {
    if (record[i] != 0xFF) return false;
  }
  return true;
}


static bool zustandIstPersistierbar(uint8_t wert) {
  return wert == STATE_UNSCHARF || wert == STATE_SCHARF || wert == STATE_ALARM;
}


static bool recordIstGueltig(const uint8_t* record) {
  uint16_t magic = static_cast<uint16_t>(record[0]) |
                   (static_cast<uint16_t>(record[1]) << 8);
  return record[EEPROM_COMMIT_OFFSET] == EEPROM_COMMIT &&
         magic == EEPROM_MAGIC &&
         record[2] == EEPROM_VERSION &&
         zustandIstPersistierbar(record[4]) &&
         record[5] == crc8(record, 5);
}


static bool sequenzIstNeuer(uint8_t kandidat, uint8_t referenz) {
  return static_cast<int8_t>(kandidat - referenz) > 0;
}


static bool schreibeSlot(uint8_t slot, AlarmState neu, uint8_t sequenz) {
  uint8_t record[EEPROM_SLOT_GROESSE];
  record[0] = static_cast<uint8_t>(EEPROM_MAGIC & 0xFF);
  record[1] = static_cast<uint8_t>(EEPROM_MAGIC >> 8);
  record[2] = EEPROM_VERSION;
  record[3] = sequenz;
  record[4] = static_cast<uint8_t>(neu);
  record[5] = crc8(record, 5);
  record[6] = EEPROM_COMMIT;

  int adresse = slotAdresse(slot);
  EEPROM.update(adresse + EEPROM_COMMIT_OFFSET, 0x00);
  for (uint8_t i = 0; i < EEPROM_COMMIT_OFFSET; i++) {
    EEPROM.update(adresse + i, record[i]);
  }
  EEPROM.update(adresse + EEPROM_COMMIT_OFFSET, EEPROM_COMMIT);

  uint8_t pruefung[EEPROM_SLOT_GROESSE];
  leseSlot(slot, pruefung);
  return recordIstGueltig(pruefung) &&
         pruefung[3] == sequenz &&
         pruefung[4] == static_cast<uint8_t>(neu);
}


static LadeErgebnis ladeZustand(AlarmState* geladen) {
  uint8_t records[EEPROM_SLOT_ANZAHL][EEPROM_SLOT_GROESSE];
  bool leer[EEPROM_SLOT_ANZAHL];
  bool gueltig[EEPROM_SLOT_ANZAHL];

  for (uint8_t slot = 0; slot < EEPROM_SLOT_ANZAHL; slot++) {
    leseSlot(slot, records[slot]);
    leer[slot] = recordIstLeer(records[slot]);
    gueltig[slot] = recordIstGueltig(records[slot]);
  }

  if (leer[0] && leer[1]) {
    aktiverSlot = 0xFF;
    aktiveSequenz = 0;
    *geladen = STATE_UNSCHARF;
    return LADE_FRISCH;
  }

  // Nicht-leere, aber ungueltige Daten bedeuten unterbrochenen Schreibvorgang
  // oder Korruption. Auch wenn der zweite Slot noch gueltig ist, wird bewusst
  // ALARM gewaehlt statt moeglicherweise auf UNSCHARF zurueckzufallen.
  if ((!leer[0] && !gueltig[0]) || (!leer[1] && !gueltig[1])) {
    aktiverSlot = 0xFF;
    aktiveSequenz = 0;
    *geladen = STATE_ALARM;
    return LADE_KORRUPT;
  }

  uint8_t slot;
  if (gueltig[0] && gueltig[1]) {
    slot = sequenzIstNeuer(records[1][3], records[0][3]) ? 1 : 0;
  } else {
    slot = gueltig[0] ? 0 : 1;
  }

  aktiverSlot = slot;
  aktiveSequenz = records[slot][3];
  *geladen = static_cast<AlarmState>(records[slot][4]);
  return LADE_GUELTIG;
}


static bool persistiereZustand(AlarmState neu) {
  if (!zustandIstPersistierbar(static_cast<uint8_t>(neu))) return false;

  uint8_t zielSlot = (aktiverSlot == 0xFF) ? 0 : static_cast<uint8_t>(1 - aktiverSlot);
  uint8_t sequenz = (aktiverSlot == 0xFF) ? 1 : static_cast<uint8_t>(aktiveSequenz + 1);
  if (!schreibeSlot(zielSlot, neu, sequenz)) return false;

  aktiverSlot = zielSlot;
  aktiveSequenz = sequenz;
  return true;
}


static void repariereAlsAlarm() {
  if (schreibeSlot(0, STATE_ALARM, 1)) {
    aktiverSlot = 0;
    aktiveSequenz = 1;
    if (schreibeSlot(1, STATE_ALARM, 2)) {
      aktiverSlot = 1;
      aktiveSequenz = 2;
    }
  }
}


static void wendeEntryAktionenAn(AlarmState neu) {
  switch (neu) {
    case STATE_UNSCHARF:
      hal_buzzerAus();
      hal_comm_sendeEvent(EVT_ALARM_UNSCHARF);
      break;
    case STATE_SCHARF:
      hal_buzzerAus();
      hal_comm_sendeEvent(EVT_ALARM_SCHARF);
      break;
    case STATE_ALARM:
      hal_buzzerEin();
      hal_comm_sendeEvent(EVT_ALARM_ON);
      break;
    default:
      // Ein unbekannter Zustand darf nie still oder unscharf werden.
      hal_buzzerEin();
      hal_comm_sendeEvent(EVT_ALARM_ON);
      break;
  }
}


static void wechsleZu(AlarmState neu) {
  if (zustand == neu) return;

  AlarmState ziel = neu;
  if (!persistiereZustand(ziel)) {
    ziel = STATE_ALARM;
    persistiereZustand(ziel);
    hal_comm_sendeEvent(EVT_PERSISTENZ_FEHLER);
  }

  if (zustand == STATE_ALARM && ziel != STATE_ALARM) {
    hal_buzzerAus();
    hal_comm_sendeEvent(EVT_ALARM_OFF);
  }

  zustand = ziel;
  wendeEntryAktionenAn(zustand);
}


void fsm_init() {
  AlarmState geladen = STATE_UNSCHARF;
  LadeErgebnis ergebnis = ladeZustand(&geladen);
  zustand = geladen;

  if (ergebnis == LADE_KORRUPT) {
    repariereAlsAlarm();
    hal_comm_sendeEvent(EVT_PERSISTENZ_FEHLER);
  }

  // Boot stellt nicht nur die Variable, sondern auch Buzzer und das serielle
  // Ereignis wieder her. Dadurch startet die Kamera nach einem Reset im
  // laufenden Alarm erneut.
  wendeEntryAktionenAn(zustand);
}


void fsm_verarbeite(AlarmInput eingabe) {
  // Ein RAM-Fehler darf auch ohne neues externes Ereignis nie als UNSCHARF
  // weiterlaufen. Der Zustand wird unmittelbar als ALARM repariert, dauerhaft
  // gespeichert und auf die physischen Ausgaenge angewendet.
  if (zustand == STATE_STOERUNG ||
      (zustand != STATE_UNSCHARF && zustand != STATE_SCHARF &&
       zustand != STATE_ALARM)) {
    zustand = STATE_STOERUNG;
    wechsleZu(STATE_ALARM);
    return;
  }

  if (eingabe == INPUT_KEINE) return;

  switch (zustand) {
    case STATE_UNSCHARF:
      if (eingabe == INPUT_KARTE_ERLAUBT || eingabe == INPUT_CMD_SCHARF) {
        wechsleZu(STATE_SCHARF);
      } else if (eingabe == INPUT_ERZWINGE_ALARM) {
        wechsleZu(STATE_ALARM);
        hal_comm_sendeEvent(EVT_ALARM_ERZWUNGEN);
      }
      break;

    case STATE_SCHARF:
      if (eingabe == INPUT_KARTE_ERLAUBT ||
          eingabe == INPUT_CMD_UNSCHARF ||
          eingabe == INPUT_ALARM_STOP) {
        wechsleZu(STATE_UNSCHARF);
      } else if (eingabe == INPUT_SENSOR_OFFEN || eingabe == INPUT_ERZWINGE_ALARM) {
        wechsleZu(STATE_ALARM);
      }
      break;

    case STATE_ALARM:
      if (eingabe == INPUT_KARTE_ERLAUBT ||
          eingabe == INPUT_CMD_UNSCHARF ||
          eingabe == INPUT_ALARM_STOP) {
        wechsleZu(STATE_UNSCHARF);
        if (zustand == STATE_UNSCHARF) {
          hal_comm_sendeEvent(EVT_ALARM_GESTOPPT);
        }
      }
      break;

    case STATE_STOERUNG:
    default:
      // Defensive zweite Barriere, falls der Zustand zwischen Vorpruefung und
      // Switch korrumpiert wird.
      zustand = STATE_STOERUNG;
      wechsleZu(STATE_ALARM);
      break;
  }
}


AlarmState fsm_getZustand() {
  return zustand;
}

bool fsm_istScharf() {
  // Nur ein explizit gueltiger UNSCHARF-Zustand darf als unscharf gemeldet
  // werden. STOERUNG und unbekannte RAM-Werte sind fail-secure scharf.
  return zustand != STATE_UNSCHARF;
}

bool fsm_istAusgeloest() {
  // SCHARF wartet noch auf einen Sensor; alle Fehler-/Alarmzustaende werden
  // dagegen als ausgeloest gemeldet.
  return zustand != STATE_UNSCHARF && zustand != STATE_SCHARF;
}
