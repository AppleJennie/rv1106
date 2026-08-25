/*
 * test_vehicle_can.c - vehicle_can 模块单元测试
 *
 * 覆盖：
 *   C1 无 CAN 时读取不阻塞，flags 全 0
 *   C2 注入后读取值与有效标志正确
 *   C3 各路信号独立过期，过期字段清零
 *   C4 刷新维持有效
 */
#include "vehicle_can.h"
#include "test_common.h"

/* C1 无 CAN 初始状态 */
static void test_no_can_initial(void)
{
    vehicle_can_init(NULL);
    vehicle_can_state_t st;
    CHECK_TRUE(!vehicle_can_get_state(&st, 0), "C1a 上电无 CAN 时无有效信号");
    CHECK_TRUE(st.valid_flags == 0, "C1b valid_flags 全 0");
    CHECK_TRUE(st.door_state == VEHICLE_DOOR_UNKNOWN, "C1c 车门状态 UNKNOWN");
}

/* C2 注入后读取 */
static void test_update_and_read(void)
{
    vehicle_can_init(NULL);
    vehicle_can_update_speed(50.0f, 100);
    vehicle_can_update_brake_pedal(true, 100);
    vehicle_can_update_accelerator_pedal(30.0f, 100);
    vehicle_can_update_door(VEHICLE_DOOR_OPEN, 100);
    vehicle_can_update_soc(76.5f, 100);

    vehicle_can_state_t st;
    CHECK_TRUE(vehicle_can_get_state(&st, 100), "C2a 注入后读取有效");
    CHECK_TRUE((st.valid_flags & VEHICLE_CAN_VALID_SPEED) != 0, "C2b 车速有效");
    CHECK_NEAR(st.speed_kph, 50.0f, 0.001f, "C2c 车速值正确");
    CHECK_TRUE(st.brake_pedal, "C2d 刹车踏板状态正确");
    CHECK_NEAR(st.accelerator_pedal, 30.0f, 0.001f, "C2e 油门开度正确");
    CHECK_TRUE(st.door_state == VEHICLE_DOOR_OPEN, "C2f 车门状态正确");
    CHECK_NEAR(st.soc, 76.5f, 0.001f, "C2g SOC 正确");
    CHECK_TRUE((st.valid_flags & VEHICLE_CAN_VALID_DOOR) != 0, "C2h 车门标志位置位");
}

/* C3 信号独立过期 */
static void test_staleness(void)
{
    vehicle_can_init(NULL);
    vehicle_can_update_speed(50.0f, 100);
    vehicle_can_update_brake_pedal(true, 100);
    vehicle_can_update_door(VEHICLE_DOOR_CLOSED, 100);
    vehicle_can_update_soc(60.0f, 100);

    /* 600ms 后：车速/踏板(500ms)过期，车门(2000ms)/SOC(5000ms)仍有效 */
    vehicle_can_state_t st;
    CHECK_TRUE(vehicle_can_get_state(&st, 700), "C3a 部分信号仍有效");
    CHECK_TRUE((st.valid_flags & VEHICLE_CAN_VALID_SPEED) == 0, "C3b 车速过期");
    CHECK_TRUE((st.valid_flags & VEHICLE_CAN_VALID_BRAKE) == 0, "C3c 踏板过期");
    CHECK_TRUE((st.valid_flags & VEHICLE_CAN_VALID_DOOR) != 0,  "C3d 车门仍有效");
    CHECK_TRUE((st.valid_flags & VEHICLE_CAN_VALID_SOC) != 0,   "C3e SOC 仍有效");
    /* 过期字段清零，防止上层误用旧值 */
    CHECK_NEAR(st.speed_kph, 0.0f, 0.001f, "C3f 过期车速清零");

    /* 全部过期后不阻塞：返回 false 但结构体仍可用 */
    CHECK_TRUE(!vehicle_can_get_state(&st, 10000), "C3g 全部过期返回 false");
    CHECK_TRUE(st.valid_flags == 0, "C3h flags 全 0");
    CHECK_TRUE(st.door_state == VEHICLE_DOOR_UNKNOWN, "C3i 过期车门置 UNKNOWN");
}

/* C4 刷新维持有效 */
static void test_refresh(void)
{
    vehicle_can_init(NULL);
    vehicle_can_update_speed(50.0f, 100);
    vehicle_can_update_speed(40.0f, 550);   /* 最新注入时刻 550，过期点 1050 */

    vehicle_can_state_t st;
    CHECK_TRUE(vehicle_can_get_state(&st, 1000), "C4a 刷新后保持有效");
    CHECK_NEAR(st.speed_kph, 40.0f, 0.001f, "C4b 读取到最新值");
    CHECK_TRUE(!vehicle_can_get_state(&st, 1100), "C4c 超过过期时间后失效");
}

int main(void)
{
    printf("==== test_vehicle_can ====\n");
    test_no_can_initial();
    test_update_and_read();
    test_staleness();
    test_refresh();
    TEST_SUMMARY("vehicle_can");
}
