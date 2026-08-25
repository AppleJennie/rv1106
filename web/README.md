# web/ - 公交安全与舒适度 Web 管理平台原型

> 状态：原型（TASK F）。HTML + CSS + Vanilla JS，**零构建、零外部 CDN**。
> 红线：本平台是安全提示与关怀系统，不作自动归责；单次事件不归责司机；
> 无信息时责任归因 UNKNOWN；司机相关推断一律 SUSPECTED（疑似）措辞并保留人工复核。

## 访问方式

静态页由 FastAPI 挂载在 `/web`（见 `server/app/main.py` 的 `create_app()`，
仅当 `项目根/web` 目录存在时挂载，`html=True`）：

```bash
cd server
.venv/bin/python -m uvicorn app.main:app --host 127.0.0.1 --port 8099
# 浏览器打开 http://127.0.0.1:8099/web/
```

开发端口固定 8099（8000/8003 被 xiaozhi 占用，禁用）。未部署，不要在服务器上起长期服务。

## 页面清单

| 页面 | 路径 | 内容 | 主要数据源 |
|------|------|------|-----------|
| 公交安全总览 | `/web/` 或 `/web/index.html` | 车辆按 NORMAL/WARNING/HIGH 计数卡片、今日疲劳高风险事件数、今日车辆运动事件数、平均舒适度；30s 自动刷新 | `GET /api/v1/dashboard/overview` |
| 驾驶员关怀（Driver Care） | `/web/driver_care.html` | 司机选择、关怀建议（NORMAL/REST_RECOMMENDED/SCHEDULE_REVIEW）、近 7 天疲劳风险趋势、高风险时段分布、班次风险评估 | `GET /api/v1/drivers/{id}/safety`、`GET /api/v1/shifts/{id}/risk`、`GET /api/v1/events?category=dms` |
| 乘客舒适度 | `/web/comfort.html` | Comfort Index、急刹/急加速/急转弯/颠簸计数、jerk（冲击）指标；按线路/车辆/班次/日期过滤 | `GET /api/v1/events?category=motion`、`/api/v1/dashboard/overview`、`/api/v1/routes/{id}/risk`、`/api/v1/vehicles/{id}/health` |
| 线路风险 | `/web/route_risk.html` | 线路风险等级、路段热区示意（CSS 色块，**mock 数据**）、急刹高发时段、疲劳高发时间 | `GET /api/v1/routes/{id}/risk`、`GET /api/v1/events` |
| 事件时间线 | `/web/timeline.html` | DMS/运动/融合三类事件按时间倒序，融合事件紫色高亮；顶部固定提示“AI 风险提示，不代表最终责任认定，需人工复核” | `GET /api/v1/events` |

共用文件：`common.css`（样式）、`api.js`（`API_BASE` 常量、`apiGet()`、导航条、中文字典）。

## API_BASE 说明

`api.js` 中 `const API_BASE = '';` 默认**同源**（页面与 API 同在 8099 服务下）。
若 web/ 单独部署到其他地址/端口，改成后端地址，如 `http://127.0.0.1:8099`
（跨域需后端另行开启 CORS；本原型默认同源，不涉及）。

## 假设与偏差（对接时请以下述为准）

1. **无司机/线路列表端点**：当前后端只有 `POST /api/v1/drivers`、`POST /api/v1/routes`
   注册接口，`GET` 列表返回 405。driver_care/route_risk 页会**先尝试 GET 列表**
   （后端将来补上后自动生效），失败则降级为**手动输入 ID**。
2. **/api/v1/events 无 vehicle/route/shift 过滤参数**（仅 event_type/category/start/end/
   分页）。comfort、route_risk、driver_care 页采用 `page_size=200` 拉取后**前端按 ID 过滤**，
   事件量大时不准确（最多统计最近 200 条）；建议后端后续补过滤参数。
3. **无 comfort_trips 列表端点**：comfort 页的 Comfort Index 取聚合值——指定线路用
   `routes/{id}/risk.avg_comfort_score`，指定车辆用 `vehicles/{id}/health.avg_comfort_score_today`，
   都未指定时用 `dashboard/overview.avg_comfort_score`（今日均值）；jerk 指标以后端事件流中的
   `HIGH_LONG_JERK`/`HIGH_LAT_JERK` **事件计数**代替数值列表。
4. **route_risk 页路段为前端 mock**：无真实 GPS/路段数据，8 个路段按线路 ID 与真实统计量
   确定性伪随机生成色块，页面已显著标注“模拟数据，仅示意”。接入真实定位后替换。
5. **趋势图分桶在前端完成**：driver_care 的近 7 天趋势、高风险时段（凌晨/上午/下午/晚间，
   与 `server/app/driver_care.py` 的 PERIOD_BUCKETS 一致）由前端对事件时间戳分桶统计。
6. 页面 ID 过滤依赖事件上携带的 driver_id/vehicle_id/route_id/shift_id；Demo 阶段这些字段
   允许为空，未关联对象的事件不会出现在按 ID 过滤的结果中。

## 文案红线自检

全部页面/注释不含 penalty/fine/punish/deduct/罚款/扣分/处罚 类措辞
（`api.js` 中 `undefined` 命中的 `fine` 子串为 JS 关键字，非业务措辞）。
时间线页固定展示“AI 风险提示，不代表最终责任认定，需人工复核”；
driver_care 页明示司机相关推断仅为 SUSPECTED、需人工复核。
