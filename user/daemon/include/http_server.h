#ifndef HTTP_SERVER_H
#define HTTP_SERVER_H

#include "app_state.h"
#include <stddef.h>

typedef struct {
    app_state_t *state;
    int port;
    const char *log_path;
} http_server_ctx_t;

int http_server_start(http_server_ctx_t *ctx);
void http_server_stop(void);

/* monitor 线程把最新 JPEG 发布给 HTTP server */
int http_server_publish_jpeg(const void *jpeg, size_t len);
void http_server_clear_mjpeg(void);

#endif