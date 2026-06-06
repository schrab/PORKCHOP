#pragma once
#include <cstdint>

// IMU accelerometer data
struct hal_imu_accel_t {
    float x;
    float y;
    float z;
};

// Initialize IMU (accelerometer) - returns true if hardware found
bool hal_imu_init();

// Read accelerometer data - returns true on success
// ax, ay, az are output pointers for G-force values
bool hal_imu_getAccel(float* ax, float* ay, float* az);

// Check if IMU is available
bool hal_imu_isAvailable();
