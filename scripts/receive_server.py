#!/usr/bin/env python3
"""
Hand Capture Right - 接收服务器
监听 TCP 0.0.0.0:9000，按协议接收上传的 CSV 和 JPG。

协议：
    FILE <相对路径> <文件大小>\n
    <原始二进制文件内容，长度严格等于文件大小>

合法相对路径必须以 code/ 或 photo/ 开头，禁止 .. 和绝对路径。
收到完整文件后返回 OK\n，出错返回 ERR\n。
"""

import os
import socket
import argparse

DEFAULT_SAVE_ROOT = "/mnt/sdcard/upload/right"
DEFAULT_HOST = "0.0.0.0"
DEFAULT_PORT = 9000


def is_valid_path(rel):
    """校验相对路径安全规则"""
    if not rel:
        return False
    if rel.startswith("/") or ".." in rel or "\\" in rel:
        return False
    parts = rel.split("/")
    if len(parts) < 2:
        return False
    if parts[0] not in ("code", "photo"):
        return False
    for p in parts[1:]:
        if p in ("", ".", ".."):
            return False
    return True


def recv_exact(sock, n):
    """从 socket 读取恰好 n 字节"""
    buf = b""
    while len(buf) < n:
        chunk = sock.recv(min(65536, n - len(buf)))
        if not chunk:
            return None
        buf += chunk
    return buf


def handle_client(conn, addr, save_root):
    print(f"[INFO] connected from {addr}")
    try:
        while True:
            # 读取文本头，直到遇到 \n
            header = b""
            while b"\n" not in header:
                chunk = conn.recv(1)
                if not chunk:
                    return
                header += chunk
                if len(header) > 1024:
                    print(f"[ERROR] header too long from {addr}")
                    return

            parts = header.decode("utf-8", errors="replace").strip().split()
            if len(parts) != 3 or parts[0] != "FILE":
                print(f"[ERROR] bad header: {header!r}")
                conn.sendall(b"ERR\n")
                continue

            rel_path, size_str = parts[1], parts[2]
            try:
                size = int(size_str)
                if size < 0 or size > 200 * 1024 * 1024:
                    raise ValueError("size out of range")
            except ValueError:
                print(f"[ERROR] bad size: {size_str}")
                conn.sendall(b"ERR\n")
                continue

            if not is_valid_path(rel_path):
                print(f"[ERROR] invalid path: {rel_path}")
                conn.sendall(b"ERR\n")
                continue

            full_path = os.path.join(save_root, rel_path)
            os.makedirs(os.path.dirname(full_path), exist_ok=True)

            tmp_path = full_path + ".tmp"
            with open(tmp_path, "wb") as f:
                remaining = size
                while remaining > 0:
                    chunk = conn.recv(min(65536, remaining))
                    if not chunk:
                        print(f"[ERROR] connection lost while receiving {rel_path}")
                        return
                    f.write(chunk)
                    remaining -= len(chunk)

            os.rename(tmp_path, full_path)
            conn.sendall(b"OK\n")
            print(f"[OK] saved {rel_path} ({size} bytes)")

    except Exception as e:
        print(f"[ERROR] client {addr}: {e}")
    finally:
        conn.close()


def main():
    parser = argparse.ArgumentParser(description="Hand Capture Right receive server")
    parser.add_argument("--root", default=DEFAULT_SAVE_ROOT,
                        help=f"save root directory (default: {DEFAULT_SAVE_ROOT})")
    parser.add_argument("--host", default=DEFAULT_HOST,
                        help=f"listen host (default: {DEFAULT_HOST})")
    parser.add_argument("--port", type=int, default=DEFAULT_PORT,
                        help=f"listen port (default: {DEFAULT_PORT})")
    args = parser.parse_args()

    os.makedirs(args.root, exist_ok=True)

    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind((args.host, args.port))
    s.listen(5)
    print(f"[INFO] listening on {args.host}:{args.port}, save root={args.root}")

    try:
        while True:
            conn, addr = s.accept()
            handle_client(conn, addr, args.root)
    except KeyboardInterrupt:
        print("[INFO] shutting down")
    finally:
        s.close()


if __name__ == "__main__":
    main()
