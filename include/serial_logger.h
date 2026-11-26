#pragma once

#include <Arduino.h>
#include <LittleFS.h>
#include <string.h>

class SerialLogger : public Print {
 public:
  bool begin() {
    if (!LittleFS.begin(false)) {
      Serial.println("ERROR: LittleFS mount failed; serial output will not be saved.");
      return false;
    }

    if (!LittleFS.exists("/logs") && !LittleFS.mkdir("/logs")) {
      Serial.println("ERROR: Could not create the logs directory.");
      return false;
    }

    uint32_t runStart = millis();
    do {
      snprintf(path_, sizeof path_, "/logs/%lu.txt", (unsigned long)runStart++);
    } while (LittleFS.exists(path_));
    file_ = LittleFS.open(path_, FILE_WRITE);
    if (!file_) {
      Serial.println("ERROR: Could not open the serial log file.");
      return false;
    }
    return true;
  }

  const char* path() const { return path_; }

  size_t write(uint8_t value) override {
    const size_t serialWritten = Serial.write(value);
    if (file_ && file_.write(value) != 1) reportWriteFailure();
    if (file_ && value == '\n') file_.flush();
    return serialWritten;
  }

  size_t write(const uint8_t* buffer, size_t size) override {
    const size_t serialWritten = Serial.write(buffer, size);
    if (file_ && size > 0 && file_.write(buffer, size) != size) reportWriteFailure();
    if (file_ && size > 0 && memchr(buffer, '\n', size) != nullptr) file_.flush();
    return serialWritten;
  }

 private:
  void reportWriteFailure() {
    if (writeFailed_) return;
    writeFailed_ = true;
    Serial.println("ERROR: Writing to the serial log file failed.");
  }

  File file_;
  char path_[32] = {};
  bool writeFailed_ = false;
};

extern SerialLogger SerialLog;
