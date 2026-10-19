// @min-host-version: 1.4
// K-snake X11 电量插件。
//
// 同时支持：
//   USB:        VID 0xA8A4 / PID 0x2255
//   2.4G Dongle: VID 0xA8A5 / PID 0x2255
//
// Vendor HID:
//   UsagePage: 0xFF01
//   Usage:     0x0010
//
// 获取电量：
//   55 30 A5 0B 2E 01 01 00 00 00 ...
//
// 响应：
//   AA 30 A5 71 0A 01 01 00 [battery] [charge_flag] ...

export function Name() {
    return "K-snake X11 (JS)";
}

export function VendorId() {
    return [0xA8A4, 0xA8A5];
}

export function ProductId() {
    return [0x2255];
}

const REPORT_SIZE = 64;
const REPORT_WITH_ID = 65;

const WAIT_MS = 20;
const READ_TIMEOUT_MS = 200;

const MAX_RETRIES = 3;
const MAX_READS_PER_TRY = 5;


// 构造电量查询包。
function makeBatteryRequest() {
    const out = new Array(REPORT_WITH_ID).fill(0);

    // HID Report ID
    out[0] = 0x00;

    // X11 协议
    out[1] = 0x55;
    out[2] = 0x30;
    out[3] = 0xA5;
    out[4] = 0x0B;
    out[5] = 0x2E;
    out[6] = 0x01;
    out[7] = 0x01;

    return out;
}


// 发送普通 HID Output Report。
function writeOutput(report) {
    if (typeof device.write !== "function") {
        return false;
    }

    try {
        const ret = device.write(
            report,
            report.length
        );

        return ret !== false;
    } catch (e) {
        return false;
    }
}


// 读取 HID Input Report。
function readInput() {
    if (typeof device.read !== "function") {
        return null;
    }

    // API 形式 1：
    // device.read(length, timeout)
    try {
        const r = device.read(
            REPORT_WITH_ID,
            READ_TIMEOUT_MS
        );

        if (
            r &&
            typeof r.length === "number" &&
            r.length > 0
        ) {
            return r;
        }
    } catch (e) {
        // 尝试下一种调用方式
    }


    // API 形式 2：
    // device.read(buffer, length, timeout)
    try {
        const buf = new Uint8Array(REPORT_WITH_ID);

        const ret = device.read(
            buf,
            REPORT_WITH_ID,
            READ_TIMEOUT_MS
        );

        if (
            ret &&
            typeof ret.length === "number" &&
            ret.length > 0
        ) {
            return ret;
        }

        if (
            typeof ret === "number" &&
            ret > 0
        ) {
            return buf.subarray(
                0,
                Math.min(ret, REPORT_WITH_ID)
            );
        }

    } catch (e) {
        return null;
    }

    return null;
}


// 去掉可能存在的 Report ID。
function normalizePayload(r) {
    if (
        !r ||
        typeof r.length !== "number"
    ) {
        return null;
    }


    // 00 AA 30 ...
    if (
        r.length >= REPORT_WITH_ID &&
        r[0] === 0x00 &&
        r[1] === 0xAA
    ) {
        return r.subarray
            ? r.subarray(1, 1 + REPORT_SIZE)
            : r.slice(1, 1 + REPORT_SIZE);
    }


    // AA 30 ...
    if (
        r.length >= REPORT_SIZE &&
        r[0] === 0xAA
    ) {
        return r.subarray
            ? r.subarray(0, REPORT_SIZE)
            : r.slice(0, REPORT_SIZE);
    }


    // 未知 Report ID + AA 30 ...
    if (
        r.length >= REPORT_WITH_ID &&
        r[1] === 0xAA
    ) {
        return r.subarray
            ? r.subarray(1, 1 + REPORT_SIZE)
            : r.slice(1, 1 + REPORT_SIZE);
    }


    return null;
}


// AA 30 A5 ...
function isBatteryResponse(p) {
    return (
        p &&
        p.length >= 10 &&
        p[0] === 0xAA &&
        p[1] === 0x30 &&
        p[2] === 0xA5
    );
}


function queryBattery() {
    const req = makeBatteryRequest();

    for (
        let retry = 0;
        retry < MAX_RETRIES;
        retry++
    ) {

        if (!writeOutput(req)) {
            return null;
        }


        if (
            WAIT_MS > 0 &&
            typeof device.pause === "function"
        ) {
            device.pause(WAIT_MS);
        }


        // Vendor HID 可能同时出现 AA FA 等主动通知，
        // 所以过滤到真正的 AA 30 A5 电量响应。
        for (
            let i = 0;
            i < MAX_READS_PER_TRY;
            i++
        ) {
            const raw = readInput();

            if (
                !raw ||
                raw.length === 0
            ) {
                break;
            }

            const p = normalizePayload(raw);

            if (isBatteryResponse(p)) {
                return p;
            }
        }
    }


    return null;
}


export function GetBattery() {
    const resp = queryBattery();

    if (!resp) {
        return null;
    }

    let percentage = resp[8];

    if (percentage < 0) {
        percentage = 0;
    }

    if (percentage > 100) {
        percentage = 100;
    }

    return {
        percentage: percentage,
        charging: resp[9] !== 0
    };
}
