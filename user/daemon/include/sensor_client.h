#ifndef SENSOR_CLIENT_H
#define SENSOR_CLIENT_H

#include <stddef.h>
#include <sys/types.h>
#include "sensor_hub_ioctl.h"

typedef struct {
    int fd;
    char dev_path[128];
} sensor_client_t;

int sensor_client_open(sensor_client_t *cli, const char *dev_path);
void sensor_client_close(sensor_client_t *cli);

int sensor_client_get_info(sensor_client_t *cli, struct sh_hub_info *info);
int sensor_client_get_snapshot(sensor_client_t *cli, struct sh_snapshot *snap);
int sensor_client_clear_events(sensor_client_t *cli);
int sensor_client_force_refresh(sensor_client_t *cli, __u32 sensor_id, __u32 timeout_ms);
int sensor_client_get_sensor_cfg(sensor_client_t *cli, struct sh_sensor_cfg *cfg);
int sensor_client_set_sensor_cfg(sensor_client_t *cli, const struct sh_sensor_cfg *cfg);

int sensor_client_wait_readable(sensor_client_t *cli, int timeout_ms);
ssize_t sensor_client_read_events(sensor_client_t *cli,
                                 struct sh_event *events,
                                 size_t max_events);

const struct sh_sensor_value *sensor_client_find_value(const struct sh_snapshot *snap,
                                                       __u32 sensor_id);

#endif