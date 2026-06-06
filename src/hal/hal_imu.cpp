#include "hal_imu.h"

// ESP32-S3 Mini doesn't have a built-in IMU
// This is a stub implementation

bool hal_imu_init() {
    return false;
}

bool hal_imu_getAccel(float* ax, float* ay, float* az) {
    if (ax) *ax = 0.0f;
    if (ay) *ay = 0.0f;
    if (az) *az = 1.0f;  // Default to 1G on Z (resting)
    return false;
}

bool hal_imu_isAvailable() {
    return false;
}
