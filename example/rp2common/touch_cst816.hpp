#ifndef RP2COMMON_TOUCH_CST816_HPP
#define RP2COMMON_TOUCH_CST816_HPP

// CST816 (CST816D on the RP2350-Touch-LCD-2) capacitive touch controller
// over I2C, polled: one touch point in the panel's native (portrait)
// coordinates. The controller is reset with the panel (shared pin), so
// init() does not reset it.

#include <cstdint>

#include "hardware/i2c.h"

namespace rp2common {

struct TouchConfig {
  i2c_inst_t *i2c;
  uint8_t sdaPin, sclPin;
  uint8_t addr;
  uint32_t baud;
};

class Cst816Touch {
 public:
  // Set up the bus and keep the controller from sleeping. False if it does
  // not answer.
  bool init(const TouchConfig &cfg);

  // The chip ID (0xB6 for the CST816D) read by init()
  uint8_t chipId() const { return chipId_; }

  // Read the current touch: true and the point if a finger is down. False
  // when none is (or the read failed).
  bool read(int &x, int &y);

 private:
  TouchConfig cfg_ = {};
  uint8_t chipId_ = 0;
  bool ok_ = false;

  bool readRegs(uint8_t reg, uint8_t *data, int n);
  bool writeReg(uint8_t reg, uint8_t value);
};

}  // namespace rp2common

#endif
