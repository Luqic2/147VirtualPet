#include "interact.h"

#include <stdio.h>
#include <string.h>
#include "iot_display.h"

namespace {

constexpr uint32_t DISPLAY_REFRESH_MS = 5000;
constexpr uint32_t BUTTON_DEBOUNCE_MS = 35;
constexpr uint32_t PET_MOOD_MS = 3500;
constexpr uint32_t ACTION_MESSAGE_MS = 1200;
constexpr float COLD_TEMPERATURE_F = 64.4f;
constexpr float SAD_TEMPERATURE_F = 80.6f;
constexpr float HOT_TEMPERATURE_F = 86.0f;

struct Button {
  uint8_t pin;
  bool stableState;
  bool lastReading;
  uint32_t changedAt;
};

Button confirmButton{IoTPins::BUTTON_1, HIGH, HIGH, 0};
Button cancelButton{IoTPins::BUTTON_2, HIGH, HIGH, 0};
bool isSleeping = false;
bool hasBeenPetted = false;
uint32_t lastPetAt = 0;
uint32_t lastDisplayAt = 0;
bool displayStarted = false;
float currentTemperatureF = 0.0f;
bool currentTemperatureAvailable = false;
int currentPotPercent = 0;
bool displayDirty = true;
const char* actionMessage = nullptr;
uint32_t actionMessageUntil = 0;

enum class Mood {
  Idle,
  Happy,
  Sad,
  Exhausted,
  Cold,
  Sleeping
};

enum class PetAction {
  Pet,
  SleepWake
};

bool buttonPressed(Button& button, uint32_t now) {
  const bool reading = digitalRead(button.pin);
  if (reading != button.lastReading) {
    button.lastReading = reading;
    button.changedAt = now;
  }

  if (now - button.changedAt < BUTTON_DEBOUNCE_MS ||
      reading == button.stableState) {
    return false;
  }

  button.stableState = reading;
  return reading == LOW;
}

Mood currentMood(uint32_t now) {
  if (currentTemperatureAvailable) {
    if (currentTemperatureF >= HOT_TEMPERATURE_F) return Mood::Exhausted;
    if (currentTemperatureF < COLD_TEMPERATURE_F) return Mood::Cold;
  }
  if (isSleeping) return Mood::Sleeping;
  if (!currentTemperatureAvailable) return Mood::Idle;
  if (currentTemperatureF >= SAD_TEMPERATURE_F) return Mood::Sad;
  if (hasBeenPetted && now - lastPetAt < PET_MOOD_MS) return Mood::Happy;
  return Mood::Idle;
}

const char* moodLabel(Mood mood) {
  switch (mood) {
    case Mood::Happy: return "HAPPY";
    case Mood::Sad: return "SAD";
    case Mood::Exhausted: return "EXHAUSTED";
    case Mood::Cold: return "COLD";
    case Mood::Sleeping: return "SLEEPING";
    case Mood::Idle: return "CHILLING";
  }
  return "IDLE";
}

void drawFan(Adafruit_SSD1306& display, int16_t x, int16_t y) {
  display.drawCircle(x, y, 2, SSD1306_WHITE);
  display.drawLine(x, y - 3, x + 3, y - 8, SSD1306_WHITE);
  display.drawLine(x + 3, y - 8, x + 5, y - 3, SSD1306_WHITE);
  display.drawLine(x + 3, y + 2, x + 8, y + 5, SSD1306_WHITE);
  display.drawLine(x + 8, y + 5, x + 3, y + 7, SSD1306_WHITE);
  display.drawLine(x - 2, y + 1, x - 5, y + 7, SSD1306_WHITE);
  display.drawLine(x - 5, y + 7, x - 7, y + 2, SSD1306_WHITE);
}

void drawHeart(Adafruit_SSD1306& display, int16_t x, int16_t y) {
  display.fillCircle(x - 2, y, 2, SSD1306_WHITE);
  display.fillCircle(x + 2, y, 2, SSD1306_WHITE);
  display.fillTriangle(x - 4, y + 1, x + 4, y + 1, x, y + 6,
                       SSD1306_WHITE);
}

void drawFace(Adafruit_SSD1306& display, Mood mood) {
  constexpr int16_t centerX = 64;
  constexpr int16_t centerY = 31;

  if (mood == Mood::Sleeping) {
    display.drawRoundRect(43, 21, 42, 20, 9, SSD1306_WHITE);
    display.drawLine(52, 27, 59, 27, SSD1306_WHITE);
    display.drawLine(66, 27, 73, 27, SSD1306_WHITE);
    display.drawLine(59, 33, 66, 33, SSD1306_WHITE);
    display.setCursor(89, 20);
    display.print("z");
    display.setCursor(96, 14);
    display.print("z");
    display.setCursor(103, 8);
    display.print("z");
    return;
  }

  display.drawCircle(centerX, centerY, 15, SSD1306_WHITE);

  if (mood == Mood::Exhausted) {
    display.drawLine(50, 16, 48, 12, SSD1306_WHITE);
    display.drawLine(48, 12, 50, 8, SSD1306_WHITE);
    display.drawLine(64, 15, 62, 11, SSD1306_WHITE);
    display.drawLine(62, 11, 64, 7, SSD1306_WHITE);
    display.drawLine(78, 16, 76, 12, SSD1306_WHITE);
    display.drawLine(76, 12, 78, 8, SSD1306_WHITE);
  } else if (mood == Mood::Cold) {
    display.drawLine(44, 27, 40, 30, SSD1306_WHITE);
    display.drawLine(40, 30, 44, 33, SSD1306_WHITE);
    display.drawLine(84, 27, 88, 30, SSD1306_WHITE);
    display.drawLine(88, 30, 84, 33, SSD1306_WHITE);
  } else if (mood == Mood::Happy) {
    display.drawLine(51, 35, 44, 27, SSD1306_WHITE);
    display.drawLine(44, 27, 48, 21, SSD1306_WHITE);
    display.drawLine(77, 35, 84, 27, SSD1306_WHITE);
    display.drawLine(84, 27, 80, 21, SSD1306_WHITE);
  }

  if (mood == Mood::Idle) {
    display.drawLine(57, 28, 61, 28, SSD1306_WHITE);
    display.drawLine(67, 28, 71, 28, SSD1306_WHITE);
    display.drawPixel(60, 35, SSD1306_WHITE);
    display.drawLine(61, 36, 67, 36, SSD1306_WHITE);
    display.drawPixel(68, 35, SSD1306_WHITE);
    return;
  }

  display.fillCircle(59, 28, 1, SSD1306_WHITE);
  display.fillCircle(69, 28, 1, SSD1306_WHITE);
  if (mood == Mood::Sad || mood == Mood::Cold ||
      mood == Mood::Exhausted) {
    display.drawLine(59, 37, 62, 34, SSD1306_WHITE);
    display.drawLine(62, 34, 66, 34, SSD1306_WHITE);
    display.drawLine(66, 34, 69, 37, SSD1306_WHITE);
    return;
  }

  display.drawPixel(60, 34, SSD1306_WHITE);
  display.drawLine(61, 35, 67, 35, SSD1306_WHITE);
  display.drawPixel(68, 34, SSD1306_WHITE);
}

void drawPetScreen(uint32_t now) {
  auto& display = iotDisplay();
  const Mood mood = currentMood(now);

  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  display.print("ROOM TEMP");

  if (currentTemperatureAvailable) {
    char temperature[12];
    snprintf(temperature, sizeof temperature, "%.1f F", currentTemperatureF);
    display.setCursor(81, 0);
    display.print(temperature);
  } else {
    display.setCursor(89, 0);
    display.print("--.- F");
  }

  drawFace(display, mood);

  const char* label = moodLabel(mood);
  int16_t labelX = (IOT_SCREEN_WIDTH - (int16_t)strlen(label) * 6) / 2;
  display.setCursor(labelX, 49);
  display.print(label);

  if (hasBeenPetted && now - lastPetAt < PET_MOOD_MS &&
      mood != Mood::Exhausted) {
    drawHeart(display, 88, 23);
  }

  if (mood == Mood::Exhausted) {
    drawFan(display, 111, 34);
    display.setCursor(24, 57);
    display.print("Cool the room!");
  } else if (mood == Mood::Cold) {
    display.setCursor(28, 57);
    display.print("A little chilly");
  } else if (mood == Mood::Idle) {
    display.setCursor(13, 57);
    display.print("Check temperature");
  } else {
    display.setCursor(20, 57);
    display.print("Use petometer");
  }

  display.display();
}

PetAction selectedAction() {
  return currentPotPercent < 50 ? PetAction::Pet : PetAction::SleepWake;
}

const char* actionLabel(PetAction action) {
  if (action == PetAction::Pet) return "PET";
  return isSleeping ? "WAKE UP" : "SLEEP";
}

void drawPetometerScreen(uint32_t now) {
  auto& display = iotDisplay2();
  const PetAction action = selectedAction();

  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  display.print("PET-O-METER");
  display.setCursor(0, 18);
  display.print("Action: ");
  display.print(actionLabel(action));
  display.setCursor(0, 36);
  if (actionMessage != nullptr && now < actionMessageUntil) {
    display.print(actionMessage);
  } else {
    display.print("B1 confirm");
    display.setCursor(0, 50);
    display.print("B2 cancel");
  }
  display.display();
}

void applyAction(PetAction action, uint32_t now) {
  if (action == PetAction::Pet) {
    isSleeping = false;
    hasBeenPetted = true;
    lastPetAt = now;
    actionMessage = "Pet confirmed";
  } else {
    isSleeping = !isSleeping;
    actionMessage = isSleeping ? "Sleep confirmed" : "Wake confirmed";
  }
  actionMessageUntil = now + ACTION_MESSAGE_MS;
  displayDirty = true;
}

}  // namespace

void petBegin() {
  pinMode(confirmButton.pin, INPUT_PULLUP);
  pinMode(cancelButton.pin, INPUT_PULLUP);
  const uint32_t now = millis();
  confirmButton.changedAt = now;
  cancelButton.changedAt = now;
  lastDisplayAt = now - DISPLAY_REFRESH_MS;
  displayStarted = false;
}

void petUpdate(float temperatureF, bool temperatureAvailable, int potPercent) {
  const uint32_t now = millis();
  currentTemperatureF = temperatureF;
  currentTemperatureAvailable = temperatureAvailable;
  const PetAction previousAction = selectedAction();
  currentPotPercent = constrain(potPercent, 0, 100);
  if (selectedAction() != previousAction) displayDirty = true;

  if (buttonPressed(confirmButton, now)) {
    applyAction(selectedAction(), now);
  }
  if (buttonPressed(cancelButton, now)) {
    actionMessage = "Cancelled";
    actionMessageUntil = now + ACTION_MESSAGE_MS;
    displayDirty = true;
  }

  if (!displayStarted || displayDirty ||
      now - lastDisplayAt >= DISPLAY_REFRESH_MS) {
    drawPetScreen(now);
    drawPetometerScreen(now);
    lastDisplayAt = now;
    displayStarted = true;
    displayDirty = false;
  }
}