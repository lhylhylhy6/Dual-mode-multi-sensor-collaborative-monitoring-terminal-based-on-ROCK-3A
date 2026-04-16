#include "camera_v4l2.h"
#include "sensor_client.h"
#include "app_state.h"
#include "http_server.h"

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <turbojpeg.h>

#define SENSOR_HUB_DEV              "/dev/sensor_hub"
#define OUTPUT_DIR                  "output"
#define CAPTURE_SKIP_FRAMES         5
#define HTTP_PORT                   8080

#define MONITOR_WIDTH               1280
#define MONITOR_HEIGHT              720
#define MONITOR_FPS                 30
#define MONITOR_JPEG_QUALITY        75
#define MONITOR_STATUS_INTERVAL_MS  1000

static volatile sig_atomic_t g_running = 1;

typedef struct {
    pthread_t tid;
    int running;
    int stop;
    camera_ctx_t *cam;
} monitor_worker_t;

static monitor_worker_t g_monitor = {0};

static void on_signal(int signo)
{
    (void)signo;
    g_running = 0;
}

static int ensure_output_dir(void)
{
    struct stat st;

    if (stat(OUTPUT_DIR, &st) == 0) {
        if (S_ISDIR(st.st_mode)) {
            return 0;
        }
        fprintf(stderr, "%s exists but is not a directory\n", OUTPUT_DIR);
        return -1;
    }

    if (mkdir(OUTPUT_DIR, 0755) < 0) {
        perror("mkdir output");
        return -1;
    }

    return 0;
}

static void format_now(char *buf, size_t len, const char *fmt)
{
    time_t now = time(NULL);
    struct tm tm_now;

    localtime_r(&now, &tm_now);
    strftime(buf, len, fmt, &tm_now);
}

static void make_capture_paths(char *yuv_path, size_t yuv_len,
                               char *jpg_path, size_t jpg_len)
{
    char ts[64];

    format_now(ts, sizeof(ts), "%Y%m%d_%H%M%S");

    snprintf(yuv_path, yuv_len,
             OUTPUT_DIR "/snap_%s_%ux%u_nv12.yuv",
             ts,
             CAMERA_DEFAULT_WIDTH,
             CAMERA_DEFAULT_HEIGHT);

    snprintf(jpg_path, jpg_len,
             OUTPUT_DIR "/snap_%s_%ux%u_nv12.jpg",
             ts,
             CAMERA_DEFAULT_WIDTH,
             CAMERA_DEFAULT_HEIGHT);
}

static int convert_yuv_to_jpg(const char *yuv_path, const char *jpg_path)
{
    char cmd[1024];
    int ret;

    snprintf(cmd, sizeof(cmd),
             "ffmpeg -y -f rawvideo -pix_fmt nv12 -video_size %ux%u "
             "-i '%s' -frames:v 1 '%s' >/dev/null 2>&1",
             CAMERA_DEFAULT_WIDTH,
             CAMERA_DEFAULT_HEIGHT,
             yuv_path,
             jpg_path);

    ret = system(cmd);
    if (ret != 0) {
        fprintf(stderr, "ffmpeg convert failed: %s\n", yuv_path);
        return -1;
    }

    if (remove(yuv_path) != 0) {
        perror("remove yuv");
    }

    return 0;
}

static int snapshot_get_pir_level(const struct sh_snapshot *snap, int *level)
{
    const struct sh_sensor_value *v;

    v = sensor_client_find_value(snap, SH_SENSOR_PIR);
    if (!v || v->nvalues < 1) {
        return -1;
    }

    *level = v->values[0];
    return 0;
}

static int snapshot_get_sht20(const struct sh_snapshot *snap, float *temp_c, float *humi_rh)
{
    const struct sh_sensor_value *v;

    v = sensor_client_find_value(snap, SH_SENSOR_SHT20);
    if (!v || v->nvalues < 2) {
        return -1;
    }

    *temp_c = (float)v->values[0] / 1000.0f;
    *humi_rh = (float)v->values[1] / 1000.0f;
    return 0;
}

static void append_event_log(const char *jpg_path, const struct sh_snapshot *snap)
{
    FILE *fp;
    char ts[64];
    float temp_c = 0.0f;
    float humi_rh = 0.0f;
    int pir_level = 0;

    format_now(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S");

    snapshot_get_sht20(snap, &temp_c, &humi_rh);
    snapshot_get_pir_level(snap, &pir_level);

    fp = fopen(OUTPUT_DIR "/events.log", "a");
    if (!fp) {
        perror("fopen events.log");
        return;
    }

    fprintf(fp,
            "%s MODE=trigger PIR=%d TEMP=%.2fC HUMI=%.2f%%RH IMG=%s\n",
            ts, pir_level, temp_c, humi_rh, jpg_path);

    fclose(fp);
}

static void print_snapshot_brief(app_mode_t mode, const struct sh_snapshot *snap)
{
    int pir_level = 0;
    float temp_c = 0.0f;
    float humi_rh = 0.0f;

    snapshot_get_pir_level(snap, &pir_level);
    snapshot_get_sht20(snap, &temp_c, &humi_rh);

    printf("[STATUS] mode=%s pir=%d temp=%.2fC humi=%.2f%%RH seq=%llu\n",
           app_mode_to_str(mode),
           pir_level,
           temp_c,
           humi_rh,
           (unsigned long long)snap->seq);
}

static int apply_default_cfg(sensor_client_t *cli)
{
    struct sh_sensor_cfg cfg;

    memset(&cfg, 0, sizeof(cfg));
    cfg.id = SH_SENSOR_PIR;
    cfg.enabled = 1;
    cfg.debounce_ms = 200;
    if (sensor_client_set_sensor_cfg(cli, &cfg) < 0) {
        perror("set PIR cfg");
        return -1;
    }

    memset(&cfg, 0, sizeof(cfg));
    cfg.id = SH_SENSOR_SHT20;
    cfg.enabled = 1;
    cfg.period_ms = 1000;
    if (sensor_client_set_sensor_cfg(cli, &cfg) < 0) {
        perror("set SHT20 cfg");
        return -1;
    }

    return 0;
}

static void update_capture_state(app_state_t *app, const char *image_path, int ok)
{
    char ts[64];

    format_now(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S");
    app_state_update_capture(app, image_path, ts, ok);
}

static int handle_trigger(sensor_client_t *cli, camera_ctx_t *cam, app_state_t *app)
{
    struct sh_snapshot snap;
    char yuv_path[256];
    char jpg_path[256];

    if (sensor_client_force_refresh(cli, SH_SENSOR_SHT20, 0) < 0) {
        perror("force refresh SHT20");
    }

    if (sensor_client_get_snapshot(cli, &snap) < 0) {
        perror("get snapshot");
        update_capture_state(app, "", 0);
        return -1;
    }

    app_state_update_snapshot(app, &snap);

    make_capture_paths(yuv_path, sizeof(yuv_path), jpg_path, sizeof(jpg_path));

    printf("[INFO] PIR trigger received, capture -> %s\n", jpg_path);

    if (camera_capture_one(cam, yuv_path, CAPTURE_SKIP_FRAMES) < 0) {
        fprintf(stderr, "camera_capture_one failed\n");
        update_capture_state(app, "", 0);
        return -1;
    }

    if (convert_yuv_to_jpg(yuv_path, jpg_path) < 0) {
        update_capture_state(app, "", 0);
        return -1;
    }

    update_capture_state(app, jpg_path, 1);
    append_event_log(jpg_path, &snap);
    print_snapshot_brief(APP_MODE_TRIGGER, &snap);
    return 0;
}

static app_mode_t parse_mode(int argc, char **argv)
{
    app_mode_t mode;

    if (argc >= 2 && app_mode_from_str(argv[1], &mode) == 0) {
        return mode;
    }

    return APP_MODE_TRIGGER;
}

static int camera_open_for_mode(camera_ctx_t *cam, app_mode_t mode)
{
    uint32_t width;
    uint32_t height;
    unsigned int fps;

    if (mode == APP_MODE_MONITOR) {
        width = MONITOR_WIDTH;
        height = MONITOR_HEIGHT;
        fps = MONITOR_FPS;
    } else {
        width = CAMERA_DEFAULT_WIDTH;
        height = CAMERA_DEFAULT_HEIGHT;
        fps = CAMERA_DEFAULT_FPS;
    }

    if (camera_init(cam,
                    CAMERA_DEFAULT_DEVICE,
                    width,
                    height,
                    CAMERA_DEFAULT_PIXFMT,
                    CAMERA_DEFAULT_REQBUFS,
                    fps) < 0) {
        fprintf(stderr, "camera_init failed for mode=%s\n", app_mode_to_str(mode));
        return -1;
    }

    if (camera_start(cam) < 0) {
        fprintf(stderr, "camera_start failed for mode=%s\n", app_mode_to_str(mode));
        camera_deinit(cam);
        return -1;
    }

    return 0;
}

static int camera_reopen_with_retry(camera_ctx_t *cam, app_mode_t mode)
{
    int i;

    for (i = 0; i < 3; ++i) {
        if (camera_open_for_mode(cam, mode) == 0) {
            return 0;
        }

        fprintf(stderr,
                "[WARN] camera_open_for_mode(%s) failed, retry %d/3\n",
                app_mode_to_str(mode), i + 1);

        usleep(300 * 1000);
    }

    return -1;
}

static int nv12_to_i420(const camera_ctx_t *cam,
                        const camera_frame_t *frame,
                        unsigned char *dst_i420)
{
    unsigned char *dst_y = dst_i420;
    unsigned char *dst_u = dst_y + cam->width * cam->height;
    unsigned char *dst_v = dst_u + (cam->width * cam->height / 4);

    const unsigned char *src_y;
    const unsigned char *src_uv;
    unsigned int y_stride;
    unsigned int uv_stride;
    uint32_t x, y;

    if (!cam || !frame || !dst_i420) {
        return -1;
    }

    if (cam->pixfmt != V4L2_PIX_FMT_NV12) {
        fprintf(stderr, "monitor only supports NV12 for now\n");
        return -1;
    }

    y_stride = cam->bytesperline[0] ? cam->bytesperline[0] : cam->width;

    src_y = (const unsigned char *)frame->planes[0];

    if (frame->num_planes >= 2) {
        src_uv = (const unsigned char *)frame->planes[1];
        uv_stride = cam->bytesperline[1] ? cam->bytesperline[1] : cam->width;
    } else {
        src_uv = src_y + (size_t)y_stride * cam->height;
        uv_stride = y_stride;
    }

    for (y = 0; y < cam->height; ++y) {
        memcpy(dst_y + (size_t)y * cam->width,
               src_y + (size_t)y * y_stride,
               cam->width);
    }

    for (y = 0; y < cam->height / 2; ++y) {
        const unsigned char *uv_row = src_uv + (size_t)y * uv_stride;
        unsigned char *u_row = dst_u + (size_t)y * (cam->width / 2);
        unsigned char *v_row = dst_v + (size_t)y * (cam->width / 2);

        for (x = 0; x < cam->width / 2; ++x) {
            u_row[x] = uv_row[x * 2];
            v_row[x] = uv_row[x * 2 + 1];
        }
    }

    return 0;
}

static int encode_nv12_frame_to_jpeg(tjhandle tj,
                                     const camera_ctx_t *cam,
                                     const camera_frame_t *frame,
                                     unsigned char *i420_buf,
                                     unsigned char *jpeg_buf,
                                     unsigned long jpeg_cap,
                                     unsigned long *jpeg_size)
{
    const unsigned char *planes[3];
    int strides[3];

    if (nv12_to_i420(cam, frame, i420_buf) < 0) {
        return -1;
    }

    planes[0] = i420_buf;
    planes[1] = i420_buf + cam->width * cam->height;
    planes[2] = planes[1] + (cam->width * cam->height / 4);

    strides[0] = (int)cam->width;
    strides[1] = (int)(cam->width / 2);
    strides[2] = (int)(cam->width / 2);

    *jpeg_size = jpeg_cap;

    if (tjCompressFromYUVPlanes(tj,
                                planes,
                                (int)cam->width,
                                strides,
                                (int)cam->height,
                                TJSAMP_420,
                                &jpeg_buf,
                                jpeg_size,
                                MONITOR_JPEG_QUALITY,
                                TJFLAG_FASTDCT | TJFLAG_NOREALLOC) != 0) {
        fprintf(stderr, "tjCompressFromYUVPlanes failed: %s\n", tjGetErrorStr());
        return -1;
    }

    return 0;
}

static void *monitor_thread_main(void *arg)
{
    monitor_worker_t *worker = (monitor_worker_t *)arg;
    camera_ctx_t *cam = worker->cam;
    tjhandle tj = NULL;
    unsigned char *i420_buf = NULL;
    unsigned char *jpeg_buf = NULL;
    unsigned long jpeg_cap = 0;

    tj = tjInitCompress();
    if (!tj) {
        fprintf(stderr, "tjInitCompress failed\n");
        worker->running = 0;
        return NULL;
    }

    i420_buf = malloc((size_t)cam->width * cam->height * 3 / 2);
    if (!i420_buf) {
        perror("malloc i420_buf");
        tjDestroy(tj);
        worker->running = 0;
        return NULL;
    }

    jpeg_cap = tjBufSize((int)cam->width, (int)cam->height, TJSAMP_420);
    jpeg_buf = tjAlloc(jpeg_cap);
    if (!jpeg_buf) {
        fprintf(stderr, "tjAlloc failed\n");
        free(i420_buf);
        tjDestroy(tj);
        worker->running = 0;
        return NULL;
    }

    printf("[INFO] monitor worker started: %ux%u@%u\n",
           cam->width, cam->height, cam->fps);

    while (!worker->stop) {
        camera_frame_t frame;
        int ret;

        ret = camera_dequeue(cam, &frame, 200);
        if (ret < 0) {
            fprintf(stderr, "camera_dequeue failed in monitor thread\n");
            break;
        }
        if (ret == 0) {
            continue;
        }

        {
            unsigned long jpeg_size = 0;

            if (encode_nv12_frame_to_jpeg(tj,
                                          cam,
                                          &frame,
                                          i420_buf,
                                          jpeg_buf,
                                          jpeg_cap,
                                          &jpeg_size) == 0) {
                http_server_publish_jpeg(jpeg_buf, (size_t)jpeg_size);
            }
        }

        if (camera_queue(cam, frame.index) < 0) {
            fprintf(stderr, "camera_queue failed in monitor thread\n");
            break;
        }
    }

    http_server_clear_mjpeg();

    tjFree(jpeg_buf);
    free(i420_buf);
    tjDestroy(tj);

    worker->running = 0;
    printf("[INFO] monitor worker stopped\n");
    return NULL;
}

static int start_monitor_worker(camera_ctx_t *cam)
{
    if (g_monitor.running) {
        return 0;
    }

    memset(&g_monitor, 0, sizeof(g_monitor));
    g_monitor.cam = cam;
    g_monitor.stop = 0;
    g_monitor.running = 1;

    if (pthread_create(&g_monitor.tid, NULL, monitor_thread_main, &g_monitor) != 0) {
        g_monitor.running = 0;
        return -1;
    }

    return 0;
}

static void stop_monitor_worker(void)
{
    if (!g_monitor.running) {
        return;
    }

    g_monitor.stop = 1;
    pthread_join(g_monitor.tid, NULL);
    memset(&g_monitor, 0, sizeof(g_monitor));
}

int main(int argc, char **argv)
{
    app_mode_t start_mode;
    app_mode_t active_mode;
    sensor_client_t cli;
    camera_ctx_t cam;
    struct sh_hub_info info;
    app_state_t app;
    http_server_ctx_t http_ctx;
    uint64_t last_monitor_status_ms = 0;

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    start_mode = parse_mode(argc, argv);

    if (ensure_output_dir() < 0) {
        return 1;
    }

    if (sensor_client_open(&cli, SENSOR_HUB_DEV) < 0) {
        return 1;
    }

    if (sensor_client_get_info(&cli, &info) == 0) {
        printf("[INFO] sensor_hub version=0x%x sensors=%u qsize=%u\n",
               info.version, info.sensor_count, info.queue_size);
    }

    if (apply_default_cfg(&cli) < 0) {
        sensor_client_close(&cli);
        return 1;
    }

    if (app_state_init(&app) < 0) {
        fprintf(stderr, "app_state_init failed\n");
        sensor_client_close(&cli);
        return 1;
    }

    app_state_set_mode(&app, start_mode);

    memset(&http_ctx, 0, sizeof(http_ctx));
    http_ctx.state = &app;
    http_ctx.port = HTTP_PORT;
    http_ctx.log_path = OUTPUT_DIR "/events.log";

    if (http_server_start(&http_ctx) < 0) {
        fprintf(stderr, "http_server_start failed\n");
        app_state_deinit(&app);
        sensor_client_close(&cli);
        return 1;
    }

    memset(&cam, 0, sizeof(cam));
    cam.fd = -1;

    active_mode = start_mode;
    if (camera_open_for_mode(&cam, active_mode) < 0) {
        http_server_stop();
        app_state_deinit(&app);
        sensor_client_close(&cli);
        return 1;
    }

    if (active_mode == APP_MODE_MONITOR) {
        if (start_monitor_worker(&cam) < 0) {
            fprintf(stderr, "start_monitor_worker failed\n");
            camera_deinit(&cam);
            http_server_stop();
            app_state_deinit(&app);
            sensor_client_close(&cli);
            return 1;
        }
    }

    printf("[INFO] start mode=%s\n", app_mode_to_str(start_mode));

    while (g_running) {
        app_mode_t requested_mode = app_state_get_mode(&app);

        if (requested_mode != active_mode) {
			int switch_ok = 1;

			printf("[INFO] mode switch: %s -> %s\n",
				   app_mode_to_str(active_mode),
				   app_mode_to_str(requested_mode));

			if (active_mode == APP_MODE_MONITOR) {
				fprintf(stderr, "[DBG] stopping monitor worker\n");
				stop_monitor_worker();
				http_server_clear_mjpeg();
			}

			fprintf(stderr, "[DBG] deinit current camera\n");
			camera_deinit(&cam);
			usleep(200 * 1000);

			fprintf(stderr, "[DBG] reopen camera for %s\n",
					app_mode_to_str(requested_mode));

			if (camera_reopen_with_retry(&cam, requested_mode) < 0) {
				fprintf(stderr, "[ERR] camera reopen failed after mode switch\n");
				switch_ok = 0;
			}

			if (switch_ok && requested_mode == APP_MODE_MONITOR) {
				http_server_clear_mjpeg();

				fprintf(stderr, "[DBG] start monitor worker\n");
				if (start_monitor_worker(&cam) < 0) {
					fprintf(stderr, "[ERR] start_monitor_worker failed after mode switch\n");
					switch_ok = 0;
				}
			}

			if (!switch_ok) {
				fprintf(stderr, "[WARN] rollback to previous mode: %s\n",
						app_mode_to_str(active_mode));

				camera_deinit(&cam);
				usleep(200 * 1000);

				if (camera_reopen_with_retry(&cam, active_mode) == 0) {
					if (active_mode == APP_MODE_MONITOR) {
						http_server_clear_mjpeg();
						if (start_monitor_worker(&cam) < 0) {
							fprintf(stderr, "[ERR] rollback start_monitor_worker failed\n");
							break;
						}
					}

					app_state_set_mode(&app, active_mode);
					continue;
				}

				fprintf(stderr, "[ERR] rollback failed, exiting\n");
				break;
			}

			active_mode = requested_mode;
			fprintf(stderr, "[DBG] mode switch done\n");
			continue;
		}

        if (active_mode == APP_MODE_MONITOR) {
            struct timespec ts;
            uint64_t now_ms;

            clock_gettime(CLOCK_MONOTONIC, &ts);
            now_ms = (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)(ts.tv_nsec / 1000000ULL);

            if (now_ms - last_monitor_status_ms >= MONITOR_STATUS_INTERVAL_MS) {
                struct sh_snapshot snap;

                if (sensor_client_get_snapshot(&cli, &snap) == 0) {
                    app_state_update_snapshot(&app, &snap);
                    print_snapshot_brief(APP_MODE_MONITOR, &snap);
                } else {
                    perror("get snapshot");
                }

                last_monitor_status_ms = now_ms;
            }

            usleep(100000);
        } else {
            struct sh_event events[16];
            ssize_t n;
            int ret;

            ret = sensor_client_wait_readable(&cli, 200);
            if (ret < 0) {
                if (!g_running) {
                    break;
                }
                continue;
            }
            if (ret == 0) {
                continue;
            }

            n = sensor_client_read_events(&cli, events, 16);
            if (n < 0) {
                if (!g_running) {
                    break;
                }
                continue;
            }

            for (ssize_t i = 0; i < n; ++i) {
                struct sh_event *evt = &events[i];

                if (evt->sensor_id != SH_SENSOR_PIR) {
                    continue;
                }

                if (evt->code != SH_CODE_TRIGGER && evt->code != SH_CODE_RISING) {
                    continue;
                }

                if (evt->nvalues < 1 || evt->values[0] != 1) {
                    continue;
                }

                if (handle_trigger(&cli, &cam, &app) < 0) {
                    fprintf(stderr, "handle_trigger failed\n");
                }
            }
        }
    }

    if (active_mode == APP_MODE_MONITOR) {
        stop_monitor_worker();
        http_server_clear_mjpeg();
    }

    camera_deinit(&cam);
    http_server_stop();
    app_state_deinit(&app);
    sensor_client_close(&cli);
    return 0;
}