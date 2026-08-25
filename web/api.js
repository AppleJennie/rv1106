/* 公交驾驶员安全与乘客舒适度智能系统 —— Web 管理平台原型共用 JS
 * 零构建、零外部依赖（Vanilla JS）。本文件提供：
 *   - API_BASE 与 apiGet()：统一访问后端 REST API
 *   - renderNav()：各页共用的顶部导航条
 *   - 常用格式化与中文字典
 */

/* API_BASE 默认 '' 表示“同源”：页面由 FastAPI 挂载在 /web/ 下，
 * API 在同一服务的 /api/v1 下，无需跨域。
 * 若日后把 web/ 静态文件单独部署到别的地址/端口，把下面常量改为后端地址，例如：
 *   const API_BASE = 'http://127.0.0.1:8099';
 * （跨域部署时后端需另行开启 CORS；本原型默认同源，不涉及。）
 */
const API_BASE = '';

/* GET 请求封装：params 为对象（空值自动忽略）；非 2xx 抛 Error（含状态码与后端 detail）。 */
async function apiGet(path, params) {
  let url = API_BASE + path;
  if (params) {
    const qs = new URLSearchParams();
    for (const [k, v] of Object.entries(params)) {
      if (v !== undefined && v !== null && v !== '') qs.append(k, v);
    }
    const s = qs.toString();
    if (s) url += (url.includes('?') ? '&' : '?') + s;
  }
  const resp = await fetch(url);
  if (!resp.ok) {
    let detail = '';
    try { detail = JSON.stringify(await resp.json()); } catch (e) { /* 忽略非 JSON 响应体 */ }
    throw new Error('HTTP ' + resp.status + ' ' + detail);
  }
  return resp.json();
}

/* 顶部导航条：active 传当前页 key，用于高亮。 */
function renderNav(active) {
  const items = [
    ['index', 'index.html', '安全总览'],
    ['driver_care', 'driver_care.html', '驾驶员关怀'],
    ['comfort', 'comfort.html', '乘客舒适度'],
    ['route_risk', 'route_risk.html', '线路风险'],
    ['timeline', 'timeline.html', '事件时间线'],
  ];
  const links = items.map(([key, href, text]) =>
    '<a href="' + href + '"' + (key === active ? ' class="active"' : '') + '>' + text + '</a>'
  ).join('');
  document.getElementById('nav').innerHTML =
    '<span class="brand">公交安全与舒适度平台</span>' + links;
}

/* ---------------- 格式化与中文字典 ---------------- */

function fmtTs(ts) {
  if (!ts) return '—';
  return String(ts).replace('T', ' ').replace(/Z$/, '');
}

function fmtScore(v) {
  return (v === null || v === undefined) ? '—' : Number(v).toFixed(1);
}

function fmtIds(ev) {
  const parts = [];
  if (ev.driver_id != null) parts.push('司机#' + ev.driver_id);
  if (ev.vehicle_id != null) parts.push('车辆#' + ev.vehicle_id);
  if (ev.route_id != null) parts.push('线路#' + ev.route_id);
  if (ev.shift_id != null) parts.push('班次#' + ev.shift_id);
  return parts.length ? parts.join(' ') : '未关联对象';
}

function escapeHtml(s) {
  return String(s == null ? '' : s)
    .replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;')
    .replace(/"/g, '&quot;').replace(/'/g, '&#39;');
}

const LEVEL_CN = {
  NORMAL: '正常', ATTENTION: '注意', WARNING: '预警', HIGH: '高风险',
};

const DMS_TYPE_CN = {
  EYE_CLOSED: '闭眼', LONG_EYE_CLOSED: '长时间闭眼', YAWN: '打哈欠',
  HEAD_DOWN: '低头', FACE_LOST: '人脸丢失',
};

const MOTION_TYPE_CN = {
  HARD_ACCEL: '急加速', HARD_BRAKE: '急刹车',
  HARD_TURN_LEFT: '急转弯(左)', HARD_TURN_RIGHT: '急转弯(右)',
  BUMP: '颠簸', HIGH_LONG_JERK: '纵向冲击偏高', HIGH_LAT_JERK: '横向冲击偏高',
};

/* 责任归因字典。红线：司机相关推断仅为 SUSPECTED（疑似），需人工复核。 */
const ATTR_CN = {
  UNKNOWN: '未知（无信息）',
  PEDESTRIAN_AVOIDANCE: '避让行人',
  TRAFFIC: '交通状况',
  DRIVER_ATTENTION: '疑似驾驶员注意力（SUSPECTED，需人工复核）',
  ROAD_CONDITION: '路况因素',
  VEHICLE: '车辆因素',
};

const CARE_CN = {
  NORMAL: '正常关注',
  REST_RECOMMENDED: '建议休息',
  SCHEDULE_REVIEW: '建议排班复核',
};

const RECO_CN = {
  NORMAL: '排班正常',
  REST_RECOMMENDED: '建议休息',
  SCHEDULE_REVIEW: '建议复核排班',
};

/* 事件类别中文名 */
const CAT_CN = { dms: 'DMS 疲劳', motion: '车辆运动', bus: '融合事件' };

/* 时段分桶（与 server/app/driver_care.py 的 PERIOD_BUCKETS 一致） */
function periodOfHour(h) {
  if (h < 6) return '凌晨';
  if (h < 12) return '上午';
  if (h < 18) return '下午';
  return '晚间';
}

/* 通用 CSS 柱状图渲染：data = [{label, value, high}] */
function renderBars(el, data) {
  const max = Math.max(1, ...data.map(d => d.value));
  el.innerHTML = data.map(d => {
    const h = Math.round((d.value / max) * 100);
    return '<div class="bar-wrap">' +
      '<div class="bar' + (d.high ? ' high' : '') + '" style="height:' + h + '%">' +
      '<span class="v">' + (d.value || '') + '</span></div>' +
      '<div class="x">' + escapeHtml(d.label) + '</div></div>';
  }).join('');
}

/* 在指定元素内渲染错误信息 */
function showError(el, err) {
  el.innerHTML = '<div class="error-box">请求失败：' + escapeHtml(err.message || err) + '</div>';
}
