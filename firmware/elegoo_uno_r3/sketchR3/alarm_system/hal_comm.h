// =============================================================================
// hal_comm.h - Hardwareabstraktion fuer serielle Kommunikation
// =============================================================================

#ifndef HAL_COMM_H
#define HAL_COMM_H

#include <Arduino.h>

// Ereignisse die gesendet werden
typedef enum {
  EVT_SYSTEM_BEREIT,
  EVT_ALARM_SCHARF,
  EVT_ALARM_UNSCHARF,
  EVT_ALARM_ON,
  EVT_ALARM_OFF,
  EVT_ALARM_ERZWUNGEN,
  EVT_ALARM_GESTOPPT,
  EVT_VERBINDUNG_AKTIV,
  EVT_VERBINDUNG_VERLOREN,
  EVT_HEARTBEAT,
  EVT_REBOOT
} KommEvent;

// Kommandos die empfangen werden
typedef enum {
  CMD_KEINE,
  CMD_HEARTBEAT_ACK,
  CMD_ERZWINGE_ALARM,
  CMD_ALARM_STOP,
  CMD_SCHARF,
  CMD_UNSCHARF,
  CMD_STATUS,
  CMD_REBOOT
} KommKommando;

// Initialisierung
void hal_comm_init(uint32_t baudrate);

// Ereignis senden
void hal_comm_sendeEvent(KommEvent evt);
void hal_comm_sendeHeartbeat();
void hal_comm_sendeStatus(bool scharf, bool ausgeloest, 
                          bool tuerOffen1, bool tuerOffen2, 
                          bool verbindungOk);

// Kommando empfangen (non-blocking)
KommKommando hal_comm_leseKommando();

#endif
