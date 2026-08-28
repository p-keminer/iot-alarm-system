#include <cassert>
#include <cstdint>

#include "../../firmware/esp8266/receiver/replay_schutz.h"

int main() {
  unsigned long basis = 0;
  uint32_t bitmap = 0;

  assert(ReplaySchutz::pruefeStrikt(100, basis, bitmap) == ReplaySchutz::NEU);
  assert(basis == 100 && bitmap == 1UL);

  // Ein verspaetetes, zuvor ungesehenes ALARM_ON mit Sequenz 99 darf den mit
  // Sequenz 100 gesetzten Zustand nicht mehr umkehren.
  assert(ReplaySchutz::pruefeStrikt(99, basis, bitmap) == ReplaySchutz::ZU_ALT);
  assert(basis == 100 && bitmap == 1UL);

  // Exakt dasselbe Paket bleibt fuer einen verlorenen ACK erneut bestaetigbar.
  assert(ReplaySchutz::pruefeStrikt(100, basis, bitmap) == ReplaySchutz::DUPLIKAT);
  assert(ReplaySchutz::pruefeStrikt(101, basis, bitmap) == ReplaySchutz::NEU);
  assert(basis == 101 && bitmap == 1UL);
  return 0;
}
