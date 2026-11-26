#pragma once
#include <Arduino.h>
#include <Wire.h>
#include <SPI.h>
#include "iot_pins.h"
#include "serial_logger.h"

inline void iotStartSerial(const char* activityName) {
  Serial.begin(115200);
  delay(600);
  const bool logReady = SerialLog.begin();
  SerialLog.println();
  SerialLog.println("========================================");
  SerialLog.print("IoT@UCI Activity: ");
  SerialLog.println(activityName);
  SerialLog.println("========================================");
  if (logReady) {
    SerialLog.print("Serial output log: ");
    SerialLog.println(SerialLog.path());
  }
}

inline void iotInitI2C() {
  Wire.begin(IoTPins::I2C_SDA, IoTPins::I2C_SCL);
}

inline void iotInitSPI() {
  SPI.begin(IoTPins::SPI_SCK, IoTPins::SPI_MISO, IoTPins::SPI_MOSI);
}

inline void iotPrintDeviceId() {
  uint64_t chipid = ESP.getEfuseMac();
  SerialLog.print("ESP32 chip id: 0x");
  SerialLog.println((uint32_t)(chipid & 0xFFFFFFFF), HEX);
}

inline bool iotEvery(uint32_t intervalMs, uint32_t& lastMs) {
  const uint32_t now = millis();
  if (now - lastMs >= intervalMs) {
    lastMs = now;
    return true;
  }
  return false;
}

inline void iotI2CScan() {
  SerialLog.println("Scanning I2C bus...");
  int count = 0;
  for (uint8_t addr = 1; addr < 127; addr++) {
    Wire.beginTransmission(addr);
    if (Wire.endTransmission() == 0) {
      SerialLog.print("Found I2C device at 0x");
      if (addr < 16) SerialLog.print("0");
      SerialLog.println(addr, HEX);
      count++;
    }
  }
  SerialLog.print("I2C devices found: ");
  SerialLog.println(count);
}
