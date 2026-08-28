// =============================================================================
// alarm_system.ino - Alarmanlage mit RFID, Sensoren und Pico2W-Anbindung
// =============================================================================
//
// Aufbau:
//   hal_*.h/cpp   - Hardwareabstraktion (Pins, RFID, Kommunikation)
//   alarm_fsm     - Zustandsautomat (UNSCHARF/SCHARF/ALARM)
//   uid_check     - RFID-Whitelist
//
// =============================================================================

#include "hal_io.h"
#include "hal_rfid.h"
#include "hal_comm.h"
#include "hal_time.h"
#include "hal_system.h"
#include "alarm_fsm.h"
#include "sensor_logic.h"
#include "uid_check.h"
#include <avr/wdt.h> 

// =============================================================================
// Einstellungen
// =============================================================================

// Diese Werte koennen bei Bedarf als Build-Flags ueberschrieben werden, zum
// Beispiel mit -DALARM_HEARTBEAT_INTERVAL_MS=10000UL. Unsichere Kombinationen
// werden bereits beim Kompilieren abgelehnt.
#ifndef ALARM_SERIAL_BAUDRATE
#define ALARM_SERIAL_BAUDRATE 9600UL
#endif

#ifndef ALARM_HEARTBEAT_INTERVAL_MS
#define ALARM_HEARTBEAT_INTERVAL_MS 5000UL
#endif

#ifndef ALARM_HEARTBEAT_TIMEOUT_MS
#define ALARM_HEARTBEAT_TIMEOUT_MS 15000UL
#endif

#ifndef ALARM_RFID_COOLDOWN_MS
#define ALARM_RFID_COOLDOWN_MS 2000UL
#endif

#if ALARM_SERIAL_BAUDRATE < 1200UL || ALARM_SERIAL_BAUDRATE > 115200UL
#error "ALARM_SERIAL_BAUDRATE muss zwischen 1200 und 115200 liegen"
#endif

#if ALARM_HEARTBEAT_INTERVAL_MS < 1000UL
#error "ALARM_HEARTBEAT_INTERVAL_MS muss mindestens 1000 ms betragen"
#endif

#if ALARM_HEARTBEAT_TIMEOUT_MS <= ALARM_HEARTBEAT_INTERVAL_MS
#error "ALARM_HEARTBEAT_TIMEOUT_MS muss groesser als das Heartbeat-Intervall sein"
#endif

#if ALARM_RFID_COOLDOWN_MS < 250UL
#error "ALARM_RFID_COOLDOWN_MS muss mindestens 250 ms betragen"
#endif

static const uint32_t BAUDRATE = ALARM_SERIAL_BAUDRATE;
static const uint32_t HEARTBEAT_INTERVALL_MS = ALARM_HEARTBEAT_INTERVAL_MS;
static const uint32_t WATCHDOG_TIMEOUT_MS = ALARM_HEARTBEAT_TIMEOUT_MS;
static const uint32_t RFID_SPERRZEIT_MS = ALARM_RFID_COOLDOWN_MS;


// =============================================================================
// Variablen Heartbeat-Watchdog
// =============================================================================

static uint32_t letzterHeartbeat = 0;
static uint32_t letzteAntwort = 0;
static bool warteAufAntwort = false;
static bool verbindungAktiv = true;


// =============================================================================
// Variablen RFID
// =============================================================================

static uint32_t letzteRfidLesung = 0;
static bool karteWarDa = false;


// =============================================================================
// Heartbeat-Watchdog Funktionen
// =============================================================================

// Heartbeat senden (alle 5s)
void watchdog_sende() {
  uint32_t jetzt = hal_zeitMs();
  
  if (!warteAufAntwort && hal_zeitAbgelaufen(letzterHeartbeat, HEARTBEAT_INTERVALL_MS)) {
    hal_comm_sendeHeartbeat();
    letzterHeartbeat = jetzt;
    warteAufAntwort = true;
  }
}

// Timeout pruefen
void watchdog_pruefe() {
  if (warteAufAntwort && hal_zeitAbgelaufen(letzterHeartbeat, WATCHDOG_TIMEOUT_MS)) {
    if (verbindungAktiv) {
      verbindungAktiv = false;
      hal_comm_sendeEvent(EVT_VERBINDUNG_VERLOREN);
      hal_setStatusLED(true);
    }
    warteAufAntwort = false;
  }
}

// Antwort vom Pico2W erhalten
void watchdog_bestaetigt() {
  letzteAntwort = hal_zeitMs();
  warteAufAntwort = false;
  
  if (!verbindungAktiv) {
    verbindungAktiv = true;
    hal_comm_sendeEvent(EVT_VERBINDUNG_AKTIV);
    hal_setStatusLED(false);
  }
}


// =============================================================================
// RFID verarbeiten
// =============================================================================

AlarmInput rfid_verarbeite() {
  uint32_t jetzt = hal_zeitMs();
  
  // Sperrzeit aktiv -> ignorieren
  if (!hal_zeitAbgelaufen(letzteRfidLesung, RFID_SPERRZEIT_MS)) {
    return INPUT_KEINE;
  }
  
  // Karte lesen
  byte uid[HAL_RFID_MAX_UID_LEN];
  byte uidLen = 0;
  RfidStatus status = hal_rfid_leseKarte(uid, &uidLen);
  
  if (status == RFID_KEINE_KARTE) {
    karteWarDa = false;
    return INPUT_KEINE;
  }
  
  if (status == RFID_LESEFEHLER) {
    return INPUT_KEINE;
  }
  
  // Karte gelesen -> Sperrzeit starten
  letzteRfidLesung = jetzt;
  hal_rfid_beenden();
  
  // Entprellung: nur reagieren wenn Karte neu
  if (karteWarDa) {
    return INPUT_KEINE;
  }
  karteWarDa = true;
  
  // UID pruefen
  if (uid_istErlaubt(uid, uidLen)) {
    return INPUT_KARTE_ERLAUBT;
  }
  
  return INPUT_KEINE;
}


// =============================================================================
// Sensoren verarbeiten (fail-secure)
// =============================================================================

AlarmInput sensoren_verarbeite() {
  bool tuerOffen1 = hal_istTuerOffen1();
  bool tuerOffen2 = hal_istTuerOffen2();
  
  // Ein einzelner offener oder unterbrochener Reed-Kreis reicht aus. Die alte
  // AND-Verknuepfung konnte einen realen Einbruch bei nur einer Tuer oder
  // einem durchtrennten Sensorkabel unterdruecken.
  if (sensorAlarmErforderlich(fsm_istScharf(), fsm_istAusgeloest(),
                             tuerOffen1, tuerOffen2)) {
    return INPUT_SENSOR_OFFEN;
  }
  
  return INPUT_KEINE;
}


// =============================================================================
// Serielle Kommandos verarbeiten
// =============================================================================

AlarmInput kommando_verarbeite() {
  KommKommando cmd = hal_comm_leseKommando();
  
  switch (cmd) {
    case CMD_HEARTBEAT_ACK:
      watchdog_bestaetigt();
      return INPUT_KEINE;
      
    case CMD_ERZWINGE_ALARM:
      return INPUT_ERZWINGE_ALARM;
      
    case CMD_ALARM_STOP:
      return INPUT_ALARM_STOP;
      
    case CMD_SCHARF:
      return INPUT_CMD_SCHARF;
      
    case CMD_UNSCHARF:
      return INPUT_CMD_UNSCHARF;
      
    case CMD_STATUS:
      hal_comm_sendeStatus(
        fsm_istScharf(),
        fsm_istAusgeloest(),
        hal_istTuerOffen1(),
        hal_istTuerOffen2(),
        verbindungAktiv
      );
      return INPUT_KEINE;
      
    case CMD_REBOOT:
      hal_reboot();  // Kehrt nicht zurueck
      return INPUT_KEINE;
      
    default:
      return INPUT_KEINE;
  }
}


// =============================================================================
// LEDs aktualisieren
// =============================================================================

void leds_aktualisiere() {
  bool scharf = fsm_istScharf();
  
  // Alarm-LED zeigt Scharfstatus
  hal_setAlarmLED(scharf);
  
  // Sensor-LEDs zeigen Tuerstatus (nur wenn scharf)
  if (scharf) {
    hal_setSensor1LED(!hal_istTuerOffen1());  // LED an = Tuer zu
    hal_setSensor2LED(!hal_istTuerOffen2());
  } else {
    hal_setSensor1LED(false);
    hal_setSensor2LED(false);
  }
}


// =============================================================================
// Setup
// =============================================================================

void setup() {
  // Hardware initialisieren
  hal_io_init();
  hal_rfid_init();
  hal_comm_init(BAUDRATE);
  
  // Logik initialisieren
  fsm_init();
  
  // Timing starten
  uint32_t jetzt = hal_zeitMs();
  letzterHeartbeat = jetzt;
  letzteAntwort = jetzt;
  letzteRfidLesung = jetzt - RFID_SPERRZEIT_MS;  // Sofort lesbereit
  
  // Bereit
  wdt_enable(WDTO_8S);    
  hal_comm_sendeEvent(EVT_SYSTEM_BEREIT);
}


// =============================================================================
// Loop
// =============================================================================

void loop() {
  // Eingaben sammeln (Prioritaet: Kommando > RFID > Sensor)
  AlarmInput eingabe = kommando_verarbeite();
  
  if (eingabe == INPUT_KEINE) {
    eingabe = rfid_verarbeite();
  }
  
  if (eingabe == INPUT_KEINE) {
    eingabe = sensoren_verarbeite();
  }
  
  // Zustandsautomat
  fsm_verarbeite(eingabe);
  
  // Watchdog
  watchdog_sende();
  watchdog_pruefe();
  
  // Ausgaben
  leds_aktualisiere();

  //Watchdog füttern
  wdt_reset(); 
}
