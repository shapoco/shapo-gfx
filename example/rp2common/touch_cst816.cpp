#include "touch_cst816.hpp"

#include "hardware/gpio.h"
#include "pico/stdlib.h"

namespace rp2common {

namespace {

// Registers
constexpr uint8_t REG_FINGER_NUM = 0x02;  // then XH, XL, YH, YL
constexpr uint8_t REG_CHIP_ID = 0xA7;
constexpr uint8_t REG_DIS_AUTO_SLEEP = 0xFE;

constexpr uint32_t TIMEOUT_US = 2000;

}  // namespace

bool Cst816Touch::readRegs(uint8_t reg, uint8_t *data, int n) {
  if (i2c_write_timeout_us(cfg_.i2c, cfg_.addr, &reg, 1, true, TIMEOUT_US) !=
      1) {
    return false;
  }
  return i2c_read_timeout_us(cfg_.i2c, cfg_.addr, data, (size_t)n, false,
                             TIMEOUT_US) == n;
}

bool Cst816Touch::writeReg(uint8_t reg, uint8_t value) {
  const uint8_t buf[2] = {reg, value};
  return i2c_write_timeout_us(cfg_.i2c, cfg_.addr, buf, 2, false,
                              TIMEOUT_US) == 2;
}

bool Cst816Touch::init(const TouchConfig &cfg) {
  cfg_ = cfg;
  i2c_init(cfg.i2c, cfg.baud);
  gpio_set_function(cfg.sdaPin, GPIO_FUNC_I2C);
  gpio_set_function(cfg.sclPin, GPIO_FUNC_I2C);
  gpio_pull_up(cfg.sdaPin);
  gpio_pull_up(cfg.sclPin);

  ok_ = readRegs(REG_CHIP_ID, &chipId_, 1);
  // Stay awake: asleep it stops answering until touched
  if (ok_) ok_ = writeReg(REG_DIS_AUTO_SLEEP, 0x01);
  return ok_;
}

bool Cst816Touch::read(int &x, int &y) {
  if (!ok_) return false;
  uint8_t d[5];
  if (!readRegs(REG_FINGER_NUM, d, 5)) return false;
  if (d[0] == 0 || d[0] > 2) return false;
  x = ((d[1] & 0x0F) << 8) | d[2];
  y = ((d[3] & 0x0F) << 8) | d[4];
  return true;
}

}  // namespace rp2common
