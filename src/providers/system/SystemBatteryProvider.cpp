#include "SystemBatteryProvider.h"
#include "util/Logger.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include <windows.h>
#include <winioctl.h> // CTL_CODE / FILE_DEVICE_BATTERY（WIN32_LEAN_AND_MEAN 下需显式引入）

#include <setupapi.h>
#pragma comment(lib, "Setupapi.lib")

#include <winrt/Windows.Devices.Power.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.System.Power.h>
#include <winrt/base.h>

using namespace winrt::Windows::Devices::Power;
using BatteryStatus = winrt::Windows::System::Power::BatteryStatus;

namespace
{
// —— Win32 电池接口常量（摘录自 Windows SDK shared\poclass.h）——
//
// poclass.h 位于 SDK 的 shared 包含目录，部分构建环境不把它加入 INCLUDE，
// 因此按公开文档值内联定义；这些是电池类驱动自 Windows 2000 起稳定的
// 公开 ABI（与 WMI root\wmi:BatteryStatus 同源），不会变动。

// 电池设备接口类 GUID = GUID_DEVICE_BATTERY {72631E54-78A4-11D0-BCF7-00AA00B7B32A}。
constexpr GUID kBatteryDeviceGuid = {
    0x72631e54, 0x78a4, 0x11d0,
    { 0xbc, 0xf7, 0x00, 0xaa, 0x00, 0xb7, 0xb3, 0x2a }
};

constexpr ULONG kIoctlBatteryQueryTag =
    CTL_CODE(FILE_DEVICE_BATTERY, 0x10, METHOD_BUFFERED, FILE_READ_ACCESS);
constexpr ULONG kIoctlBatteryQueryStatus =
    CTL_CODE(FILE_DEVICE_BATTERY, 0x13, METHOD_BUFFERED, FILE_READ_ACCESS);

constexpr ULONG kBatteryUnknownRate = 0x80000000;
constexpr ULONG kBatteryCharging = 0x00000004;

// IOCTL_BATTERY_QUERY_STATUS 输入（BatteryTag 之外清零 = 立即返回，不等待）。
struct NativeBatteryWaitStatus
{
    ULONG BatteryTag;
    ULONG Timeout;
    ULONG PowerState;
    ULONG LowCapacity;
    ULONG HighCapacity;
};

// IOCTL_BATTERY_QUERY_STATUS 输出。Rate 单位毫瓦：
// 正 = 放电，负 = 充电，kBatteryUnknownRate = 驱动未上报。
struct NativeBatteryStatusData
{
    ULONG PowerState;
    ULONG Capacity;
    ULONG Voltage;
    LONG Rate;
};

// 精确百分比 -> 离散档位（与其它 provider 一致）。
BatteryLevel levelFromPercentage(int percentage)
{
    if (percentage < 0) {
        return BatteryLevel::Unknown;
    }
    if (percentage >= 80) {
        return BatteryLevel::Full;
    }
    if (percentage >= 50) {
        return BatteryLevel::Medium;
    }
    if (percentage >= 20) {
        return BatteryLevel::Low;
    }
    return BatteryLevel::Empty;
}

// Rate 四舍五入到 100 mW：读数逐轮轻微抖动，若原样上报会让界面每轮重建。
// C++ 整数除法向零截断，负值按符号分离后再取整。
int roundRateTo100Mw(int rateMw)
{
    if (rateMw == 0) {
        return 0;
    }
    const int sign = rateMw < 0 ? -1 : 1;
    const int magnitude = rateMw * sign;
    return sign * ((magnitude + 50) / 100 * 100);
}

// Win32 电池 IOCTL 查询结果。
struct NativeBatteryStatus
{
    bool present = false;
    // 多电池取带符号总和（正 = 放电，负 = 充电）。
    int rateMilliwatts = 0;
    bool charging = false;
};

// 通过 SetupAPI 枚举电池设备接口，逐个 CreateFile + IOCTL_BATTERY_QUERY_STATUS。
// 这是驱动层最直接的速率来源：部分机器的 WinRT AggregateBattery 不回填速率，
// 此路径仍能拿到 BATTERY_STATUS.Rate（与 WMI root\wmi:BatteryStatus 同源）。
NativeBatteryStatus queryNativeBatteryStatus()
{
    NativeBatteryStatus result;

    HDEVINFO devInfo = SetupDiGetClassDevsW(
        &kBatteryDeviceGuid, nullptr, nullptr,
        DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (devInfo == INVALID_HANDLE_VALUE) {
        return result;
    }

    SP_DEVICE_INTERFACE_DATA interfaceData{};
    interfaceData.cbSize = sizeof(interfaceData);
    for (DWORD index = 0;
         SetupDiEnumDeviceInterfaces(devInfo, nullptr, &kBatteryDeviceGuid,
                                     index, &interfaceData);
         ++index) {
        DWORD needed = 0;
        SetupDiGetDeviceInterfaceDetailW(
            devInfo, &interfaceData, nullptr, 0, &needed, nullptr);
        if (needed == 0) {
            continue;
        }
        std::vector<BYTE> detailBuffer(needed, 0);
        auto *detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W *>(
            detailBuffer.data());
        detail->cbSize = sizeof(*detail);
        if (!SetupDiGetDeviceInterfaceDetailW(
                devInfo, &interfaceData, detail, needed, &needed, nullptr)) {
            continue;
        }

        HANDLE batteryHandle = CreateFileW(
            detail->DevicePath, GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
            0, nullptr);
        if (batteryHandle == INVALID_HANDLE_VALUE) {
            // 个别环境下读写权限被拒；查询类 IOCTL 用 0 权限句柄通常仍可用。
            batteryHandle = CreateFileW(
                detail->DevicePath, 0, FILE_SHARE_READ | FILE_SHARE_WRITE,
                nullptr, OPEN_EXISTING, 0, nullptr);
            if (batteryHandle == INVALID_HANDLE_VALUE) {
                continue;
            }
        }

        // 先取 BatteryTag（电池在场标志），再用它查询瞬时状态。
        ULONG batteryTag = 0;
        DWORD returned = 0;
        if (DeviceIoControl(batteryHandle, kIoctlBatteryQueryTag, nullptr, 0,
                            &batteryTag, sizeof(batteryTag), &returned,
                            nullptr) &&
            batteryTag != 0) {
            NativeBatteryWaitStatus waitStatus{};
            waitStatus.BatteryTag = batteryTag;
            NativeBatteryStatusData status{};
            if (DeviceIoControl(batteryHandle, kIoctlBatteryQueryStatus,
                                &waitStatus, sizeof(waitStatus), &status,
                                sizeof(status), &returned, nullptr)) {
                result.present = true;
                if (status.Rate != static_cast<LONG>(kBatteryUnknownRate)) {
                    result.rateMilliwatts += status.Rate;
                }
                result.charging =
                    result.charging || (status.PowerState & kBatteryCharging) != 0;
            }
        }
        CloseHandle(batteryHandle);
    }
    SetupDiDestroyDeviceInfoList(devInfo);
    return result;
}
} // namespace

std::wstring SystemBatteryProvider::displayName() const
{
    return L"System";
}

std::vector<BatteryDevice> SystemBatteryProvider::readDevices()
{
    int percentage = -1;
    bool charging = false;
    bool batteryPresent = false;
    int rateMilliwatts = 0;
    bool externalPower = false;

    // —— 主路径：WinRT 聚合电池（惰性初始化 apartment，在 worker 线程）——
    try {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        const BatteryReport report = Battery::AggregateBattery().GetReport();

        const BatteryStatus status = report.Status();
        if (status != BatteryStatus::NotPresent) {
            batteryPresent = true;
            charging = (status == BatteryStatus::Charging);
        }

        const auto remaining = report.RemainingCapacityInMilliwattHours();
        const auto full = report.FullChargeCapacityInMilliwattHours();
        if (remaining && full && full.Value() > 0) {
            const int pct = static_cast<int>(std::lround(
                remaining.Value() * 100.0 / full.Value()));
            percentage = std::clamp(pct, 0, 100);
            batteryPresent = true;
        }

        // ChargeRateInMilliwatts 是 BatteryReport 唯一的带符号速率属性
        // （IReference）：负 = 充电，正 = 放电，与 Win32 BATTERY_STATUS.Rate
        // 约定一致。个别驱动符号不规范时用 Status 枚举兜底修正方向。
        const auto chargeRate = report.ChargeRateInMilliwatts();
        if (chargeRate && chargeRate.Value() != 0) {
            int rateMw = chargeRate.Value();
            if (status == BatteryStatus::Charging && rateMw > 0) {
                rateMw = -rateMw;
            } else if (status == BatteryStatus::Discharging && rateMw < 0) {
                rateMw = -rateMw;
            }
            rateMilliwatts = roundRateTo100Mw(rateMw);
        }
    } catch (const winrt::hresult_error &e) {
        LOG_WARN_W(L"[System] WinRT battery report failed: " +
                   std::wstring(e.message()));
    } catch (...) {
        LOG_WARN("[System] WinRT battery report failed (unknown)");
    }

    // —— 补充路径：Win32 电池 IOCTL 取速率 / 校正充电状态 ——
    // 部分机器 AggregateBattery 不回填速率，驱动层 IOCTL 仍能给出真实值。
    // WinRT 已拿到速率时跳过，省去每轮 SetupAPI 枚举开销。
    if (rateMilliwatts == 0) {
        const NativeBatteryStatus native = queryNativeBatteryStatus();
        if (native.present) {
            batteryPresent = true;
            charging = charging || native.charging;
            if (native.rateMilliwatts != 0) {
                rateMilliwatts = roundRateTo100Mw(native.rateMilliwatts);
            }
        }
    }

    // —— 回退路径：GetSystemPowerStatus 补百分比 / 充电状态 / 交流在线 ——
    // 该 API 不提供功率，但 ACLineStatus 是「是否插电」的权威来源。
    {
        SYSTEM_POWER_STATUS powerStatus{};
        if (GetSystemPowerStatus(&powerStatus)) {
            // BatteryFlag 位 128 = 无电池，255 = 状态未知；
            // BatteryLifePercent 255 = 未知。
            const BYTE flag = powerStatus.BatteryFlag;
            const bool noBattery =
                (flag & 128) != 0 || powerStatus.BatteryLifePercent == 255;
            if (!noBattery) {
                batteryPresent = true;
                if (percentage < 0 && powerStatus.BatteryLifePercent <= 100) {
                    percentage = powerStatus.BatteryLifePercent;
                }
                // BatteryFlag 位 8 = 正在充电（状态未知时不可信，跳过）。
                charging = charging || (flag != 255 && (flag & 8) != 0);
            }
            // ACLineStatus：1 = 交流在线，0 = 使用电池，255 = 未知。
            externalPower = powerStatus.ACLineStatus == 1;
        }
    }

    if (!batteryPresent || percentage < 0) {
        // 台式机等无电池场景：静默返回空，不产生设备行。
        LOG_VERBOSE("[System] no system battery detected");
        return {};
    }

    BatteryDevice device;
    device.id = L"system:pc-battery";
    device.name = L"PC Battery";
    device.type = BatteryDevice::Type::System;
    device.percentage = percentage;
    device.level = levelFromPercentage(percentage);
    device.charging = charging;
    device.rateMilliwatts = rateMilliwatts;
    device.externalPower = externalPower;
    // wired 必须保持 false：UI 会把 wired 且非 Xbox 的设备当作
    // 「有线供电不显示电量」处理，接入交流电源不是这个语义。
    device.wired = false;
    device.connected = true;

    LOG_VERBOSE_W(L"[System] pc battery = " + std::to_wstring(percentage) +
                  L"% rate=" + std::to_wstring(rateMilliwatts) +
                  L"mW charging=" + (charging ? L"1" : L"0") +
                  L" ac=" + (externalPower ? L"1" : L"0"));
    return { std::move(device) };
}
