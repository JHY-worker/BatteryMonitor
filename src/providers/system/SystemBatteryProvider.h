#pragma once

#include "src/core/IBatteryProvider.h"

// 本机（笔记本电脑）电池提供者。
//
// 三条读取路径互补：
//   1. WinRT Battery::AggregateBattery()：百分比（剩余/满充容量）、充电状态
//      （Status 枚举）、速率（ChargeRateInMilliwatts，带符号：负 = 充电，
//      正 = 放电）；
//   2. Win32 电池 IOCTL（SetupAPI + IOCTL_BATTERY_QUERY_STATUS）：部分机器
//      WinRT 不回填速率，驱动层仍能拿到 BATTERY_STATUS.Rate；
//   3. GetSystemPowerStatus：兜底百分比 / 充电状态，并提供 ACLineStatus
//      （是否接入外部电源，驱动层不暴露该信息时的权威来源）。
// 台式机无电池时返回空列表。
class SystemBatteryProvider : public IBatteryProvider
{
public:
    std::wstring displayName() const override;
    std::vector<BatteryDevice> readDevices() override;
};
