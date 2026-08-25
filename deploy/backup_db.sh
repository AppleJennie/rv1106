#!/bin/bash
# ============================================================
# Bus DMS Server —— SQLite 备份脚本
# 用法：crontab -e 加入（每小时一次，示例）：
#   5 * * * * /opt/dms-bus/deploy/backup_db.sh >> /opt/dms-bus/server/logs/backup.log 2>&1
# 说明：优先用 sqlite3 .backup（WAL 安全），无 sqlite3 CLI 时退化为 cp。
# ============================================================
set -euo pipefail

# 从同目录 .env 读取配置（缺省值兜底）
ENV_FILE="$(dirname "$0")/.env"
DB_PATH="./data/dms_bus.db"
BACKUP_DIR="./backup"
KEEP_DAYS=14
if [ -f "$ENV_FILE" ]; then
    # shellcheck disable=SC1090
    . "$ENV_FILE"
    DB_PATH="${DMS_BUS_DB_PATH:-$DB_PATH}"
    BACKUP_DIR="${DMS_BUS_BACKUP_DIR:-$BACKUP_DIR}"
    KEEP_DAYS="${DMS_BUS_BACKUP_KEEP_DAYS:-$KEEP_DAYS}"
fi

if [ ! -f "$DB_PATH" ]; then
    echo "[backup_db] 数据库不存在：$DB_PATH，跳过" >&2
    exit 1
fi

mkdir -p "$BACKUP_DIR"
TS="$(date +%Y%m%d_%H%M%S)"
DST="$BACKUP_DIR/dms_bus_$TS.db"

if command -v sqlite3 >/dev/null 2>&1; then
    sqlite3 "$DB_PATH" ".backup '$DST'"
else
    cp "$DB_PATH" "$DST"
fi
echo "[backup_db] $(date '+%F %T') 已备份到 $DST"

# 清理超龄备份
find "$BACKUP_DIR" -name 'dms_bus_*.db' -type f -mtime "+$KEEP_DAYS" -delete
echo "[backup_db] 已清理 ${KEEP_DAYS} 天前的备份"
