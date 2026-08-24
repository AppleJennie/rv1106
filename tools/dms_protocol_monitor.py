#!/usr/bin/env python3
"""
dms_protocol_monitor.py - DMS 协议抓包监控工具

读取 UART 数据，解析 DMS 协议帧，打印可读信息。
用于 RV1106 和 MCU 联调时监控通讯。

用法：
    # 监控串口
    python3 tools/dms_protocol_monitor.py --port /dev/ttyUSB0

    # 从文件读取（离线分析）
    python3 tools/dms_protocol_monitor.py --file capture.bin

    # 虚拟模式（从 stdin 读取，配合模拟器管道）
    python3 tools/dms_mcu_simulator.py --virtual --scenario warning | \
        python3 tools/dms_protocol_monitor.py --stdin
"""

import argparse
import struct
import sys
import time
from datetime import datetime

# ==================== 协议常量 ====================
HEADER_0 = 0xAA
HEADER_1 = 0x55
VERSION = 0x01
MAX_PAYLOAD = 32
FIXED_LEN = 13  # header(2)+ver(1)+event(1)+risk(1)+conf(1)+dur(2)+ts(4)+len(1)
CRC_LEN = 2

EVENT_NAMES = {
    0x01: 'HEARTBEAT',
    0x10: 'EYE_CLOSED',
    0x11: 'LONG_EYE_CLOSED',
    0x12: 'YAWN',
    0x13: 'HEAD_DOWN',
    0x14: 'FACE_LOST',
    0x20: 'FATIGUE_WARNING',
    0x21: 'FATIGUE_HIGH',
}

RISK_NAMES = {0: 'NORMAL', 1: 'ATTENTION', 2: 'WARNING', 3: 'HIGH'}


def crc16_ccitt(data: bytes) -> int:
    crc = 0xFFFF
    for byte in data:
        crc ^= byte << 8
        for _ in range(8):
            if crc & 0x8000:
                crc = ((crc << 1) ^ 0x1021) & 0xFFFF
            else:
                crc = (crc << 1) & 0xFFFF
    return crc


class ProtocolMonitor:
    """DMS 协议监控器（字节流解析）"""

    STATE_SYNC0 = 0
    STATE_SYNC1 = 1
    STATE_FIXED = 2
    STATE_PAYLOAD = 3
    STATE_CRC = 4

    def __init__(self):
        self.state = self.STATE_SYNC0
        self.buf = bytearray()
        self.payload_len = 0
        self.crc_bytes = 0

        # 统计
        self.total_bytes = 0
        self.valid_frames = 0
        self.crc_errors = 0
        self.resync_count = 0
        self.parser_errors = 0

    def feed_byte(self, byte: int) -> bool:
        """喂一个字节，返回是否解析出完整帧"""
        self.total_bytes += 1

        if self.state == self.STATE_SYNC0:
            if byte == HEADER_0:
                self.buf = bytearray([byte])
                self.state = self.STATE_SYNC1
            return False

        elif self.state == self.STATE_SYNC1:
            if byte == HEADER_1:
                self.buf.append(byte)
                self.state = self.STATE_FIXED
            elif byte == HEADER_0:
                self.buf = bytearray([byte])
            else:
                self.resync_count += 1
                self.state = self.STATE_SYNC0
            return False

        elif self.state == self.STATE_FIXED:
            self.buf.append(byte)
            if len(self.buf) >= FIXED_LEN:
                self.payload_len = self.buf[12]
                if self.payload_len > MAX_PAYLOAD:
                    self.parser_errors += 1
                    self.resync_count += 1
                    self.state = self.STATE_SYNC0
                    self.buf = bytearray()
                    return False
                if self.payload_len > 0:
                    self.state = self.STATE_PAYLOAD
                else:
                    self.state = self.STATE_CRC
                    self.crc_bytes = 0
            return False

        elif self.state == self.STATE_PAYLOAD:
            self.buf.append(byte)
            if len(self.buf) >= FIXED_LEN + self.payload_len:
                self.state = self.STATE_CRC
                self.crc_bytes = 0
            return False

        elif self.state == self.STATE_CRC:
            self.buf.append(byte)
            self.crc_bytes += 1
            if self.crc_bytes >= CRC_LEN:
                self._process_frame()
                self.state = self.STATE_SYNC0
                self.buf = bytearray()
                return True
            return False

        return False

    def _process_frame(self):
        """处理完整帧"""
        data = bytes(self.buf)
        frame_data_len = FIXED_LEN + self.payload_len

        # CRC 校验
        received_crc = struct.unpack('>H', data[frame_data_len:frame_data_len+2])[0]
        computed_crc = crc16_ccitt(data[:frame_data_len])

        if received_crc != computed_crc:
            self.crc_errors += 1
            self.resync_count += 1
            return

        # 版本检查
        if data[2] != VERSION:
            self.parser_errors += 1
            return

        self.valid_frames += 1

        # 解析字段
        event = data[3]
        risk = data[4]
        confidence = data[5]
        duration = struct.unpack('>H', data[6:8])[0]
        timestamp = struct.unpack('>I', data[8:12])[0]
        payload = data[FIXED_LEN:frame_data_len] if self.payload_len > 0 else b''

        # 打印
        now = datetime.now().strftime('%H:%M:%S.%f')[:-3]
        event_name = EVENT_NAMES.get(event, f'UNKNOWN(0x{event:02X})')
        risk_name = RISK_NAMES.get(risk, f'UNKNOWN({risk})')

        print(f"\n[{now}]")
        print(f"  {event_name}")
        print(f"  risk={risk_name} confidence={confidence}")
        if duration > 0:
            print(f"  duration={duration}ms")
        if timestamp > 0:
            print(f"  timestamp={timestamp}")

        if event == 0x01 and len(payload) >= 4:
            # 心跳 payload
            print(f"  dms_alive={payload[0]} camera_alive={payload[1]}"
                  f" ai_alive={payload[2]} hb_risk={RISK_NAMES.get(payload[3], '?')}")
        elif payload:
            hex_str = ' '.join(f'{b:02X}' for b in payload)
            print(f"  payload({len(payload)}B): {hex_str}")

    def print_stats(self):
        """打印统计信息"""
        print(f"\n{'='*50}")
        print(f"Protocol Monitor Statistics:")
        print(f"  total_bytes    = {self.total_bytes}")
        print(f"  valid_frames   = {self.valid_frames}")
        print(f"  crc_errors     = {self.crc_errors}")
        print(f"  resync_count   = {self.resync_count}")
        print(f"  parser_errors  = {self.parser_errors}")
        print(f"{'='*50}")


def main():
    parser = argparse.ArgumentParser(description='DMS Protocol Monitor')
    parser.add_argument('--port', type=str, help='Serial port')
    parser.add_argument('--baud', type=int, default=115200)
    parser.add_argument('--file', type=str, help='Read from binary file')
    parser.add_argument('--stdin', action='store_true',
                        help='Read from stdin')
    args = parser.parse_args()

    monitor = ProtocolMonitor()

    if args.file:
        # 从文件读取
        print(f"[MON] Reading from file: {args.file}")
        with open(args.file, 'rb') as f:
            data = f.read()
        for byte in data:
            monitor.feed_byte(byte)
        monitor.print_stats()

    elif args.stdin:
        # 从 stdin 读取
        print("[MON] Reading from stdin...", file=sys.stderr)
        try:
            while True:
                byte = sys.stdin.buffer.read(1)
                if not byte:
                    break
                monitor.feed_byte(byte[0])
        except KeyboardInterrupt:
            pass
        monitor.print_stats()

    elif args.port:
        # 从串口读取
        try:
            import serial
        except ImportError:
            print("[MON] pyserial not installed. Install: pip install pyserial")
            sys.exit(1)

        print(f"[MON] Opening {args.port} @ {args.baud}")
        ser = serial.Serial(args.port, args.baud, timeout=0.1)
        print("[MON] Monitoring... (Ctrl+C to stop)")

        try:
            while True:
                data = ser.read(256)
                if data:
                    for byte in data:
                        monitor.feed_byte(byte)
        except KeyboardInterrupt:
            pass
        finally:
            ser.close()
            monitor.print_stats()
    else:
        print("[MON] Specify --port, --file, or --stdin")
        sys.exit(1)


if __name__ == '__main__':
    main()
