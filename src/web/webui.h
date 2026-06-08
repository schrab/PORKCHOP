#pragma once

#include <Arduino.h>

namespace WebUI {
    void start();
    void stop();
    void update();
    bool isActive();
}
