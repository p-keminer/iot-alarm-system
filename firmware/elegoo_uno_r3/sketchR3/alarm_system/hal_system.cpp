// =============================================================================
// hal_system.cpp - Hardwareabstraktion fuer Systemfunktionen
// =============================================================================

#include "hal_system.h"
#include "hal_io.h"
#include "hal_comm.h"
#include <avr/wdt.h>


void hal_vorReboot() {
  // Ausgaenge sicher abschalten
  hal_buzzerAus();
  hal_setAlarmLED(false);
  hal_setSensor1LED(false);
  hal_setSensor2LED(false);
  hal_setStatusLED(false);
  
  // Bestaetigung senden
  hal_comm_sendeEvent(EVT_REBOOT);
  delay(100);  // Warten bis gesendet
}


void hal_reboot() {
  hal_vorReboot();
  
  // Watchdog auf 15ms setzen und warten
  wdt_enable(WDTO_15MS);
  while (true) {}
}
