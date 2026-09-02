#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Read battery and charging information from Flydigi game controllers.

Two query paths, tried in order:
  1. Local IPC: the vendor background service exposes device state over the
     named pipe ``fcs.sock`` (protobuf messages, ``FDG_PROTOCOL`` framing).
     Works in every connection mode, including XInput over a wireless dongle.
  2. Direct HID: for new-generation devices (VID 0x37E7, usage page 0xFFF0)
     and legacy DInput interfaces, when the service is not running.

Battery is reported by the device as a coarse level:
    0..5 = discharge level (0 lowest), 6 = charging.

Dependency (only needed for the direct-HID fallback):
    python -m pip install hidapi

Examples:
    python scripts/flydigi_controller_battery.py
    python scripts/flydigi_controller_battery.py --verbose
    python scripts/flydigi_controller_battery.py --count 10 --interval 1
"""

from __future__ import annotations

import argparse
import struct
import sys
import time
from dataclasses import dataclass
from typing import Any

try:
    import hid
except ImportError:
    hid = None


IPC_PIPE_NAME = "fcs.sock"
IPC_HEADER = b"FDG_PROTOCOL\n"
IPC_CMD_GET_CONNECTED_DEVICES = 4097
IPC_REPLY_TIMEOUT = 4.0


VID_FLYDIGI = 0x37E7
VID_XINPUT_COMPAT = 0x045E
USAGE_PAGE_VENDOR = 0xFFF0

READ_LENGTH = 64
NEW_PROTOCOL_PACKET = 32
NEW_HEADER_LEN = 2
CMD_NEW_HEARTBEAT = 0x01
CMD_XINPUT_HEARTBEAT = 0x10
CMD_DINPUT_HEARTBEAT = 0xEC

# 大致把 0-5 档换算成百分比用于展示，仅作参考。
LEVEL_TO_PERCENT = {0: 5, 1: 20, 2: 40, 3: 60, 4: 80, 5: 100}
CHARGING_LEVEL = 6


def additive_crc(data: bytearray, start: int, end: int) -> int:
    return sum(data[start:end]) & 0xFF


def build_new_heartbeat() -> bytes:
    packet = bytearray(NEW_PROTOCOL_PACKET)
    packet[1] = 0x5A
    packet[2] = 0xA5
    packet[3] = CMD_NEW_HEARTBEAT
    packet[4] = NEW_HEADER_LEN
    packet[5] = additive_crc(packet, 3, 3 + NEW_HEADER_LEN)
    return bytes(packet)


def build_legacy_heartbeat(command_id: int) -> bytes:
    packet = bytearray(15)
    packet[1] = command_id
    return bytes(packet)


@dataclass(frozen=True)
class ServiceDevice:
    name: str
    device_code: str
    mac: str
    connected: bool
    level: int
    firmware: str
    vid: int
    pid: int
    connect_type: int


def _pb_read_varint(data: bytes, pos: int) -> tuple[int, int]:
    value = 0
    shift = 0
    while True:
        b = data[pos]
        pos += 1
        value |= (b & 0x7F) << shift
        if not b & 0x80:
            return value, pos
        shift += 7


def _pb_fields(data: bytes):
    """Yield (field_number, wire_type, value) triples of a protobuf message."""
    pos = 0
    while pos < len(data):
        tag, pos = _pb_read_varint(data, pos)
        field, wire = tag >> 3, tag & 7
        if wire == 0:
            value, pos = _pb_read_varint(data, pos)
        elif wire == 2:
            ln, pos = _pb_read_varint(data, pos)
            value = data[pos:pos + ln]
            pos += ln
        elif wire == 5:
            value = data[pos:pos + 4]
            pos += 4
        elif wire == 1:
            value = data[pos:pos + 8]
            pos += 8
        else:
            raise ValueError(f"unsupported wire type {wire}")
        yield field, wire, value


def _parse_device_info(data: bytes) -> ServiceDevice:
    info: dict[str, Any] = {
        "name": "", "device_code": "", "mac": "", "connected": False,
        "level": -1, "firmware": "", "vid": 0, "pid": 0, "connect_type": 0,
    }
    for field, _wire, value in _pb_fields(data):
        if field == 1:
            info["name"] = value.decode(errors="replace")
        elif field == 3:
            info["connect_type"] = value
        elif field == 4:
            info["device_code"] = value.decode(errors="replace")
        elif field == 6:
            info["mac"] = value.decode(errors="replace")
        elif field == 8:
            info["connected"] = bool(value)
        elif field == 9:
            info["level"] = value
        elif field == 10:
            info["firmware"] = value.decode(errors="replace")
        elif field == 11:
            info["vid"] = value
        elif field == 12:
            info["pid"] = value
    return ServiceDevice(**info)


class ServiceClient:
    """与本地后台服务保持一条长连接，读取设备状态。

    服务侧消息：IpcResult(cmdId, state, ...)。
    cmdId=4097 时 field5 为设备列表；cmdId=4098 时 field6 为单个设备
    状态变化事件。设备列表偶发为空，此时可等待变化事件补充。
    """

    def __init__(self, verbose: bool = False):
        self.verbose = verbose
        self._pipe = None

    def connect(self) -> bool:
        if self._pipe is not None:
            return True
        pipe_path = "\\\\.\\pipe\\" + IPC_PIPE_NAME
        for _ in range(3):
            try:
                self._pipe = open(pipe_path, "r+b", buffering=0)
                return True
            except OSError as exc:
                if self.verbose:
                    print(f"IPC 连接失败: {exc}")
                time.sleep(0.3)
        return False

    def close(self) -> None:
        if self._pipe is not None:
            try:
                self._pipe.close()
            except OSError:
                pass
            self._pipe = None

    def _read_message(self, timeout: float) -> bytes | None:
        assert self._pipe is not None
        deadline = time.time() + timeout
        header = b""
        while True:
            c = self._pipe.read(1)
            if not c:
                return None
            if c == b"\n":
                break
            header += c
        if header.strip() != IPC_HEADER.strip():
            return None
        remaining = deadline - time.time()
        if remaining <= 0:
            return None
        length_bytes = self._read_exactly(4, remaining)
        if length_bytes is None:
            return None
        ln = struct.unpack("<i", length_bytes)[0]
        remaining = deadline - time.time()
        payload = self._read_exactly(ln, max(remaining, 0.05))
        return payload

    def _read_exactly(self, count: int, timeout: float) -> bytes | None:
        assert self._pipe is not None
        data = b""
        deadline = time.time() + timeout
        while len(data) < count:
            chunk = self._pipe.read(count - len(data))
            if chunk:
                data += chunk
            elif time.time() > deadline:
                return None
            else:
                time.sleep(0.02)
        return data

    def query_devices(self) -> list[ServiceDevice] | None:
        """返回在线设备列表；服务不可用时返回 None。

        每次请求都必有应答，读取不会无限阻塞；设备列表偶发为空，
        此时重发请求，并顺带收集服务推送的设备状态变化事件。
        """
        if self._pipe is None and not self.connect():
            return None
        assert self._pipe is not None
        devices: dict[str, ServiceDevice] = {}
        for _attempt in range(3):
            request = bytes([0x08, IPC_CMD_GET_CONNECTED_DEVICES & 0x7F | 0x80,
                             IPC_CMD_GET_CONNECTED_DEVICES >> 7, 0x1A, 0x00])
            try:
                self._pipe.write(IPC_HEADER + struct.pack("<i", len(request)) + request)
            except OSError as exc:
                if self.verbose:
                    print(f"IPC 写入失败: {exc}")
                self.close()
                return None
            deadline = time.time() + IPC_REPLY_TIMEOUT
            while time.time() < deadline:
                try:
                    payload = self._read_message(deadline - time.time())
                except OSError as exc:
                    if self.verbose:
                        print(f"IPC 读取失败: {exc}")
                    self.close()
                    return None
                if payload is None:
                    break
                try:
                    fields = list(_pb_fields(payload))
                except ValueError:
                    continue
                cmd_id = next((v for f, _w, v in fields if f == 1), None)
                list_bytes = next((v for f, _w, v in fields
                                   if f == 5 and isinstance(v, bytes)), None)
                changed_bytes = next((v for f, _w, v in fields
                                      if f == 6 and isinstance(v, bytes)), None)
                if changed_bytes is not None:
                    for f2, _w2, v2 in _pb_fields(changed_bytes):
                        if f2 == 1 and isinstance(v2, bytes):
                            dev = _parse_device_info(v2)
                            devices[dev.mac or dev.name] = dev
                if cmd_id == IPC_CMD_GET_CONNECTED_DEVICES and list_bytes is not None:
                    for f2, _w2, v2 in _pb_fields(list_bytes):
                        if f2 == 1 and isinstance(v2, bytes):
                            dev = _parse_device_info(v2)
                            devices[dev.mac or dev.name] = dev
                    break
            if devices:
                return list(devices.values())
            time.sleep(0.3)
        return list(devices.values())


def model_from_product(product: str) -> str:
    keywords = (
        "APEX 6", "APEX 5", "APEX 4", "APEX 3", "APEX 2",
        "VADER 5", "VADER 4", "VADER 3", "VADER 2",
        "Direwolf 4", "Direwolf 3", "Direwolf 2", "Direwolf",
    )
    for kw in keywords:
        if kw.lower() in product.lower():
            return f"Flydigi {kw}"
    return product or "Flydigi 手柄"


@dataclass(frozen=True)
class Candidate:
    path: Any
    vid: int
    pid: int
    interface_number: int
    usage_page: int
    usage: int
    product: str
    serial: str
    kind: str  # "new-controller" / "dock" / "legacy"

    @property
    def name(self) -> str:
        if self.kind == "dock":
            return "Flydigi 充电底座"
        return model_from_product(self.product)

    @property
    def report_ids(self) -> tuple[int, ...]:
        if self.kind == "new-controller":
            return (0x06, 0x05, 0x00)
        if self.kind == "dock":
            return (0x06, 0x00)
        return (0xA5, 0x05, 0x00)


def is_controller(pid: int) -> bool:
    return pid >> 12 == 2 and (pid & 0xFF00) != 0x2800


def is_dock(pid: int) -> bool:
    return pid >> 12 == 6


MODEL_KEYWORDS = (
    "APEX", "VADER", "DIREWOLF", "WASP", "STORM", "WEE",
)


def product_is_flydigi(product: str) -> bool:
    lowered = product.lower()
    return "flydigi" in lowered or any(k.lower() in lowered for k in MODEL_KEYWORDS)


def enumerate_candidates() -> list[Candidate]:
    found: list[Candidate] = []
    for info in hid.enumerate():
        vid, pid = info["vendor_id"], info["product_id"]
        usage_page = info.get("usage_page", 0)
        interface = info.get("interface_number", -1)
        product = info.get("product_string") or ""
        manufacturer = info.get("manufacturer_string") or ""
        serial = info.get("serial_number") or ""
        path = info["path"]

        if vid == VID_FLYDIGI and usage_page == USAGE_PAGE_VENDOR:
            if is_controller(pid):
                found.append(Candidate(path, vid, pid, interface, usage_page,
                                       info.get("usage", 0), product, serial, "new-controller"))
            elif is_dock(pid):
                found.append(Candidate(path, vid, pid, interface, usage_page,
                                       info.get("usage", 0), product, serial, "dock"))
        elif product_is_flydigi(product) and usage_page == USAGE_PAGE_VENDOR:
            kind = "legacy-xinput" if manufacturer == "Microsoft" else "legacy-dinput"
            found.append(Candidate(path, vid, pid, interface, usage_page,
                                   info.get("usage", 0), product, serial, kind))
        elif vid == VID_XINPUT_COMPAT and product_is_flydigi(product):
            # XInput 兼容集合没有厂商输出报告，控制命令需要专用驱动通道，
            # 这里仅作标记，读取电量请把手柄切到 DInput/无线模式。
            found.append(Candidate(path, vid, pid, interface, usage_page,
                                   info.get("usage", 0), product, serial, "xinput-driver"))
    return found


def decode_battery(raw: int) -> tuple[int, bool]:
    """高 4 位为 1 表示充电中，低 4 位为 0-5 电量档。"""
    if raw >> 4 == 1:
        return CHARGING_LEVEL, True
    return raw & 0xF, False


def bcd_pair(value: int) -> str:
    return f"{value >> 4}.{value & 0xF}"


def parse_new_controller(data: bytes) -> dict[str, Any]:
    # data[2] == 0x01 已由调用方校验；分片首包 data[4] == 0。
    more = data[4] < data[3]
    pos = 5 if more else 4
    device_type = data[pos]
    connect = ("有线" if data[pos + 1] == 1 else "适配器/无线")
    mac = ":".join(f"{b:02X}" for b in reversed(data[pos + 2:pos + 6]))
    pos += 6
    battery_raw = data[pos]
    level, charging = decode_battery(battery_raw)
    pos += 1
    pos += 1  # 芯片类型
    pos += 1  # 体感芯片类型
    pos += 1
    fw = f"{bcd_pair(data[pos])}.{bcd_pair(data[pos + 1])}"
    return {
        "device_type": device_type,
        "connect": connect,
        "mac": mac,
        "level": level,
        "charging": charging,
        "firmware": fw,
    }


def parse_dock(data: bytes) -> dict[str, Any]:
    connected = data[7] == 1
    level, charging = decode_battery(data[8])
    fw = f"{bcd_pair(data[16])}.{bcd_pair(data[17])}"
    return {
        "connected": connected,
        "level": level if connected else None,
        "charging": charging if connected else False,
        "firmware": fw,
    }


def parse_legacy_xinput(data: bytes) -> dict[str, Any]:
    mac = ":".join(f"{b:02X}" for b in (data[20], data[19], data[18], data[17]))
    level, charging = decode_battery(data[23])
    fw = f"{bcd_pair(data[22])}.{bcd_pair(data[21])}"
    return {
        "mac": mac,
        "level": level,
        "charging": charging,
        "firmware": fw,
        "connect": "有线" if data[25] == 1 else "适配器/无线",
    }


def parse_legacy_dinput(data: bytes) -> dict[str, Any]:
    mac = ":".join(f"{b:02X}" for b in (data[7], data[6], data[5], data[4]))
    level, charging = decode_battery(data[10])
    fw = f"{bcd_pair(data[9])}.{bcd_pair(data[8])}"
    return {
        "mac": mac,
        "level": level,
        "charging": charging,
        "firmware": fw,
        "connect": "有线" if data[12] == 1 else "适配器/无线",
    }


def query(candidate: Candidate, verbose: bool) -> dict[str, Any] | None:
    if candidate.kind == "xinput-driver":
        return {"level": None, "note": "XInput 模式无厂商 HID 输出通道，请切换到 DInput/无线模式后重试"}
    if candidate.kind in ("new-controller", "dock"):
        payload = build_new_heartbeat()
    elif candidate.kind == "legacy-xinput":
        payload = build_legacy_heartbeat(CMD_XINPUT_HEARTBEAT)
    else:
        payload = build_legacy_heartbeat(CMD_DINPUT_HEARTBEAT)

    device = hid.Device(path=candidate.path)
    try:
        last_error: Exception | None = None
        for report_id in candidate.report_ids:
            frame = bytes([report_id]) + payload[1:]
            try:
                device.write(frame)
            except Exception as exc:  # noqa: BLE001
                last_error = exc
                continue
            deadline = time.time() + 0.6
            while time.time() < deadline:
                chunk = device.read(READ_LENGTH, timeout_ms=100)
                if not chunk:
                    continue
                for data in (bytes(chunk), bytes(chunk[1:])):
                    if len(data) < 16:
                        continue
                    if candidate.kind in ("new-controller", "dock"):
                        if data[2] != CMD_NEW_HEARTBEAT:
                            continue
                        if data[4] != 0:
                            continue
                        result = parse_new_controller(data) if candidate.kind == "new-controller" else parse_dock(data)
                    elif candidate.kind == "legacy-xinput":
                        if len(data) <= 23 or data[15] != CMD_XINPUT_HEARTBEAT:
                            continue
                        result = parse_legacy_xinput(data)
                    else:
                        if data[14] != CMD_DINPUT_HEARTBEAT:
                            continue
                        result = parse_legacy_dinput(data)
                    result["report_id"] = report_id
                    result["raw"] = bytes(chunk).hex(" ")
                    return result
        if verbose and last_error is not None:
            print(f"    写入失败: {last_error}")
        return None
    finally:
        device.close()


def describe(result: dict[str, Any], candidate: Candidate) -> str:
    level = result.get("level")
    if level is None:
        battery = result.get("note") or "未连接手柄"
    elif result.get("charging"):
        battery = "充电中"
    else:
        battery = f"{LEVEL_TO_PERCENT.get(level, level * 20)}%（档位 {level}/5）"
    parts = [f"{candidate.name} [{candidate.vid:04X}:{candidate.pid:04X}]"]
    if "connect" in result:
        parts.append(f"连接: {result['connect']}")
    parts.append(f"电量: {battery}")
    if result.get("mac"):
        parts.append(f"MAC: {result['mac']}")
    if result.get("firmware"):
        parts.append(f"固件: {result['firmware']}")
    return "  ".join(parts)


def describe_service_device(dev: ServiceDevice) -> str:
    if dev.level == 6:
        battery = "充电中"
    elif 0 <= dev.level <= 5:
        battery = f"{LEVEL_TO_PERCENT.get(dev.level, dev.level * 20)}%（档位 {dev.level}/5）"
    else:
        battery = "未知"
    connect = {0: "未知", 1: "有线", 2: "无线"}.get(dev.connect_type, f"类型{dev.connect_type}")
    parts = [f"{dev.name} [{dev.vid:04X}:{dev.pid:04X}]"]
    if dev.device_code:
        parts.append(f"型号代码: {dev.device_code}")
    parts.append(f"连接: {'在线' if dev.connected else '离线'}（{connect}）")
    parts.append(f"电量: {battery}")
    if dev.mac:
        parts.append(f"MAC: {dev.mac}")
    if dev.firmware:
        parts.append(f"固件: {dev.firmware}")
    return "  ".join(parts)


def main() -> int:
    parser = argparse.ArgumentParser(description="读取飞智手柄/底座电量信息")
    parser.add_argument("--count", type=int, default=1, help="轮询次数（默认 1，<1 表示无限）")
    parser.add_argument("--interval", type=float, default=2.0, help="轮询间隔秒数")
    parser.add_argument("--no-service", action="store_true", help="跳过本地服务通道，直接走 HID")
    parser.add_argument("--verbose", action="store_true", help="输出原始报告内容")
    args = parser.parse_args()

    client = None if args.no_service else ServiceClient(args.verbose)
    if client is not None and not client.connect():
        client = None
    candidates = enumerate_candidates() if hid is not None else []

    if client is None and not candidates:
        print("未发现飞智手柄或充电底座设备（后台服务未运行且未发现可直连的 HID 设备）")
        return 1
        print("未发现飞智手柄或充电底座设备（后台服务未运行且未发现可直连的 HID 设备）")
        return 1

    exit_code = 0
    rounds = args.count if args.count >= 1 else 10**9
    for round_no in range(rounds):
        print(f"\n== 第 {round_no + 1} 轮 ==")
        if client is not None:
            devices = client.query_devices()
            if devices is not None:
                if devices:
                    for dev in devices:
                        print(f"  [本地服务] {describe_service_device(dev)}")
                else:
                    print("  [本地服务] 服务已连接，但当前没有在线设备")
                continue
            print("  本地服务不可用，回退到 HID 直连")
            client = None
        if not candidates:
            exit_code = 1
            continue
        for cand in candidates:
            result = query(cand, args.verbose)
            if result is None:
                print(f"  {cand.name}: 无应答")
                exit_code = exit_code or 1
                continue
            print("  " + describe(result, cand))
            if args.verbose and "raw" in result:
                print(f"    report_id=0x{result['report_id']:02X} raw: {result['raw']}")
        if round_no + 1 < rounds:
            time.sleep(args.interval)
    if client is not None:
        client.close()
    return exit_code


if __name__ == "__main__":
    raise SystemExit(main())
