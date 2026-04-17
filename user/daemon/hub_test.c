#include "sensor_client.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define SENSOR_HUB_DEV "/dev/sensor_hub"

static void dump_snapshot(const struct sh_snapshot *snap)
{
    unsigned int i;

    printf("snapshot: version=0x%x count=%u seq=%llu dropped=%llu\n",
           snap->version,
           snap->count,
           (unsigned long long)snap->seq,
           (unsigned long long)snap->dropped_events);

    for (i = 0; i < snap->count; ++i) {
        const struct sh_sensor_value *v = &snap->items[i];

        printf("  endpoint id=%u type=%u dir=%u caps=0x%x flags=0x%x nvalues=%u ts=%lld",
               v->id, v->type, v->direction, v->caps, v->flags, v->nvalues,
               (long long)v->timestamp_ns);

        for (unsigned int j = 0; j < v->nvalues && j < SH_MAX_VALUES; ++j) {
            printf(" val[%u]=%d", j, v->values[j]);
        }
        printf("\n");
    }
}

static void dump_event(const struct sh_event *evt)
{
    printf("event: seq=%llu type=%u sensor=%u code=%u flags=0x%x nvalues=%u ts=%lld",
           (unsigned long long)evt->seq,
           evt->type,
           evt->sensor_id,
           evt->code,
           evt->flags,
           evt->nvalues,
           (long long)evt->timestamp_ns);

    for (unsigned int i = 0; i < evt->nvalues && i < SH_MAX_VALUES; ++i) {
        printf(" val[%u]=%d", i, evt->values[i]);
    }
    printf("\n");
}

int main(void)
{
    sensor_client_t cli;
    struct sh_hub_info info;
    struct sh_snapshot snap;
    struct sh_action_req action = {0};
    struct sh_event events[8];
    int ret;

    ret = sensor_client_open(&cli, SENSOR_HUB_DEV);
    if (ret < 0) {
        return 1;
    }

    if (sensor_client_get_info(&cli, &info) == 0) {
        printf("hub info: version=0x%x endpoints=%u inputs=%u outputs=%u qsize=%u qdepth=%u event_seq=%llu snapshot_seq=%llu\n",
               info.version,
               info.sensor_count,
               info.input_count,
               info.output_count,
               info.queue_size,
               info.queue_depth,
               (unsigned long long)info.event_seq,
               (unsigned long long)info.snapshot_seq);
    } else {
        perror("GET_INFO");
    }

    if (sensor_client_get_snapshot(&cli, &snap) == 0) {
        dump_snapshot(&snap);
    } else {
        perror("GET_SNAPSHOT");
    }

    printf("forcing SHT20 refresh...\n");
    if (sensor_client_refresh(&cli, SH_SENSOR_SHT20, 0) < 0) {
        perror("FORCE_REFRESH SHT20");
    }

    memset(&action, 0, sizeof(action));
    action.id = SH_SENSOR_BUZZER;
    action.action = SH_ACTION_ALERT;
    if (sensor_client_run_action(&cli, &action) == 0) {
        printf("triggered buzzer alert\n");
    }

    if (sensor_client_get_snapshot(&cli, &snap) == 0) {
        dump_snapshot(&snap);
    } else {
        perror("GET_SNAPSHOT after refresh");
    }

    printf("waiting PIR event...\n");

    while (1) {
        ssize_t n;

        ret = sensor_client_wait_readable(&cli, 5000);
        if (ret < 0) {
            break;
        }
        if (ret == 0) {
            printf("poll timeout\n");
            continue;
        }

        n = sensor_client_read_events(&cli, events, 8);
        if (n < 0) {
            break;
        }

        for (ssize_t i = 0; i < n; ++i) {
            dump_event(&events[i]);
        }

        if (sensor_client_get_snapshot(&cli, &snap) == 0) {
            dump_snapshot(&snap);
        }
    }

    sensor_client_close(&cli);
    return 0;
}
