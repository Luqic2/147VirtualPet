#pragma once
#include <Arduino.h>
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include "iot_pins.h"
#include "serial_logger.h"

constexpr int IOT_SCREEN_WIDTH = 128;
constexpr int IOT_SCREEN_HEIGHT = 64;

inline Adafruit_SSD1306& iotDisplay() {
//   static Adafruit_SSD1306 display(
//       IOT_SCREEN_WIDTH,
//       IOT_SCREEN_HEIGHT,
//       &SPI,
//       IoTPins::OLED_DC,
//       IoTPins::OLED_RST,
//       IoTPins::OLED_CS);
//   return display;
  static Adafruit_SSD1306 display(
      IOT_SCREEN_WIDTH, IOT_SCREEN_HEIGHT, &SPI,
      IoTPins::OLED_DC, -1, IoTPins::OLED_CS);
  return display;
}

inline Adafruit_SSD1306& iotDisplay2() {
  static Adafruit_SSD1306 display(
      IOT_SCREEN_WIDTH,
      IOT_SCREEN_HEIGHT,
      &SPI,
      IoTPins::OLED_DC,
      -1,
      IoTPins::OLED_2_CS);
  return display;
}

inline bool iotInitDisplay() {
pinMode(IoTPins::OLED_RST, OUTPUT);
digitalWrite(IoTPins::OLED_RST, LOW);
delay(10);
digitalWrite(IoTPins::OLED_RST, HIGH);
delay(10);
  pinMode(IoTPins::OLED_CS, OUTPUT);
  pinMode(IoTPins::OLED_2_CS, OUTPUT);

  digitalWrite(IoTPins::OLED_CS, HIGH);
  digitalWrite(IoTPins::OLED_2_CS, HIGH);

  SPI.begin(
      IoTPins::SPI_SCK,
      IoTPins::SPI_MISO,
      IoTPins::SPI_MOSI
  );

  bool firstStarted = iotDisplay().begin(SSD1306_SWITCHCAPVCC);
  bool secondStarted = iotDisplay2().begin(SSD1306_SWITCHCAPVCC);

  SerialLog.printf("First OLED: %s\n", firstStarted ? "OK" : "FAILED");
  SerialLog.printf("Second OLED: %s\n", secondStarted ? "OK" : "FAILED");

  if (firstStarted) {
    iotDisplay().clearDisplay();
    iotDisplay().setTextSize(2);
    iotDisplay().setTextColor(SSD1306_WHITE);
    iotDisplay().setCursor(0, 0);
    iotDisplay().println("IoT Kit");
    iotDisplay().display();
  }

  if (secondStarted) {
    iotDisplay2().clearDisplay();
    iotDisplay2().setTextSize(2);
    iotDisplay2().setTextColor(SSD1306_WHITE);
    iotDisplay2().setCursor(0, 0);
    iotDisplay2().println("Petometer");
    iotDisplay2().display();
  }

  return firstStarted && secondStarted;
}
// inline bool iotInitDisplay() {
//   const bool firstStarted = iotDisplay().begin(SSD1306_SWITCHCAPVCC);
//   if (!firstStarted) {
//     Serial.println("OLED init failed. Check SPI wiring and pin map.");
//   }

//   const bool secondStarted = iotDisplay2().begin(SSD1306_SWITCHCAPVCC);
//   if (!secondStarted) {
//     Serial.println("Second OLED init failed. Check its CS wiring and pin map.");
//   }

//   if (firstStarted) {
//     auto& display = iotDisplay();
//     display.clearDisplay();
//     display.setTextSize(1);
//     display.setTextColor(SSD1306_WHITE);
//     display.setCursor(0, 0);
//     display.println("IoT Kit");
//     display.display();
//   }
//   if (secondStarted) {
//     auto& display = iotDisplay2();
//     display.clearDisplay();
//     display.setTextSize(1);
//     display.setTextColor(SSD1306_WHITE);
//     display.setCursor(0, 0);
//     display.println("Petometer");
//     display.display();
//   }
//   return firstStarted && secondStarted;
// }

inline void iotDisplayLines(const String& line1,
                              const String& line2 = "",
                              const String& line3 = "",
                              const String& line4 = "") {
  auto& display = iotDisplay();
  display.clearDisplay();
  display.setCursor(0, 0);
  display.println(line1);
  if (line2.length()) display.println(line2);
  if (line3.length()) display.println(line3);
  if (line4.length()) display.println(line4);
  display.display();
}
