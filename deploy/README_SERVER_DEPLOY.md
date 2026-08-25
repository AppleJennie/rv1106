# Bus DMS Server 部署说明（部署包，只准备、不部署）

> 版本：v1.0（2026-08-25）
> 本目录是**部署包**：所有文件仅做准备，**本阶段严禁在服务器上启动任何 Bus DMS 长期服务**。

---

## ⚠️ 显著警示（先读这个，再谈部署）

1. **目标服务器（101.42.200.249）上已运行 `xiaozhi-esp32-server` 容器**，占用 **8000 / 8003 端口、约 745MB 内存**。
   - **严禁**停止、重启、覆盖、升级该容器或其 Docker 环境；
   - **严禁**占用 8000 / 8003 端口；
   - **严禁**改动该机 Docker 的网络/iptables 配置。
2. **本阶段严禁启动 Bus DMS 长期服务**：不 `systemctl start`、不 `docker run`、不 `nohup` 挂后台；也**不要**在本阶段 SSH 到 101.42.200.249 执行任何部署动作。
3. `.env.example` 中 `DMS_BUS_PORT=8099` **仅为开发占位**；正式端口必须在腾讯云控制台安全组放行后确定，并先在服务器上 `ss -tlnp` 确认无冲突。
4. 服务器内存有限（xiaozhi 已占 ~745MB），Bus DMS 内存目标 **<300MB**，由 systemd `MemoryMax=300M` 强制兜底。

---

## 1. 部署形态选择：systemd（不用 Docker），理由

| 维度 | systemd + venv（**选定**） | Docker |
|------|---------------------------|--------|
| 资源开销 | 裸进程，无额外运行时；`MemoryMax` 直接限制 300MB | 容器+overlay 额外开销；宿主机已被 xiaozhi 占 ~745MB |
| 对既有服务的风险 | 完全不触碰 Docker 网络/iptables | 新增容器需动 Docker 网络，存在影响 xiaozhi 的风险 |
| 依赖复杂度 | FastAPI+SQLite 单进程单写者，1 worker，无需容器编排 | 优势（镜像分发）在本场景用不上 |
| 运维 | `systemctl status/restart` + journalctl，够用 | 多一层概念 |

结论：单机、单进程、SQLite 的轻量服务，用 systemd 管理最简单且对既有 xiaozhi 容器零干扰。

## 2. 部署包文件清单

| 文件 | 用途 |
|------|------|
| `requirements.txt` | Python 依赖（生产只需 fastapi + uvicorn；pytest/httpx 仅测试） |
| `.env.example` | 配置模板（端口/数据库路径/日志级别/备份参数） |
| `dms-bus.service` | systemd 单元模板（1 worker、MemoryMax=300M、Restart=on-failure） |
| `backup_db.sh` | SQLite 定时备份脚本（优先 `.backup`，退化 `cp`，自动清理超龄备份） |
| `logrotate.dms-bus` | 日志轮转配置（daily、保留 14 份、50M 上限、copytruncate） |

## 3. 部署步骤（将来执行，供检查单）

```bash
# 0. 前置确认（缺一不可）
ss -tlnp | grep -E '8000|8003'          # 确认 xiaozhi 在跑，且新端口不冲突
free -m                                  # 确认剩余内存 > 400MB

# 1. 低权限用户与目录
sudo useradd -r -s /usr/sbin/nologin dmsbus
sudo mkdir -p /opt/dms-bus
sudo chown -R dmsbus:dmsbus /opt/dms-bus

# 2. 代码与依赖（server/ 由 TASK D/E 提供）
sudo -u dmsbus python3 -m venv /opt/dms-bus/venv
sudo -u dmsbus /opt/dms-bus/venv/bin/pip install fastapi "uvicorn[standard]"

# 3. 配置
sudo cp deploy/.env.example /opt/dms-bus/deploy/.env
#   编辑 .env：正式端口（腾讯云放行后定）、DMS_BUS_HOST=0.0.0.0

# 4. 数据库初始化（见第 4 节）

# 5. systemd 安装（确认无误后才 enable）
sudo cp deploy/dms-bus.service /etc/systemd/system/dms-bus.service
sudo systemctl daemon-reload
sudo systemctl enable --now dms-bus      # ⚠️ 只有获得明确上线指令后才执行

# 6. 备份与日志
#   crontab -e 加入： 5 * * * * /opt/dms-bus/deploy/backup_db.sh >> /opt/dms-bus/server/logs/backup.log 2>&1
sudo cp deploy/logrotate.dms-bus /etc/logrotate.d/dms-bus
```

## 4. 数据库初始化

SQLite 单文件，无需独立服务：

```bash
sudo -u dmsbus mkdir -p /opt/dms-bus/server/data /opt/dms-bus/server/backup /opt/dms-bus/server/logs
```

建表以 server/ 模块（TASK D/E）实际提供的入口为准，二选一：

```bash
# 方式 A：应用启动时自动建表（假设默认行为，待 server/ 实现确认）
#   首次启动 uvicorn 即完成，无额外步骤

# 方式 B：显式初始化（若 server/ 提供初始化命令/脚本）
sudo -u dmsbus /opt/dms-bus/venv/bin/python -m app.db_init
```

初始化后用 `sqlite3 /opt/dms-bus/server/data/dms_bus.db ".tables"` 核对表结构（10 张表，以 TASK D/E 交付为准）。

## 5. 备份策略

- `backup_db.sh`：优先 `sqlite3 .backup`（WAL 安全，可在服务运行中执行）；无 sqlite3 CLI 时退化为 `cp`（此时建议挑低峰执行）。
- 产物：`$DMS_BUS_BACKUP_DIR/dms_bus_YYYYMMDD_HHMMSS.db`，默认保留 14 天（`DMS_BUS_BACKUP_KEEP_DAYS`）。
- 建议 crontab 每小时一次；异地容灾（rsync 到对象存储）留待正式上线后补。

## 6. 日志与轮转

- 应用日志目录 `/opt/dms-bus/server/logs/`（uvicorn access/error 由 systemd journal 与日志文件双写，按 server/ 实现的 logging 配置为准）。
- `logrotate.dms-bus`：daily、rotate 14、compress、maxsize 50M、copytruncate（进程不重启即可截断）。

## 7. 上线检查单（执行人逐项打勾）

- [ ] 腾讯云安全组已放行正式端口，且 `.env` 已改为该端口
- [ ] `ss -tlnp` 确认与 xiaozhi（8000/8003）无冲突
- [ ] xiaozhi-esp32-server 运行状态未受任何操作影响
- [ ] `curl http://127.0.0.1:<port>/health` 返回 `status: ok`
- [ ] `systemctl status dms-bus` 无频繁 Restart；`journalctl -u dms-bus` 无 ERROR
- [ ] 备份脚本手动跑一次成功，`backup/` 有产物
- [ ] 内存实测 <300MB（`systemd-cgtop` 或 `ps`）

---

> 再次提醒：本目录所有文件**只准备**，任何启动动作等待明确上线指令。
