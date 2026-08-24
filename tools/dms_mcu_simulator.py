#!/usr/bin/env python3
"""
dms_mcu_simulator.py - RV1106 协议模拟器

通过串口或虚拟串口模拟 RV1106 发送 DMS 协议帧。
用于在没有 RV1106 主程序的情况下测试 MCU 侧。

用法：
    # 发送单个事件
    python3 tools/dms_mcu_simulator.py --port /dev/ttyUSB0 \
        --event LONG_EYE_CLOSED --risk WARNING --duration 1800

    # 持续发送心跳
    python3 tools/dms_mcu_simulator.py --port /dev/ttyUSB0 --heartbeat

    # 运行场景
    python3 tools/dms_mcu_simulator.py --port /dev/ttyUSB0 --scenario warning

    # 虚拟串口测试（无硬件）
    python3 tools/dms_mcu_simulator.py --virtual --scenario high
"""

import argparse
import struct
import sys
import time
import os

# ==================== 协议常量 ====================
HEADER_0 = 0xAA
HEADER_1 = 0x55
VERSION = 0x01

EVENTS = {
    'HEARTBEAT':       0x01,
    'EYE_CLOSED':      0x10,
    'LONG_EYE_CLOSED': 0x11,
    'YAWN':            0x12,
    'HEAD_DOWN':       0x13,
    'FACE_LOST':       0x14,
    'FATIGUE_WARNING': 0x20,
    'FATIGUE_HIGH':    0x21,
}

RISK_LEVELS = {
    'NORMAL':    0,
    'ATTENTION': 1,
    'WARNING':   2,
    'HIGH':      3,
}


def crc16_ccitt(data: bytes) -> int:
    """CRC16-CCITT (poly 0x1021, init 0xFFFF)"""
    crc = 0xFFFF
    for byte in data:
        crc ^= byte << 8
        for _ in range(8):
            if crc & 0x8000:
                crc = ((crc << 1) ^ 0x1021) & 0xFFFF
            else:
                crc = (crc << 1) & 0xFFFF
    return crc


def encode_frame(event: int, risk_level: int = 0, confidence: int = 100,
                 duration_ms: int = 0, timestamp: int = 0,
                 payload: bytes = b'') -> bytes:
    """编码一帧 DMS 协议数据"""
    frame = bytearray()
    frame.append(HEADER_0)
    frame.append(HEADER_1)
    frame.append(VERSION)
    frame.append(event)
    frame.append(risk_level)
    frame.append(confidence)
    frame += struct.pack('>H', duration_ms)   # big-endian uint16
    frame += struct.pack('>I', timestamp)      # big-endian uint32
    frame.append(len(payload))
    frame += payload
    crc = crc16_ccitt(bytes(frame))
    frame += struct.pack('>H', crc)
    return bytes(frame)


def encode_heartbeat(risk_level: int = 0, dms_alive: int = 1,
                     camera_alive: int = 1, ai_alive: int = 1,
                     timestamp: int = 0) -> bytes:
    """编码心跳帧"""
    payload = bytes([dms_alive, camera_alive, ai_alive, risk_level])
    return encode_frame(
        event=EVENTS['HEARTBEAT'],
        risk_level=risk_level,
        confidence=100,
        duration_ms=0,
        timestamp=timestamp,
        payload=payload
    )


def open_port(port: str, baud: int = 115200):
    """打开串口"""
    try:
        import serial
        ser = serial.Serial(port, baud, timeout=0.1)
        print(f"[SIM] Opened {port} @ {baud}")
        return ser
    except ImportError:
        print("[SIM] pyserial not installed. Install: pip install pyserial")
        sys.exit(1)
    except Exception as e:
        print(f"[SIM] Failed to open {port}: {e}")
        sys.exit(1)


def open_virtual():
    """打开虚拟输出（stdout，用于管道测试）"""
    print("[SIM] Virtual mode: writing to stdout", file=sys.stderr)
    return sys.stdout.buffer


def send_frame(port, frame: bytes, verbose: bool = True):
    """发送一帧数据"""
    port.write(frame)
    if hasattr(port, 'flush'):
        port.flush()
    if verbose:
        hex_str = ' '.join(f'{b:02X}' for b in frame)
        print(f"[SIM] TX ({len(frame)}B): {hex_str}")


def run_heartbeat(port, risk: int = 0, interval: float = 1.0, count: int = 0):
    """持续发送心跳"""
    print(f"[SIM] Heartbeat mode: risk={risk}, interval={interval}s")
    i = 0
    try:
        while True:
            ts = int(time.time())
            frame = encode_heartbeat(risk_level=risk, timestamp=ts)
            send_frame(port, frame)
            i += 1
            if count > 0 and i >= count:
                break
            time.sleep(interval)
    except KeyboardInterrupt:
        print(f"\n[SIM] Sent {i} heartbeats")


def run_single_event(port, event: int, risk: int, duration: int,
                     confidence: int = 95):
    """发送单个事件"""
    ts = int(time.time())
    frame = encode_frame(event=event, risk_level=risk,
                         confidence=confidence, duration_ms=duration,
                         timestamp=ts)
    send_frame(port, frame)


def run_scenario(port, scenario: str):
    """运行预定义场景"""
    print(f"[SIM] Running scenario: {scenario}")

    if scenario == 'normal':
        # 正常驾驶：只有心跳
        for i in range(10):
            ts = int(time.time())
            frame = encode_heartbeat(risk_level=0, timestamp=ts)
            send_frame(port, frame)
            time.sleep(1.0)

    elif scenario == 'attention':
        # 轻度注意：心跳 + 单次哈欠
        for i in range(3):
            ts = int(time.time())
            send_frame(port, encode_heartbeat(risk_level=0, timestamp=ts))
            time.sleep(1.0)

        # 单次哈欠
        ts = int(time.time())
        send_frame(port, encode_frame(EVENTS['YAWN'], risk_level=1,
                                       confidence=90, duration_ms=1200,
                                       timestamp=ts))
        time.sleep(2.0)

        for i in range(3):
            ts = int(time.time())
            send_frame(port, encode_heartbeat(risk_level=1, timestamp=ts))
            time.sleep(1.0)

    elif scenario == 'warning':
        # 警告：心跳 + 长闭眼
        for i in range(3):
            ts = int(time.time())
            send_frame(port, encode_heartbeat(risk_level=0, timestamp=ts))
            time.sleep(1.0)

        # 长闭眼
        ts = int(time.time())
        send_frame(port, encode_frame(EVENTS['LONG_EYE_CLOSED'],
                                       risk_level=2, confidence=92,
                                       duration_ms=1800, timestamp=ts))
        time.sleep(1.0)

        # 持续 WARNING 心跳
        for i in range(5):
            ts = int(time.time())
            send_frame(port, encode_heartbeat(risk_level=2, timestamp=ts))
            time.sleep(1.0)

    elif scenario == 'high':
        # 高危：心跳 + 重复长闭眼
        for i in range(2):
            ts = int(time.time())
            send_frame(port, encode_heartbeat(risk_level=0, timestamp=ts))
            time.sleep(1.0)

        # 第一次长闭眼
        ts = int(time.time())
        send_frame(port, encode_frame(EVENTS['LONG_EYE_CLOSED'],
                                       risk_level=2, confidence=92,
                                       duration_ms=1600, timestamp=ts))
        time.sleep(2.0)

        # 第二次长闭眼
        ts = int(time.time())
        send_frame(port, encode_frame(EVENTS['LONG_EYE_CLOSED'],
                                       risk_level=3, confidence=95,
                                       duration_ms=2000, timestamp=ts))
        time.sleep(1.0)

        # FATIGUE_HIGH
        ts = int(time.time())
        send_frame(port, encode_frame(EVENTS['FATIGUE_HIGH'],
                                       risk_level=3, confidence=95,
                                       duration_ms=3000, timestamp=ts))

        # 持续 HIGH 心跳
        for i in range(5):
            ts = int(time.time())
            send_frame(port, encode_heartbeat(risk_level=3, timestamp=ts))
            time.sleep(1.0)

    elif scenario == 'heartbeat_loss':
        # 心跳丢失：发 3 个心跳后停止
        print("[SIM] Sending 3 heartbeats then stopping (simulating loss)...")
        for i in range(3):
            ts = int(time.time())
            send_frame(port, encode_heartbeat(risk_level=0, timestamp=ts))
            time.sleep(1.0)
        print("[SIM] Heartbeat stopped. MCU should detect timeout in ~5s.")
        time.sleep(10)

    else:
        print(f"[SIM] Unknown scenario: {scenario}")
        print(f"[SIM] Available: normal, attention, warning, high, heartbeat_loss")

    print(f"[SIM] Scenario '{scenario}' complete")


def main():
    parser = argparse.ArgumentParser(description='DMS MCU Protocol Simulator')
    parser.add_argument('--port', type=str, help='Serial port (e.g. /dev/ttyUSB0)')
    parser.add_argument('--baud', type=int, default=115200, help='Baud rate')
    parser.add_argument('--virtual', action='store_true',
                        help='Virtual mode (stdout)')
    parser.add_argument('--event', type=str, choices=EVENTS.keys(),
                        help='Event to send')
    parser.add_argument('--risk', type=str, default='NORMAL',
                        choices=RISK_LEVELS.keys(), help='Risk level')
    parser.add_argument('--duration', type=int, default=0,
                        help='Event duration in ms')
    parser.add_argument('--confidence', type=int, default=95,
                        help='Confidence 0-100')
    parser.add_argument('--heartbeat', action='store_true',
                        help='Send continuous heartbeat')
    parser.add_argument('--scenario', type=str,
                        choices=['normal', 'attention', 'warning', 'high',
                                 'heartbeat_loss'],
                        help='Run predefined scenario')
    parser.add_argument('--count', type=int, default=0,
                        help='Number of frames (0=unlimited for heartbeat)')

    args = parser.parse_args()

    # 打开端口
    if args.virtual:
        port = open_virtual()
    elif args.port:
        port = open_port(args.port, args.baud)
    else:
        print("[SIM] Specify --port or --virtual")
        sys.exit(1)

    risk_val = RISK_LEVELS[args.risk]

    if args.heartbeat:
        run_heartbeat(port, risk=risk_val, count=args.count)
    elif args.scenario:
        run_scenario(port, args.scenario)
    elif args.event:
        event_val = EVENTS[args.event]
        run_single_event(port, event_val, risk_val, args.duration,
                         args.confidence)
    else:
        # 默认发送一个心跳
        frame = encode_heartbeat(risk_level=risk_val,
                                 timestamp=int(time.time()))
        send_frame(port, frame)

    if hasattr(port, 'close'):
        port.close()


if __name__ == '__main__':
    main()
