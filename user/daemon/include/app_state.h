#ifndef APP_STATE_H
#define APP_STATE_H

#include <pthread.h>
#include "sensor_hub_ioctl.h"

typedef enum {
    APP_MODE_MONITOR = 0,
    APP_MODE_TRIGGER = 1,
} app_mode_t;

typedef struct {
    pthread_mutex_t lock;

    app_mode_t mode;

    struct sh_snapshot last_snapshot;
    char last_image_path[256];
    char last_event_time[64];
    int last_capture_ok;
} app_state_t;

int app_state_init(app_state_t *st);
void app_state_deinit(app_state_t *st);

void app_state_set_mode(app_state_t *st, app_mode_t mode);
app_mode_t app_state_get_mode(app_state_t *st);

void app_state_update_snapshot(app_state_t *st, const struct sh_snapshot *snap);
void app_state_update_capture(app_state_t *st,
                              const char *image_path,
                              const char *event_time,
                              int ok);

void app_state_copy(app_state_t *st, app_state_t *out_copy);

const char *app_mode_to_str(app_mode_t mode);
int app_mode_from_str(const char *s, app_mode_t *mode);

#endif