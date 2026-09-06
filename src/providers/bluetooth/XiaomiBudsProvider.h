#pragma once

#include "src/core/IBatteryProvider.h"
#include "src/providers/bluetooth/SharedBleWatcher.h"

#include <winrt/base.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <map>
#include <mutex>

//
//   1) 厂商数据帧（AD type 0xFF，CompanyId=0x038F Xiaomi）：
//      校验 len>=0x18 && p[0]==len-2 && p[1]==0x01；
//      p[2]=majorId，minorId=(p[3]>>1)&0x07，p[5]/p[6]/p[7]=左/右/盒电量。
//      另在 p[11..16] / p[18..23] 携带两个 48bit 设备地址（经典蓝牙 / BLE
//      MAC，字节顺序有位置重排），可用于和系统配对列表匹配。
//
//   2) 服务数据帧（AD type 0x16，UUID=0xFD2D MAF，同账号设备）：
//      校验 len>=0x0F && p[0]==0x01；p[1]=majorId，p[2..3]=LE16 minorId，
//      p[12]/p[13]/p[14]=右/左/盒电量（注意左右顺序与厂商数据帧相反）。
//
//   两类帧的电量字节编码一致：0xFF=未知；低 7 位=百分比；bit7=充电中。
//
// 共享 watcher 订阅在 readDevices() 首次调用时注册并保持；readDevices()
// 返回“最近 N 秒内收到过广播”的设备快照。收到有意义的新广播时会
// 通知 BatteryManager 定向刷新，无需等待下一个轮询周期。
class XiaomiBudsProvider : public IBatteryProvider
{
public:
    XiaomiBudsProvider();
    ~XiaomiBudsProvider() override;

    std::wstring displayName() const override;
    std::vector<BatteryDevice> readDevices() override;
    void setChangeCallback(ChangeCallback callback) override;

private:
    // 单台小米耳机的最新解析结果（缓存，按 BLE 广播地址聚合）。
    struct AdvDevice
    {
        int leftPercent = -1;
        int rightPercent = -1;
        int casePercent = -1;
        bool charging = false;
        bool leftCharging = false;
        bool rightCharging = false;
        bool caseCharging = false;
        // 小米型号查表键。V1=厂商数据帧，V2=FD2D 服务数据帧。
        uint8_t protocolVersion = 0;
        uint8_t majorId = 0;
        uint16_t minorId = 0;
        // 厂商数据帧携带的两个 48bit 地址（原始 6 字节，顺序按帧内重排还原）。
        // 用于与 Windows 配对设备匹配；为 0 表示帧里没带。
        uint8_t embeddedAddrA[6] = {};
        uint8_t embeddedAddrB[6] = {};
        bool hasEmbeddedAddr = false;
        short rssi = 0;
        std::chrono::steady_clock::time_point lastSeen; // 最近一次收到广播的时间
    };

    // 注册共享 watcher 订阅（幂等）。
    void ensureSubscribed();

    // 广播接收回调（在 WinRT 线程池线程触发，经 SharedBleWatcher 分发）。
    void onAdvertisementReceived(const SharedBleWatcher::Args &args);

    // 更新单台设备的最新广播；首次出现、电量变化或静默后重新出现时
    // 立即请求 Manager 刷新，连续重复广播不会造成刷新风暴。
    void updateDevice(uint64_t address, AdvDevice device);

    // 蓝牙地址 -> 解析结果。由 watcher 回调线程写、readDevices() 读，需加锁。
    std::map<uint64_t, AdvDevice> m_devices;
    std::mutex m_mutex;
    ChangeCallback m_changeCallback;

    // 广播 RSSI 阈值（dBm）。readDevices() 每轮从 AppSettings 刷新，
    // watcher 回调线程高频读取（不能每个广播都查 QSettings，故缓存为原子量）。
    std::atomic<int> m_rssiThreshold{-70};

    // SharedBleWatcher 订阅 id；0 表示未订阅。
    std::uint64_t m_subscriptionId = 0;
};
