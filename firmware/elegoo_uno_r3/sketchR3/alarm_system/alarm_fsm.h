// =============================================================================
// alarm_fsm.h - Zustandsautomat fuer Alarmlogik
// =============================================================================

#ifndef ALARM_FSM_H
#define ALARM_FSM_H

#include <Arduino.h>

// Systemzustaende
typedef enum {
  STATE_UNSCHARF,       // Alarm aus
  STATE_SCHARF,         // Alarm aktiv, wartet
  STATE_ALARM,          // Alarm ausgeloest
  STATE_STOERUNG        // Fehler (optional)
} AlarmState;

// Eingabe-Events
typedef enum {
  INPUT_KEINE,
  INPUT_KARTE_ERLAUBT,
  INPUT_SENSOR_OFFEN,
  INPUT_ERZWINGE_ALARM,
  INPUT_ALARM_STOP,
  INPUT_CMD_SCHARF,
  INPUT_CMD_UNSCHARF
} AlarmInput;

// Initialisierung
void fsm_init();

// Event verarbeiten
void fsm_verarbeite(AlarmInput eingabe);

// Zustand abfragen
AlarmState fsm_getZustand();
bool fsm_istScharf();
bool fsm_istAusgeloest();

#endif
