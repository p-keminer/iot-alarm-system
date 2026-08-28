#ifndef REPLAY_SCHUTZ_H
#define REPLAY_SCHUTZ_H

#include <stdint.h>

// ALARM_ON/OFF sind zustandssetzende Last-Writer-Wins-Befehle. Deshalb darf
// nur eine strikt hoehere Sequenz wirken. Ein exakt gleiches Paket wird fuer
// verlorene ACKs erkannt; jede niedrigere Sequenz ist veraltet, auch wenn sie
// innerhalb eines frueheren Sliding-Windows noch ungesehen war.
namespace ReplaySchutz {
enum Ergebnis : uint8_t {
  NEU,
  DUPLIKAT,
  ZU_ALT
};

inline Ergebnis pruefeStrikt(unsigned long sequenz,
                             unsigned long& hoechsteSequenz,
                             uint32_t& bitmap) {
  if (sequenz > hoechsteSequenz) {
    hoechsteSequenz = sequenz;
    bitmap = 1UL;
    return NEU;
  }
  if (sequenz == hoechsteSequenz && (bitmap & 1UL) != 0) return DUPLIKAT;
  return ZU_ALT;
}
}  // namespace ReplaySchutz

#endif
