#include "sensor_client.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

int sensor_client_open(sensor_client_t *cli, const char *dev_path)
{
    if (!cli || !dev_path) {
        errno = EINVAL;
        return -1;
    }

    memset(cli, 0, sizeof(*cli));
    cli->fd = -1;

    snprintf(cli->dev_path, sizeof(cli->dev_path), "%s", dev_path);

    cli->fd = open(dev_path, O_RDWR | O_NONBLOCK);
    if (cli->fd < 0) {
        perror("open sensor_hub");
        return -1;
    }

    return 0;
}

void sensor_client_close(sensor_client_t *cli)
{
    if (!cli) {
        return;
    }

    if (cli->fd >= 0) {
        close(cli->fd);
        cli->fd = -1;
    }
}

int sensor_client_get_info(sensor_client_t *cli, struct sh_hub_info *info)
{
    if (!cli || cli->fd < 0 || !info) {
        errno = EINVAL;
        return -1;
    }

    return ioctl(cli->fd, SH_IOC_GET_INFO, info);
}

int sensor_client_get_snapshot(sensor_client_t *cli, struct sh_snapshot *snap)
{
    if (!cli || cli->fd < 0 || !snap) {
        errno = EINVAL;
        return -1;
    }

    return ioctl(cli->fd, SH_IOC_GET_SNAPSHOT, snap);
}

int sensor_client_clear_events(sensor_client_t *cli)
{
    if (!cli || cli->fd < 0) {
        errno = EINVAL;
        return -1;
    }

    return ioctl(cli->fd, SH_IOC_CLR_EVENTS);
}

int sensor_client_force_refresh(sensor_client_t *cli, __u32 sensor_id, __u32 timeout_ms)
{
    struct sh_refresh_req req;

    if (!cli || cli->fd < 0) {
        errno = EINVAL;
        return -1;
    }

    memset(&req, 0, sizeof(req));
    req.id = sensor_id;
    req.timeout_ms = timeout_ms;

    return ioctl(cli->fd, SH_IOC_FORCE_REFRESH, &req);
}

int sensor_client_get_sensor_cfg(sensor_client_t *cli, struct sh_sensor_cfg *cfg)
{
    if (!cli || cli->fd < 0 || !cfg) {
        errno = EINVAL;
        return -1;
    }

    return ioctl(cli->fd, SH_IOC_GET_SENSOR_CFG, cfg);
}

int sensor_client_set_sensor_cfg(sensor_client_t *cli, const struct sh_sensor_cfg *cfg)
{
    struct sh_sensor_cfg tmp;

    if (!cli || cli->fd < 0 || !cfg) {
        errno = EINVAL;
        return -1;
    }

    tmp = *cfg;
    return ioctl(cli->fd, SH_IOC_SET_SENSOR_CFG, &tmp);
}

int sensor_client_wait_readable(sensor_client_t *cli, int timeout_ms)
{
    struct pollfd pfd;
    int ret;

    if (!cli || cli->fd < 0) {
        errno = EINVAL;
        return -1;
    }

    memset(&pfd, 0, sizeof(pfd));
    pfd.fd = cli->fd;
    pfd.events = POLLIN;

    ret = poll(&pfd, 1, timeout_ms);
    if (ret < 0) {
        perror("poll sensor_hub");
        return -1;
    }

    return ret;
}

ssize_t sensor_client_read_events(sensor_client_t *cli,
                                  struct sh_event *events,
                                  size_t max_events)
{
    ssize_t nbytes;

    if (!cli || cli->fd < 0 || !events || max_events == 0) {
        errno = EINVAL;
        return -1;
    }

    nbytes = read(cli->fd, events, max_events * sizeof(struct sh_event));
    if (nbytes < 0) {
        if (errno == EAGAIN) {
            return 0;
        }
        perror("read sensor_hub");
        return -1;
    }

    return nbytes / (ssize_t)sizeof(struct sh_event);
}

const struct sh_sensor_value *sensor_client_find_value(const struct sh_snapshot *snap,
                                                       __u32 sensor_id)
{
    __u32 i;

    if (!snap) {
        return NULL;
    }

    for (i = 0; i < snap->count; ++i) {
        if (snap->items[i].id == sensor_id) {
            return &snap->items[i];
        }
    }

    return NULL;
}