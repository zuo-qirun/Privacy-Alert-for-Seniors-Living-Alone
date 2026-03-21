#pragma once

#include <Arduino.h>

namespace ConfigPins {

constexpr uint8_t kPirBedroom = 34;
constexpr uint8_t kPirToilet = 35;
constexpr uint8_t kDoorToilet = 32;
constexpr uint8_t kBedPressure = 33;
constexpr uint8_t kBuzzer = 27;

constexpr bool kBedPressureUsePullup = false;

}  // namespace ConfigPins
