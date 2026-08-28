#include <cassert>

#include "EEPROM.h"
#include "../../firmware/elegoo_uno_r3/sketchR3/alarm_system/hal_comm.h"

EEPROMClass EEPROM;

static bool testBuzzerActive = false;
static KommEvent testLastEvent = EVT_SYSTEM_BEREIT;

void hal_buzzerEin() { testBuzzerActive = true; }
void hal_buzzerAus() { testBuzzerActive = false; }
void hal_comm_sendeEvent(KommEvent event) { testLastEvent = event; }

// Die echte Implementierung wird als eine Translation Unit mit den HAL-Stubs
// kompiliert. Dadurch prueft der Test die reale Zustandslogik statt eine Kopie.
#include "../../firmware/elegoo_uno_r3/sketchR3/alarm_system/alarm_fsm.cpp"

static void expectFailSecureRepair(AlarmState brokenState) {
  EEPROM.reset();
  aktiverSlot = 0xFF;
  aktiveSequenz = 0;
  zustand = brokenState;
  testBuzzerActive = false;
  testLastEvent = EVT_SYSTEM_BEREIT;

  assert(fsm_istScharf());
  assert(fsm_istAusgeloest());
  fsm_verarbeite(INPUT_KEINE);
  assert(fsm_getZustand() == STATE_ALARM);
  assert(fsm_istScharf());
  assert(fsm_istAusgeloest());
  assert(testBuzzerActive);
  assert(testLastEvent == EVT_ALARM_ON);

  AlarmState restored = STATE_UNSCHARF;
  assert(ladeZustand(&restored) == LADE_GUELTIG);
  assert(restored == STATE_ALARM);
}

int main() {
  zustand = STATE_UNSCHARF;
  assert(!fsm_istScharf());
  assert(!fsm_istAusgeloest());

  zustand = STATE_SCHARF;
  assert(fsm_istScharf());
  assert(!fsm_istAusgeloest());

  expectFailSecureRepair(STATE_STOERUNG);
  expectFailSecureRepair(static_cast<AlarmState>(99));
  return 0;
}
