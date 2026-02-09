// =============================================================================
// alarm_fsm.cpp - Zustandsautomat fuer Alarmlogik
// =============================================================================

#include "alarm_fsm.h"
#include "hal_io.h"
#include "hal_comm.h"

// Aktueller Zustand
static AlarmState zustand = STATE_UNSCHARF;


// Zustandswechsel mit Entry/Exit-Aktionen
static void wechsleZu(AlarmState neu) {
  if (zustand == neu) return;
  
  // Exit-Aktion alter Zustand
  if (zustand == STATE_ALARM) {
    hal_buzzerAus();
    hal_comm_sendeEvent(EVT_ALARM_OFF);
  }
  
  // Entry-Aktion neuer Zustand
  switch (neu) {
    case STATE_UNSCHARF:
      hal_comm_sendeEvent(EVT_ALARM_UNSCHARF);
      break;
    case STATE_SCHARF:
      hal_comm_sendeEvent(EVT_ALARM_SCHARF);
      break;
    case STATE_ALARM:
      hal_buzzerEin();
      hal_comm_sendeEvent(EVT_ALARM_ON);
      break;
    default:
      break;
  }
  
  zustand = neu;
}


void fsm_init() {
  zustand = STATE_UNSCHARF;
}


void fsm_verarbeite(AlarmInput eingabe) {
  if (eingabe == INPUT_KEINE) return;
  
  switch (zustand) {
    
    // === UNSCHARF ===
    case STATE_UNSCHARF:
      if (eingabe == INPUT_KARTE_ERLAUBT || eingabe == INPUT_CMD_SCHARF) {
        wechsleZu(STATE_SCHARF);
      }
      else if (eingabe == INPUT_ERZWINGE_ALARM) {
        wechsleZu(STATE_ALARM);
        hal_comm_sendeEvent(EVT_ALARM_ERZWUNGEN);
      }
      break;
    
    // === SCHARF ===
    case STATE_SCHARF:
      if (eingabe == INPUT_KARTE_ERLAUBT || 
          eingabe == INPUT_CMD_UNSCHARF || 
          eingabe == INPUT_ALARM_STOP) {
        wechsleZu(STATE_UNSCHARF);
      }
      else if (eingabe == INPUT_SENSOR_OFFEN || eingabe == INPUT_ERZWINGE_ALARM) {
        wechsleZu(STATE_ALARM);
      }
      break;
    
    // === ALARM ===
    case STATE_ALARM:
      if (eingabe == INPUT_KARTE_ERLAUBT || 
          eingabe == INPUT_CMD_UNSCHARF || 
          eingabe == INPUT_ALARM_STOP) {
        wechsleZu(STATE_UNSCHARF);
        hal_comm_sendeEvent(EVT_ALARM_GESTOPPT);
      }
      break;
    
    // === STOERUNG ===
    case STATE_STOERUNG:
      // Nur durch Reset verlassbar
      break;
  }
}


AlarmState fsm_getZustand() {
  return zustand;
}

bool fsm_istScharf() {
  return (zustand == STATE_SCHARF || zustand == STATE_ALARM);
}

bool fsm_istAusgeloest() {
  return (zustand == STATE_ALARM);
}
