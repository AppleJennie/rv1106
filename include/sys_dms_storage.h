#ifndef SYS_DMS_STORAGE_H
#define SYS_DMS_STORAGE_H

#include "common.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * DMS 疲劳驾驶视觉采集第一版存储模块。
 *
 * 目录结构：
 *   /mnt/sdcard/dms/sessions/YYYYMMDD_HHMMSS/
 *     ├── frames/
 *     │   ├── 000001.jpg
 *     │   ├── 000002.jpg
 *     │   └── ...
 *     ├── frame_timestamps.csv
 *     └── session_info.json
 */

bool dms_storage_init(void);
bool dms_storage_start_session(void);
bool dms_storage_push_frame(const frame_data_t *frame);
bool dms_storage_save_alarm_frame(const frame_data_t *frame, const char *reason);
void dms_storage_stop_session(void);
void dms_storage_deinit(void);

#ifdef __cplusplus
}
#endif

#endif
