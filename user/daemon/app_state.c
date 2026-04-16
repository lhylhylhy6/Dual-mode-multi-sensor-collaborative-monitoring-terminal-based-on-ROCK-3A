#include "app_state.h"

#include <stdio.h>
#include <string.h>

int app_state_init(app_state_t *st)
{
    if (!st) {
        return -1;
    }

    memset(st, 0, sizeof(*st));

    if (pthread_mutex_init(&st->lock, NULL) != 0) {
        return -1;
    }

    st->mode = APP_MODE_TRIGGER;
    return 0;
}

void app_state_deinit(app_state_t *st)
{
    if (!st) {
        return;
    }

    pthread_mutex_destroy(&st->lock);
}

void app_state_set_mode(app_state_t *st, app_mode_t mode)
{
    pthread_mutex_lock(&st->lock);
    st->mode = mode;
    pthread_mutex_unlock(&st->lock);
}

app_mode_t app_state_get_mode(app_state_t *st)
{
    app_mode_t mode;

    pthread_mutex_lock(&st->lock);
    mode = st->mode;
    pthread_mutex_unlock(&st->lock);

    return mode;
}

void app_state_update_snapshot(app_state_t *st, const struct sh_snapshot *snap)
{
    pthread_mutex_lock(&st->lock);
    st->last_snapshot = *snap;
    pthread_mutex_unlock(&st->lock);
}

void app_state_update_capture(app_state_t *st,
                              const char *image_path,
                              const char *event_time,
                              int ok)
{
    pthread_mutex_lock(&st->lock);

    snprintf(st->last_image_path, sizeof(st->last_image_path), "%s",
             image_path ? image_path : "");
    snprintf(st->last_event_time, sizeof(st->last_event_time), "%s",
             event_time ? event_time : "");
    st->last_capture_ok = ok;

    pthread_mutex_unlock(&st->lock);
}

void app_state_copy(app_state_t *st, app_state_t *out_copy)
{
    pthread_mutex_lock(&st->lock);
    *out_copy = *st;
    pthread_mutex_unlock(&st->lock);
}

const char *app_mode_to_str(app_mode_t mode)
{
    return mode == APP_MODE_MONITOR ? "monitor" : "trigger";
}

int app_mode_from_str(const char *s, app_mode_t *mode)
{
    if (!s || !mode) {
        return -1;
    }

    if (strcmp(s, "monitor") == 0) {
        *mode = APP_MODE_MONITOR;
        return 0;
    }

    if (strcmp(s, "trigger") == 0) {
        *mode = APP_MODE_TRIGGER;
        return 0;
    }

    return -1;
}