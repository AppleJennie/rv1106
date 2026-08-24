#ifndef STATE_MACHINE_H
#define STATE_MACHINE_H

#include "common.h"

#ifdef __cplusplus
extern "C" {
#endif

bool state_machine_init(void);
void state_machine_run(void);
void state_machine_deinit(void);

bool state_machine_set_state(system_state_e new_state);
system_state_e state_machine_get_state(void);

#ifdef __cplusplus
}
#endif

#endif