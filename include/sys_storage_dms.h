#ifndef SYS_STORAGE_DMS_H
#define SYS_STORAGE_DMS_H

#include "common.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * DMS 模式专用存储层。
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
void dms_storage_stop_session(void);
bool dms_storage_push_frame(const frame_data_t *frame);
void dms_storage_deinit(void);

#ifdef __cplusplus
}
#endif

#endif
