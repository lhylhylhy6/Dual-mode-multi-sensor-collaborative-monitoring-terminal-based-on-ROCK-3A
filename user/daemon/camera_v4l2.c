#include "camera_v4l2.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/select.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

#define CLEAR(x) memset(&(x), 0, sizeof(x))

static int xioctl(int fd, unsigned long request, void *arg)
{
    int ret;
    do {
        ret = ioctl(fd, request, arg);
    } while (ret == -1 && errno == EINTR);
    return ret;
}

const char *camera_pixfmt_to_str(uint32_t pixfmt)
{
    switch (pixfmt) {
    case V4L2_PIX_FMT_NV12:
        return "NV12";
    case V4L2_PIX_FMT_UYVY:
        return "UYVY";
    case V4L2_PIX_FMT_YUV420:
        return "YU12";
    default:
        return "UNKNOWN";
    }
}

static int camera_check_caps(camera_ctx_t *ctx)
{
    struct v4l2_capability cap;
    uint32_t dev_caps;

    CLEAR(cap);

    if (xioctl(ctx->fd, VIDIOC_QUERYCAP, &cap) == -1) {
        perror("VIDIOC_QUERYCAP");
        return -1;
    }

    printf("Driver Info:\n");
    printf("  driver      : %s\n", cap.driver);
    printf("  card        : %s\n", cap.card);
    printf("  bus_info    : %s\n", cap.bus_info);
    printf("  capabilities: 0x%08x\n", cap.capabilities);
    printf("  device_caps : 0x%08x\n", cap.device_caps);

    dev_caps = cap.device_caps ? cap.device_caps : cap.capabilities;

    if (!(dev_caps & V4L2_CAP_VIDEO_CAPTURE_MPLANE)) {
        fprintf(stderr, "device does not support VIDEO_CAPTURE_MPLANE\n");
        return -1;
    }

    if (!(dev_caps & V4L2_CAP_STREAMING)) {
        fprintf(stderr, "device does not support STREAMING I/O\n");
        return -1;
    }

    return 0;
}

static int camera_set_format(camera_ctx_t *ctx)
{
    struct v4l2_format fmt;
    unsigned int i;

    CLEAR(fmt);

    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    fmt.fmt.pix_mp.width = ctx->width;
    fmt.fmt.pix_mp.height = ctx->height;
    fmt.fmt.pix_mp.pixelformat = ctx->pixfmt;
    fmt.fmt.pix_mp.field = V4L2_FIELD_NONE;

    if (xioctl(ctx->fd, VIDIOC_S_FMT, &fmt) == -1) {
        perror("VIDIOC_S_FMT");
        return -1;
    }

    ctx->width = fmt.fmt.pix_mp.width;
    ctx->height = fmt.fmt.pix_mp.height;
    ctx->pixfmt = fmt.fmt.pix_mp.pixelformat;
    ctx->num_planes = fmt.fmt.pix_mp.num_planes;

    if (ctx->num_planes == 0 || ctx->num_planes > VIDEO_MAX_PLANES) {
        fprintf(stderr, "invalid num_planes=%u\n", ctx->num_planes);
        return -1;
    }

    printf("Applied format:\n");
    printf("  width      : %u\n", ctx->width);
    printf("  height     : %u\n", ctx->height);
    printf("  pixelformat: %.4s\n", (char *)&ctx->pixfmt);
    printf("  num_planes : %u\n", ctx->num_planes);

    for (i = 0; i < ctx->num_planes; ++i) {
        ctx->bytesperline[i] = fmt.fmt.pix_mp.plane_fmt[i].bytesperline;
        ctx->sizeimage[i] = fmt.fmt.pix_mp.plane_fmt[i].sizeimage;

        printf("  plane[%u]   : bytesperline=%u sizeimage=%u\n",
               i,
               ctx->bytesperline[i],
               ctx->sizeimage[i]);
    }

    return 0;
}

static void camera_try_set_fps(camera_ctx_t *ctx)
{
    struct v4l2_streamparm parm;

    CLEAR(parm);
    parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;

    if (xioctl(ctx->fd, VIDIOC_G_PARM, &parm) == -1) {
        perror("VIDIOC_G_PARM");
        return;
    }

    if (!(parm.parm.capture.capability & V4L2_CAP_TIMEPERFRAME)) {
        printf("camera does not report V4L2_CAP_TIMEPERFRAME, skip fps setup\n");
        return;
    }

    parm.parm.capture.timeperframe.numerator = 1;
    parm.parm.capture.timeperframe.denominator = ctx->fps ? ctx->fps : CAMERA_DEFAULT_FPS;

    if (xioctl(ctx->fd, VIDIOC_S_PARM, &parm) == -1) {
        perror("VIDIOC_S_PARM");
        return;
    }

    if (parm.parm.capture.timeperframe.numerator != 0) {
        double actual_fps =
            (double)parm.parm.capture.timeperframe.denominator /
            (double)parm.parm.capture.timeperframe.numerator;
        printf("Applied fps: %.2f\n", actual_fps);
    }
}

void camera_print_format(camera_ctx_t *ctx)
{
    struct v4l2_format fmt;
    unsigned int i;

    if (!ctx || ctx->fd < 0) {
        return;
    }

    CLEAR(fmt);
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;

    if (xioctl(ctx->fd, VIDIOC_G_FMT, &fmt) == -1) {
        perror("VIDIOC_G_FMT");
        return;
    }

    printf("Current format:\n");
    printf("  width      : %u\n", fmt.fmt.pix_mp.width);
    printf("  height     : %u\n", fmt.fmt.pix_mp.height);
    printf("  pixelformat: %.4s\n", (char *)&fmt.fmt.pix_mp.pixelformat);
    printf("  num_planes : %u\n", fmt.fmt.pix_mp.num_planes);

    for (i = 0; i < fmt.fmt.pix_mp.num_planes; ++i) {
        printf("  plane[%u]   : bytesperline=%u sizeimage=%u\n",
               i,
               fmt.fmt.pix_mp.plane_fmt[i].bytesperline,
               fmt.fmt.pix_mp.plane_fmt[i].sizeimage);
    }
}

static int camera_reqbufs_and_mmap(camera_ctx_t *ctx)
{
    struct v4l2_requestbuffers req;
    unsigned int i, p;

    CLEAR(req);
    req.count = ctx->reqbuf_count;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    req.memory = V4L2_MEMORY_MMAP;

    if (xioctl(ctx->fd, VIDIOC_REQBUFS, &req) == -1) {
        perror("VIDIOC_REQBUFS");
        return -1;
    }

    if (req.count < 2) {
        fprintf(stderr, "insufficient buffer memory: got %u buffers\n", req.count);
        return -1;
    }

    ctx->buffers = calloc(req.count, sizeof(camera_buffer_t));
    if (!ctx->buffers) {
        perror("calloc");
        return -1;
    }

    ctx->n_buffers = req.count;

    for (i = 0; i < ctx->n_buffers; ++i) {
        struct v4l2_buffer buf;
        struct v4l2_plane planes[VIDEO_MAX_PLANES];

        CLEAR(buf);
        CLEAR(planes);

        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;
        buf.length = ctx->num_planes;
        buf.m.planes = planes;

        if (xioctl(ctx->fd, VIDIOC_QUERYBUF, &buf) == -1) {
            perror("VIDIOC_QUERYBUF");
            return -1;
        }

        ctx->buffers[i].num_planes = buf.length;

        for (p = 0; p < buf.length; ++p) {
            ctx->buffers[i].planes[p].length = buf.m.planes[p].length;
            ctx->buffers[i].planes[p].start =
                mmap(NULL,
                     buf.m.planes[p].length,
                     PROT_READ | PROT_WRITE,
                     MAP_SHARED,
                     ctx->fd,
                     buf.m.planes[p].m.mem_offset);

            if (ctx->buffers[i].planes[p].start == MAP_FAILED) {
                perror("mmap");
                return -1;
            }
        }
    }

    printf("MMAP initialized with %u buffers\n", ctx->n_buffers);
    return 0;
}

static int camera_queue_all(camera_ctx_t *ctx)
{
    unsigned int i;

    for (i = 0; i < ctx->n_buffers; ++i) {
        struct v4l2_buffer buf;
        struct v4l2_plane planes[VIDEO_MAX_PLANES];

        CLEAR(buf);
        CLEAR(planes);

        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;
        buf.length = ctx->num_planes;
        buf.m.planes = planes;

        if (xioctl(ctx->fd, VIDIOC_QBUF, &buf) == -1) {
            perror("VIDIOC_QBUF");
            return -1;
        }
    }

    return 0;
}

static int camera_wait_readable(camera_ctx_t *ctx, int timeout_ms)
{
    fd_set fds;
    struct timeval tv;
    int ret;

    FD_ZERO(&fds);
    FD_SET(ctx->fd, &fds);

    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;

    ret = select(ctx->fd + 1, &fds, NULL, NULL, &tv);
    if (ret == -1) {
        if (errno == EINTR) {
            return 0;
        }
        perror("select");
        return -1;
    }

    if (ret == 0) {
        return 0;
    }

    return 1;
}

static int camera_save_frame_raw(camera_ctx_t *ctx,
                                 const camera_frame_t *frame,
                                 const char *filename)
{
    FILE *fp;
    unsigned int p;

    fp = fopen(filename, "wb");
    if (!fp) {
        perror("fopen");
        return -1;
    }

    for (p = 0; p < frame->num_planes; ++p) {
        if (frame->bytesused[p] == 0) {
            continue;
        }

        if (fwrite(frame->planes[p], 1, frame->bytesused[p], fp) != frame->bytesused[p]) {
            perror("fwrite");
            fclose(fp);
            return -1;
        }
    }

    fclose(fp);

    printf("Saved frame to %s\n", filename);
    for (p = 0; p < frame->num_planes; ++p) {
        printf("  plane[%u] bytesused=%zu\n", p, frame->bytesused[p]);
    }

    (void)ctx;
    return 0;
}

int camera_init(camera_ctx_t *ctx,
                const char *dev_name,
                uint32_t width,
                uint32_t height,
                uint32_t pixfmt,
                unsigned int reqbuf_count,
                unsigned int fps)
{
    if (!ctx || !dev_name) {
        return -1;
    }

    memset(ctx, 0, sizeof(*ctx));
    ctx->fd = -1;
    ctx->width = width;
    ctx->height = height;
    ctx->pixfmt = pixfmt;
    ctx->reqbuf_count = reqbuf_count;
    ctx->fps = fps ? fps : CAMERA_DEFAULT_FPS;
    ctx->streaming = 0;

    strncpy(ctx->dev_name, dev_name, sizeof(ctx->dev_name) - 1);
    ctx->dev_name[sizeof(ctx->dev_name) - 1] = '\0';

    ctx->fd = open(ctx->dev_name, O_RDWR | O_NONBLOCK, 0);
    if (ctx->fd == -1) {
        perror("open");
        return -1;
    }

    if (camera_check_caps(ctx) < 0) {
        return -1;
    }

    if (camera_set_format(ctx) < 0) {
        return -1;
    }

    camera_try_set_fps(ctx);

    if (camera_reqbufs_and_mmap(ctx) < 0) {
        return -1;
    }

    return 0;
}

int camera_start(camera_ctx_t *ctx)
{
    enum v4l2_buf_type type;

    if (!ctx || ctx->fd < 0) {
        return -1;
    }

    if (camera_queue_all(ctx) < 0) {
        return -1;
    }

    type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    if (xioctl(ctx->fd, VIDIOC_STREAMON, &type) == -1) {
        perror("VIDIOC_STREAMON");
        return -1;
    }

    ctx->streaming = 1;
    return 0;
}

int camera_dequeue(camera_ctx_t *ctx, camera_frame_t *frame, int timeout_ms)
{
    struct v4l2_buffer buf;
    struct v4l2_plane planes[VIDEO_MAX_PLANES];
    int ret;
    unsigned int p;

    if (!ctx || !frame || ctx->fd < 0 || !ctx->streaming) {
        return -1;
    }

    ret = camera_wait_readable(ctx, timeout_ms);
    if (ret <= 0) {
        return ret;
    }

    CLEAR(buf);
    CLEAR(planes);

    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    buf.memory = V4L2_MEMORY_MMAP;
    buf.length = ctx->num_planes;
    buf.m.planes = planes;

    if (xioctl(ctx->fd, VIDIOC_DQBUF, &buf) == -1) {
        if (errno == EAGAIN) {
            return 0;
        }
        perror("VIDIOC_DQBUF");
        return -1;
    }

    memset(frame, 0, sizeof(*frame));
    frame->index = buf.index;
    frame->num_planes = buf.length;
    frame->timestamp = buf.timestamp;
    frame->sequence = buf.sequence;

    for (p = 0; p < buf.length; ++p) {
        frame->planes[p] = ctx->buffers[buf.index].planes[p].start;
        frame->bytesused[p] = planes[p].bytesused;
    }

    return 1;
}

int camera_queue(camera_ctx_t *ctx, unsigned int index)
{
    struct v4l2_buffer buf;
    struct v4l2_plane planes[VIDEO_MAX_PLANES];

    if (!ctx || ctx->fd < 0 || index >= ctx->n_buffers) {
        return -1;
    }

    CLEAR(buf);
    CLEAR(planes);

    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    buf.memory = V4L2_MEMORY_MMAP;
    buf.index = index;
    buf.length = ctx->num_planes;
    buf.m.planes = planes;

    if (xioctl(ctx->fd, VIDIOC_QBUF, &buf) == -1) {
        perror("VIDIOC_QBUF");
        return -1;
    }

    return 0;
}

int camera_capture_one(camera_ctx_t *ctx,
                       const char *filename,
                       unsigned int skip_frames)
{
    unsigned int seen = 0;

    if (!ctx || !filename || ctx->fd < 0 || !ctx->streaming) {
        return -1;
    }

    while (1) {
        camera_frame_t frame;
        int ret;

        ret = camera_dequeue(ctx, &frame, 2000);
        if (ret < 0) {
            return -1;
        }
        if (ret == 0) {
            continue;
        }

        seen++;

        if (seen <= skip_frames) {
            printf("Skipping frame %u/%u\n", seen, skip_frames);
            if (camera_queue(ctx, frame.index) < 0) {
                return -1;
            }
            continue;
        }

        if (camera_save_frame_raw(ctx, &frame, filename) < 0) {
            camera_queue(ctx, frame.index);
            return -1;
        }

        if (camera_queue(ctx, frame.index) < 0) {
            return -1;
        }

        return 0;
    }
}

int camera_stop(camera_ctx_t *ctx)
{
    enum v4l2_buf_type type;

    if (!ctx || ctx->fd < 0 || !ctx->streaming) {
        return 0;
    }

    type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    if (xioctl(ctx->fd, VIDIOC_STREAMOFF, &type) == -1) {
        perror("VIDIOC_STREAMOFF");
        return -1;
    }

    ctx->streaming = 0;
    return 0;
}

void camera_deinit(camera_ctx_t *ctx)
{
    unsigned int i, p;

    if (!ctx) {
        return;
    }

    if (ctx->streaming) {
        camera_stop(ctx);
    }

    if (ctx->buffers) {
        for (i = 0; i < ctx->n_buffers; ++i) {
            for (p = 0; p < ctx->buffers[i].num_planes; ++p) {
                if (ctx->buffers[i].planes[p].start &&
                    ctx->buffers[i].planes[p].start != MAP_FAILED) {
                    munmap(ctx->buffers[i].planes[p].start,
                           ctx->buffers[i].planes[p].length);
                }
            }
        }

        free(ctx->buffers);
        ctx->buffers = NULL;
    }

    if (ctx->fd >= 0) {
        close(ctx->fd);
        ctx->fd = -1;
    }
}