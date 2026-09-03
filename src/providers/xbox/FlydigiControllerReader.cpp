#include "FlydigiControllerReader.h"

#include "util/Logger.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <windows.h>

namespace
{
constexpr wchar_t kPipePath[] = L"\\\\.\\pipe\\fcs.sock";
constexpr char kProtocolHeader[] = "FDG_PROTOCOL\n";
constexpr uint64_t kGetConnectedDevices = 4097;
constexpr DWORD kReplyTimeoutMs = 1500;
constexpr uint32_t kMaxPayloadSize = 1024 * 1024;

struct HandleCloser
{
    void operator()(void *handle) const
    {
        if (handle && handle != INVALID_HANDLE_VALUE) {
            CloseHandle(static_cast<HANDLE>(handle));
        }
    }
};
using UniqueHandle = std::unique_ptr<void, HandleCloser>;

struct ProtobufField
{
    uint32_t number = 0;
    uint32_t wireType = 0;
    uint64_t integer = 0;
    std::vector<uint8_t> bytes;
};

struct ServiceDevice
{
    std::string name;
    std::string deviceCode;
    std::string mac;
    std::string firmware;
    int connectType = 0;
    int level = -1;
    uint16_t vid = 0;
    uint16_t pid = 0;
    bool connected = false;
};

std::wstring utf8ToWide(const std::string &text)
{
    if (text.empty()) {
        return {};
    }
    const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                           text.data(), static_cast<int>(text.size()),
                                           nullptr, 0);
    if (count <= 0) {
        return std::wstring(text.begin(), text.end());
    }
    std::wstring result(static_cast<size_t>(count), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                        static_cast<int>(text.size()), result.data(), count);
    return result;
}

bool readVarint(const std::vector<uint8_t> &data, size_t &position, uint64_t &value)
{
    value = 0;
    for (unsigned shift = 0; shift < 64 && position < data.size(); shift += 7) {
        const uint8_t byte = data[position++];
        value |= static_cast<uint64_t>(byte & 0x7f) << shift;
        if ((byte & 0x80) == 0) {
            return true;
        }
    }
    return false;
}

std::optional<std::vector<ProtobufField>> parseProtobuf(const std::vector<uint8_t> &data)
{
    std::vector<ProtobufField> fields;
    size_t position = 0;
    while (position < data.size()) {
        uint64_t tag = 0;
        if (!readVarint(data, position, tag) || tag == 0) {
            return std::nullopt;
        }
        ProtobufField field;
        field.number = static_cast<uint32_t>(tag >> 3);
        field.wireType = static_cast<uint32_t>(tag & 7);
        if (field.wireType == 0) {
            if (!readVarint(data, position, field.integer)) {
                return std::nullopt;
            }
        } else if (field.wireType == 2) {
            uint64_t length = 0;
            if (!readVarint(data, position, length) ||
                length > data.size() - position) {
                return std::nullopt;
            }
            field.bytes.assign(data.begin() + static_cast<ptrdiff_t>(position),
                               data.begin() + static_cast<ptrdiff_t>(position + length));
            position += static_cast<size_t>(length);
        } else if (field.wireType == 1) {
            if (data.size() - position < 8) {
                return std::nullopt;
            }
            position += 8;
        } else if (field.wireType == 5) {
            if (data.size() - position < 4) {
                return std::nullopt;
            }
            position += 4;
        } else {
            return std::nullopt;
        }
        fields.push_back(std::move(field));
    }
    return fields;
}

std::optional<ServiceDevice> parseDevice(const std::vector<uint8_t> &data)
{
    const auto fields = parseProtobuf(data);
    if (!fields) {
        return std::nullopt;
    }
    ServiceDevice device;
    for (const auto &field : *fields) {
        const std::string text(field.bytes.begin(), field.bytes.end());
        switch (field.number) {
        case 1: if (field.wireType == 2) device.name = text; break;
        case 3: if (field.wireType == 0) device.connectType = static_cast<int>(field.integer); break;
        case 4: if (field.wireType == 2) device.deviceCode = text; break;
        case 6: if (field.wireType == 2) device.mac = text; break;
        case 8: if (field.wireType == 0) device.connected = field.integer != 0; break;
        case 9: if (field.wireType == 0) device.level = static_cast<int>(field.integer); break;
        case 10: if (field.wireType == 2) device.firmware = text; break;
        case 11: if (field.wireType == 0) device.vid = static_cast<uint16_t>(field.integer); break;
        case 12: if (field.wireType == 0) device.pid = static_cast<uint16_t>(field.integer); break;
        default: break;
        }
    }
    if (device.name.empty() && device.mac.empty()) {
        return std::nullopt;
    }
    return device;
}

void parseDeviceContainer(const std::vector<uint8_t> &data,
                          std::map<std::string, ServiceDevice> &devices)
{
    const auto fields = parseProtobuf(data);
    if (!fields) {
        return;
    }
    for (const auto &field : *fields) {
        if (field.number != 1 || field.wireType != 2) {
            continue;
        }
        const auto device = parseDevice(field.bytes);
        if (device) {
            const std::string key = device->mac.empty() ? device->name : device->mac;
            devices[key] = *device;
        }
    }
}

bool readExactly(HANDLE pipe, uint8_t *buffer, size_t size, ULONGLONG deadline)
{
    size_t received = 0;
    while (received < size && GetTickCount64() < deadline) {
        DWORD available = 0;
        if (!PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr)) {
            return false;
        }
        if (available == 0) {
            Sleep(10);
            continue;
        }
        const DWORD wanted = static_cast<DWORD>((std::min)(
            size - received, static_cast<size_t>(available)));
        DWORD count = 0;
        if (!ReadFile(pipe, buffer + received, wanted, &count, nullptr) || count == 0) {
            return false;
        }
        received += count;
    }
    return received == size;
}

std::optional<std::vector<uint8_t>> readMessage(HANDLE pipe, DWORD timeoutMs)
{
    const ULONGLONG deadline = GetTickCount64() + timeoutMs;
    std::string header;
    while (header.size() < 64) {
        uint8_t byte = 0;
        if (!readExactly(pipe, &byte, 1, deadline)) {
            return std::nullopt;
        }
        header.push_back(static_cast<char>(byte));
        if (byte == '\n') {
            break;
        }
    }
    if (header != kProtocolHeader) {
        return std::nullopt;
    }

    uint8_t lengthBytes[4]{};
    if (!readExactly(pipe, lengthBytes, sizeof(lengthBytes), deadline)) {
        return std::nullopt;
    }
    const uint32_t length = static_cast<uint32_t>(lengthBytes[0]) |
        (static_cast<uint32_t>(lengthBytes[1]) << 8) |
        (static_cast<uint32_t>(lengthBytes[2]) << 16) |
        (static_cast<uint32_t>(lengthBytes[3]) << 24);
    if (length > kMaxPayloadSize) {
        return std::nullopt;
    }
    std::vector<uint8_t> payload(length);
    if (length != 0 && !readExactly(pipe, payload.data(), payload.size(), deadline)) {
        return std::nullopt;
    }
    return payload;
}

UniqueHandle connectPipe()
{
    for (int attempt = 0; attempt < 3; ++attempt) {
        HANDLE pipe = CreateFileW(kPipePath, GENERIC_READ | GENERIC_WRITE, 0,
                                  nullptr, OPEN_EXISTING, 0, nullptr);
        if (pipe != INVALID_HANDLE_VALUE) {
            return UniqueHandle(pipe);
        }
        if (GetLastError() != ERROR_PIPE_BUSY || !WaitNamedPipeW(kPipePath, 200)) {
            Sleep(50);
        }
    }
    return UniqueHandle(nullptr);
}

bool writeRequest(HANDLE pipe)
{
    constexpr uint8_t request[] = {0x08, 0x81, 0x20, 0x1a, 0x00};
    std::vector<uint8_t> frame;
    frame.insert(frame.end(), std::begin(kProtocolHeader), std::end(kProtocolHeader) - 1);
    const uint32_t length = sizeof(request);
    frame.push_back(static_cast<uint8_t>(length));
    frame.push_back(static_cast<uint8_t>(length >> 8));
    frame.push_back(static_cast<uint8_t>(length >> 16));
    frame.push_back(static_cast<uint8_t>(length >> 24));
    frame.insert(frame.end(), std::begin(request), std::end(request));
    DWORD written = 0;
    return WriteFile(pipe, frame.data(), static_cast<DWORD>(frame.size()),
                     &written, nullptr) && written == frame.size();
}

std::vector<ServiceDevice> queryService()
{
    auto pipe = connectPipe();
    if (!pipe) {
        return {};
    }

    std::map<std::string, ServiceDevice> devices;
    for (int attempt = 0; attempt < 3 && devices.empty(); ++attempt) {
        if (!writeRequest(static_cast<HANDLE>(pipe.get()))) {
            break;
        }
        const ULONGLONG deadline = GetTickCount64() + kReplyTimeoutMs;
        while (GetTickCount64() < deadline) {
            const DWORD remaining = static_cast<DWORD>(deadline - GetTickCount64());
            const auto payload = readMessage(static_cast<HANDLE>(pipe.get()), remaining);
            if (!payload) {
                break;
            }
            const auto fields = parseProtobuf(*payload);
            if (!fields) {
                continue;
            }
            uint64_t command = 0;
            for (const auto &field : *fields) {
                if (field.number == 1 && field.wireType == 0) {
                    command = field.integer;
                } else if (field.number == 6 && field.wireType == 2) {
                    parseDeviceContainer(field.bytes, devices);
                }
            }
            if (command == kGetConnectedDevices) {
                for (const auto &field : *fields) {
                    if (field.number == 5 && field.wireType == 2) {
                        parseDeviceContainer(field.bytes, devices);
                    }
                }
                break;
            }
        }
    }
    std::vector<ServiceDevice> result;
    for (auto &[key, device] : devices) {
        (void)key;
        result.push_back(std::move(device));
    }
    return result;
}

int percentageFromLevel(int level)
{
    constexpr int values[] = {5, 20, 40, 60, 80, 100};
    return level >= 0 && level < static_cast<int>(std::size(values))
        ? values[level] : -1;
}

BatteryLevel batteryLevelFromPercentage(int percentage)
{
    if (percentage < 0) return BatteryLevel::Unknown;
    if (percentage >= 80) return BatteryLevel::Full;
    if (percentage >= 50) return BatteryLevel::Medium;
    if (percentage >= 20) return BatteryLevel::Low;
    return BatteryLevel::Empty;
}
} // namespace

std::vector<BatteryDevice> readFlydigiControllerDevices()
{
    std::vector<BatteryDevice> result;
    for (const auto &source : queryService()) {
        if (!source.connected) {
            continue;
        }
        BatteryDevice device;
        const std::wstring mac = utf8ToWide(source.mac);
        const std::wstring code = utf8ToWide(source.deviceCode);
        device.id = !mac.empty() ? L"flydigi:" + mac
                                 : L"flydigi:" + code + L":" + utf8ToWide(source.name);
        device.name = utf8ToWide(source.name);
        if (device.name.empty()) {
            device.name = L"Flydigi Controller";
        }
        device.type = BatteryDevice::Type::Xbox;
        device.connected = true;
        device.wired = source.connectType == 1;
        device.charging = source.level == 6;
        device.percentage = percentageFromLevel(source.level);
        device.level = batteryLevelFromPercentage(device.percentage);
        result.push_back(std::move(device));

        LOG_VERBOSE_W(L"[Flydigi] " + utf8ToWide(source.name) + L" code=" + code +
                      L" mac=" + mac + L" batteryLevel=" +
                      std::to_wstring(source.level) + L" firmware=" +
                      utf8ToWide(source.firmware));
    }
    return result;
}
