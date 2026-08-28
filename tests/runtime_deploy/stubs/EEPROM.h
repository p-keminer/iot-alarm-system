#ifndef TEST_EEPROM_H
#define TEST_EEPROM_H

#include <cstdint>
#include <cstring>

class EEPROMClass {
 public:
  EEPROMClass() { reset(); }

  uint8_t read(int address) const { return bytes[address]; }
  void update(int address, uint8_t value) { bytes[address] = value; }
  void reset() { std::memset(bytes, 0xFF, sizeof(bytes)); }

 private:
  uint8_t bytes[64];
};

extern EEPROMClass EEPROM;

#endif
