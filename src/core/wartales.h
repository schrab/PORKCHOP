#pragma once

#include <Arduino.h>

namespace Wartales {

void init();
void update();
void sessionStart();
void sessionEnd();
void logEvent(const char* event);
void logCapture(const char* type, const char* ssid);
void logDetection(const char* type, const char* detail);
void logModeChange(const char* from, const char* to);

} // namespace Wartales
