#ifndef SENSOR_LOGIC_H
#define SENSOR_LOGIC_H

// Fail-secure: Bei scharfem System reicht ein einzelner offener oder
// unterbrochener Reed-Kreis aus, um Alarm auszuloesen.
inline bool sensorAlarmErforderlich(bool scharf, bool bereitsAusgeloest,
                                    bool tuerOffen1, bool tuerOffen2) {
  return scharf && !bereitsAusgeloest && (tuerOffen1 || tuerOffen2);
}

#endif
