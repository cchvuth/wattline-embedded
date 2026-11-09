/* The app's demo scenes (test/dash_shot_test.dart), for the host shots and the Dash's demo mode. */
#pragma once
#include "dash_state.h"

typedef struct {
	const char *name;
	void (*fill)(dash_state_t *s);
} scene_t;
extern const scene_t scenes[];
extern const int scene_count;
