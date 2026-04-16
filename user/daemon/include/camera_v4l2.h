#ifndef CAMERA_V4L2_H
#define CAMERA_V4L2_H

#include <linux/videodev2.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/time.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CAMERA_DEFAULT_DEVICE   "/dev/video0"
#define CAMERA_DEFAULT_WIDTH    1920
#define CAMERA_DEFAULT_HEIGHT   1080
#define CAMERA_DEFAULT_PIXFMT   V4L2_PIX_FMT_NV12
#define CAMERA_DEFAULT_REQBUFS  4
#define CAMERA_DEFAULT_FPS      30

typedef struct {
    void *start;
    size_t length;
} camera_plane_buffer_t;

typedef struct {
    camera_plane_buffer_t planes[VIDEO_MAX_PLANES];
    unsigned int num_planes;
} camera_buffer_t;

typedef struct {
    int fd;
    char dev_name[64];

    uint32_t width;
    uint32_t height;
    uint32_t pixfmt;
    unsigned int fps;

    unsigned int reqbuf_count;
    unsigned int num_planes;
    unsigned int n_buffers;

    unsigned int bytesperline[VIDEO_MAX_PLANES];
    unsigned int sizeimage[VIDEO_MAX_PLANES];

    int streaming;

    camera_buffer_t *buffers;
} camera_ctx_t;

typedef struct {
    unsigned int index;
    unsigned int num_planes;
    void *planes[VIDEO_MAX_PLANES];
    size_t bytesused[VIDEO_MAX_PLANES];
    struct timeval timestamp;
    uint32_t sequence;
} camera_frame_t;

/* 生命周期 */
int camera_init(camera_ctx_t *ctx,
                const char *dev_name,
                uint32_t width,
                uint32_t height,
                uint32_t pixfmt,
                unsigned int reqbuf_count,
                unsigned int fps);

int camera_start(camera_ctx_t *ctx);
int camera_stop(camera_ctx_t *ctx);
void camera_deinit(camera_ctx_t *ctx);

/* 连续采集接口 */
int camera_dequeue(camera_ctx_t *ctx, camera_frame_t *frame, int timeout_ms);
int camera_queue(camera_ctx_t *ctx, unsigned int index);

/* trigger 用：抓取一帧并保存为原始 yuv 数据 */
int camera_capture_one(camera_ctx_t *ctx,
                       const char *filename,
                       unsigned int skip_frames);

/* 工具函数 */
const char *camera_pixfmt_to_str(uint32_t pixfmt);
void camera_print_format(camera_ctx_t *ctx);

#ifdef __cplusplus
}
#endif

#endif