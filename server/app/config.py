"""服务器端启发式阈值集中配置。

注意：本文件所有数值均为 **仅工程初始值，需实车标定**，
在实车数据采集与标定完成前不得作为正式运营依据。
"""
from dataclasses import dataclass


@dataclass(frozen=True)
class DashboardConfig:
    """仪表盘聚合规则（仅工程初始值，需实车标定）。"""
    # 车辆当前状态分桶：车辆最新一条 DMS 事件等级为 ATTENTION 时归入 WARNING 桶
    attention_counts_as_warning: bool = True


@dataclass(frozen=True)
class RouteRiskConfig:
    """线路风险启发式规则（仅工程初始值，需实车标定）。"""
    high_events_high: int = 3        # 线路 HIGH 等级 DMS 事件总数 >= N → 线路风险 HIGH
    high_events_attention: int = 1   # 出现 >= N 个 HIGH 事件 → 至少 ATTENTION
    motion_events_attention: int = 10  # 运动事件总数 >= N → 至少 ATTENTION


@dataclass(frozen=True)
class VehicleHealthConfig:
    """车辆运行状态启发式规则（仅工程初始值，需实车标定）。"""
    today_motion_high: int = 5       # 当日运动事件 >= N → 车辆状态 HIGH
    today_motion_attention: int = 1  # 当日出现 >= N 起运动事件 → ATTENTION


@dataclass(frozen=True)
class CareWindowConfig:
    """司机关怀观察窗（仅工程初始值，需实车标定）。"""
    recent_window_hours: int = 24    # “近期疲劳事件”统计窗口（小时）


DASHBOARD = DashboardConfig()
ROUTE_RISK = RouteRiskConfig()
VEHICLE_HEALTH = VehicleHealthConfig()
CARE_WINDOW = CareWindowConfig()
