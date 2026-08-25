#!/usr/bin/env bash
# 一键运行服务器后台测试（使用 server/.venv，ASGI transport，不起真实端口）
set -euo pipefail
cd "$(dirname "$0")"
exec .venv/bin/python -m pytest tests -q "$@"
