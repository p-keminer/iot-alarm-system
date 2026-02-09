// =============================================================================
// hal_comm.cpp - Hardwareabstraktion fuer serielle Kommunikation
// =============================================================================

#include "hal_comm.h"

// Kommando-Strings im Flash (spart RAM)
static const char STR_HB_ACK[] PROGMEM = "HB_ACK";
static const char STR_ERZWINGE[] PROGMEM = "ERZWINGE_ALARM";
static const char STR_STOP[] PROGMEM = "ALARM_STOP";
static const char STR_SCHARF[] PROGMEM = "SCHARF";
static const char STR_UNSCHARF[] PROGMEM = "UNSCHARF";
static const char STR_STATUS[] PROGMEM = "STATUS";
static const char STR_REBOOT[] PROGMEM = "REBOOT";

// Eingabepuffer
static const uint8_t PUFFER_GROESSE = 20;
static char puffer[PUFFER_GROESSE];
static uint8_t pufferIdx = 0;


void hal_comm_init(uint32_t baudrate) {
  Serial.begin(baudrate);
  pufferIdx = 0;
}


void hal_comm_sendeEvent(KommEvent evt) {
  switch (evt) {
    case EVT_SYSTEM_BEREIT:       Serial.println(F("STATUS:SYSTEM_BEREIT")); break;
    case EVT_ALARM_SCHARF:        Serial.println(F("STATUS:ALARM_SCHARF")); break;
    case EVT_ALARM_UNSCHARF:      Serial.println(F("STATUS:ALARM_UNSCHARF")); break;
    case EVT_ALARM_ON:            Serial.println(F("ALARM_ON")); break;
    case EVT_ALARM_OFF:           Serial.println(F("ALARM_OFF")); break;
    case EVT_ALARM_ERZWUNGEN:     Serial.println(F("STATUS:ALARM_ERZWUNGEN")); break;
    case EVT_ALARM_GESTOPPT:      Serial.println(F("STATUS:ALARM_GESTOPPT")); break;
    case EVT_VERBINDUNG_AKTIV:    Serial.println(F("STATUS:VERBINDUNG_AKTIV")); break;
    case EVT_VERBINDUNG_VERLOREN: Serial.println(F("STATUS:VERBINDUNG_VERLOREN")); break;
    case EVT_HEARTBEAT:           Serial.println(F("HB")); break;
    case EVT_REBOOT:              Serial.println(F("STATUS:REBOOT")); break;
  }
}


void hal_comm_sendeHeartbeat() {
  hal_comm_sendeEvent(EVT_HEARTBEAT);
}


void hal_comm_sendeStatus(bool scharf, bool ausgeloest, 
                          bool tuerOffen1, bool tuerOffen2, 
                          bool verbindungOk) {
  Serial.print(F("STATUS:SCHARF="));
  Serial.print(scharf ? '1' : '0');
  Serial.print(F(",AUSGELOEST="));
  Serial.print(ausgeloest ? '1' : '0');
  Serial.print(F(",TUER1="));
  Serial.print(tuerOffen1 ? F("OFFEN") : F("ZU"));
  Serial.print(F(",TUER2="));
  Serial.print(tuerOffen2 ? F("OFFEN") : F("ZU"));
  Serial.print(F(",VERBINDUNG="));
  Serial.println(verbindungOk ? F("OK") : F("VERLOREN"));
}


// Vergleich mit PROGMEM-String (ohne RAM-Kopie)
static bool gleichPM(const char* ram, const char* progmem) {
  return strcmp_P(ram, progmem) == 0;
}


// Eingabe zu Kommando-Enum
static KommKommando parseKommando(const char* eingabe) {
  // Kurzkommandos
  if (eingabe[0] == '1' && eingabe[1] == '\0') return CMD_SCHARF;
  if (eingabe[0] == '0' && eingabe[1] == '\0') return CMD_UNSCHARF;
  
  // Lange Kommandos
  if (gleichPM(eingabe, STR_HB_ACK))   return CMD_HEARTBEAT_ACK;
  if (gleichPM(eingabe, STR_ERZWINGE)) return CMD_ERZWINGE_ALARM;
  if (gleichPM(eingabe, STR_STOP))     return CMD_ALARM_STOP;
  if (gleichPM(eingabe, STR_SCHARF))   return CMD_SCHARF;
  if (gleichPM(eingabe, STR_UNSCHARF)) return CMD_UNSCHARF;
  if (gleichPM(eingabe, STR_STATUS))   return CMD_STATUS;
  if (gleichPM(eingabe, STR_REBOOT))   return CMD_REBOOT;
  
  return CMD_KEINE;
}


KommKommando hal_comm_leseKommando() {
  while (Serial.available() > 0) {
    char c = Serial.read();
    
    // Zeilenende = Kommando komplett
    if (c == '\n' || c == '\r') {
      if (pufferIdx > 0) {
        puffer[pufferIdx] = '\0';
        pufferIdx = 0;
        return parseKommando(puffer);
      }
    }
    // Zeichen sammeln
    else if (pufferIdx < PUFFER_GROESSE - 1) {
      puffer[pufferIdx++] = c;
    }
    // Ueberlauf -> Reset
    else {
      pufferIdx = 0;
    }
  }
  return CMD_KEINE;
}
