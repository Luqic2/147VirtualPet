#pragma once

#include <Arduino.h>

void petBegin();
void petUpdate(float temperatureF, bool temperatureAvailable, int potPercent);
