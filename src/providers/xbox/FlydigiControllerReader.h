#pragma once

#include "src/core/BatteryDevice.h"

#include <vector>

// 读取 Flydigi XInput 兼容手柄的型号、连接方式和电量档位。
std::vector<BatteryDevice> readFlydigiControllerDevices();
