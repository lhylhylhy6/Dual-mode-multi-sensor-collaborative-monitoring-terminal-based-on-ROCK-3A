#include "http_server.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#define WEB_INDEX_PATH "web/index.html"
#define MJPEG_BOUNDARY "frame"

typedef struct {
    pthread_mutex_t lock;
    pthread_cond_t cond;
    unsigned char *buf;
    size_t len;
    size_t cap;
    uint64_t seq;
    int valid;
} mjpeg_cache_t;

typedef struct {
    int client_fd;
} client_arg_t;

static volatile int g_http_running = 0;
static pthread_t g_http_thread;
static http_server_ctx_t g_ctx;

static mjpeg_cache_t g_mjpeg = {
    .lock = PTHREAD_MUTEX_INITIALIZER,
    .cond = PTHREAD_COND_INITIALIZER,
    .buf = NULL,
    .len = 0,
    .cap = 0,
    .seq = 0,
    .valid = 0,
};

static int snapshot_get_pir_level(const struct sh_snapshot *snap, int *level)
{
    unsigned int i;

    if (!snap || !level) {
        return -1;
    }

    for (i = 0; i < snap->count; ++i) {
        if (snap->items[i].id == SH_SENSOR_PIR && snap->items[i].nvalues >= 1) {
            *level = snap->items[i].values[0];
            return 0;
        }
    }

    return -1;
}

static int snapshot_get_sht20(const struct sh_snapshot *snap, float *temp_c, float *humi_rh)
{
    unsigned int i;

    if (!snap || !temp_c || !humi_rh) {
        return -1;
    }

    for (i = 0; i < snap->count; ++i) {
        if (snap->items[i].id == SH_SENSOR_SHT20 && snap->items[i].nvalues >= 2) {
            *temp_c = (float)snap->items[i].values[0] / 1000.0f;
            *humi_rh = (float)snap->items[i].values[1] / 1000.0f;
            return 0;
        }
    }

    return -1;
}

static int send_all(int fd, const void *buf, size_t len)
{
    const unsigned char *p = (const unsigned char *)buf;
    size_t sent = 0;

    while (sent < len) {
        ssize_t n = write(fd, p + sent, len - sent);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (n == 0) {
            return -1;
        }
        sent += (size_t)n;
    }

    return 0;
}

static void send_response_header(int fd,
                                 int status_code,
                                 const char *status_text,
                                 const char *content_type,
                                 size_t content_length)
{
    char header[512];
    int n;

    n = snprintf(header, sizeof(header),
                 "HTTP/1.1 %d %s\r\n"
                 "Content-Type: %s\r\n"
                 "Content-Length: %zu\r\n"
                 "Connection: close\r\n"
                 "\r\n",
                 status_code, status_text, content_type, content_length);

    if (n > 0) {
        send_all(fd, header, (size_t)n);
    }
}

static void send_json(int fd, int status_code, const char *status_text, const char *json)
{
    send_response_header(fd, status_code, status_text, "application/json", strlen(json));
    send_all(fd, json, strlen(json));
}

static void send_text(int fd, int status_code, const char *status_text, const char *text)
{
    send_response_header(fd, status_code, status_text,
                         "text/plain; charset=utf-8", strlen(text));
    send_all(fd, text, strlen(text));
}

static const char *guess_content_type(const char *path)
{
    const char *ext = strrchr(path, '.');

    if (!ext) {
        return "application/octet-stream";
    }

    if (strcmp(ext, ".html") == 0) return "text/html; charset=utf-8";
    if (strcmp(ext, ".jpg") == 0) return "image/jpeg";
    if (strcmp(ext, ".jpeg") == 0) return "image/jpeg";
    if (strcmp(ext, ".png") == 0) return "image/png";
    if (strcmp(ext, ".css") == 0) return "text/css; charset=utf-8";
    if (strcmp(ext, ".js") == 0) return "application/javascript";

    return "application/octet-stream";
}

static int send_file_path(int fd, const char *path)
{
    FILE *fp;
    struct stat st;
    char buf[4096];
    size_t n;

    if (stat(path, &st) < 0 || !S_ISREG(st.st_mode)) {
        send_json(fd, 404, "Not Found", "{\"error\":\"file not found\"}");
        return -1;
    }

    fp = fopen(path, "rb");
    if (!fp) {
        send_json(fd, 500, "Internal Server Error", "{\"error\":\"open file failed\"}");
        return -1;
    }

    send_response_header(fd, 200, "OK", guess_content_type(path), (size_t)st.st_size);

    while ((n = fread(buf, 1, sizeof(buf), fp)) > 0) {
        if (send_all(fd, buf, n) < 0) {
            break;
        }
    }

    fclose(fp);
    return 0;
}

static int is_safe_path(const char *path)
{
    if (!path || path[0] != '/') {
        return 0;
    }

    if (strstr(path, "..")) {
        return 0;
    }

    return 1;
}

static void strip_query_string(char *path)
{
    char *q;

    if (!path) {
        return;
    }

    q = strchr(path, '?');
    if (q) {
        *q = '\0';
    }
}

static void handle_status(int fd)
{
    app_state_t copy;
    char body[2048];
    int pir = 0;
    float temp = 0.0f;
    float humi = 0.0f;

    app_state_copy(g_ctx.state, &copy);
    snapshot_get_pir_level(&copy.last_snapshot, &pir);
    snapshot_get_sht20(&copy.last_snapshot, &temp, &humi);

    snprintf(body, sizeof(body),
             "{"
             "\"mode\":\"%s\","
             "\"pir\":%d,"
             "\"temperature\":%.2f,"
             "\"humidity\":%.2f,"
             "\"last_image\":\"%s\","
             "\"last_event_time\":\"%s\","
             "\"last_capture_ok\":%d,"
             "\"snapshot_seq\":%llu"
             "}",
             app_mode_to_str(copy.mode),
             pir,
             temp,
             humi,
             copy.last_image_path,
             copy.last_event_time,
             copy.last_capture_ok,
             (unsigned long long)copy.last_snapshot.seq);

    send_json(fd, 200, "OK", body);
}

static void handle_mode(int fd, const char *raw_target)
{
    app_mode_t mode;

    if (strstr(raw_target, "value=monitor")) {
        mode = APP_MODE_MONITOR;
    } else if (strstr(raw_target, "value=trigger")) {
        mode = APP_MODE_TRIGGER;
    } else {
        send_json(fd, 400, "Bad Request",
                  "{\"error\":\"use /api/mode?value=monitor or trigger\"}");
        return;
    }

    app_state_set_mode(g_ctx.state, mode);

    if (mode == APP_MODE_MONITOR) {
        send_json(fd, 200, "OK", "{\"mode\":\"monitor\"}");
    } else {
        send_json(fd, 200, "OK", "{\"mode\":\"trigger\"}");
    }
}

static void handle_logs(int fd)
{
    FILE *fp;
    char body[8192];
    size_t used = 0;
    char line[512];

    fp = fopen(g_ctx.log_path, "r");
    if (!fp) {
        send_text(fd, 200, "OK", "");
        return;
    }

    body[0] = '\0';

    while (fgets(line, sizeof(line), fp)) {
        size_t len = strlen(line);
        if (used + len + 1 >= sizeof(body)) {
            break;
        }
        memcpy(body + used, line, len);
        used += len;
        body[used] = '\0';
    }

    fclose(fp);
    send_text(fd, 200, "OK", body);
}

static int wait_and_copy_next_jpeg(uint64_t *last_seq,
                                   unsigned char **local_buf,
                                   size_t *local_cap,
                                   size_t *out_len)
{
    int rc = -1;

    pthread_mutex_lock(&g_mjpeg.lock);

    while (g_http_running) {
        if (app_state_get_mode(g_ctx.state) != APP_MODE_MONITOR) {
            rc = 1;   /* mode changed, caller should close stream */
            goto out;
        }

        if (g_mjpeg.valid && g_mjpeg.seq != *last_seq) {
            break;
        }

        pthread_cond_wait(&g_mjpeg.cond, &g_mjpeg.lock);
    }

    if (!g_http_running) {
        goto out;
    }

    if (*local_cap < g_mjpeg.len) {
        unsigned char *new_buf = realloc(*local_buf, g_mjpeg.len);
        if (!new_buf) {
            goto out;
        }
        *local_buf = new_buf;
        *local_cap = g_mjpeg.len;
    }

    memcpy(*local_buf, g_mjpeg.buf, g_mjpeg.len);
    *out_len = g_mjpeg.len;
    *last_seq = g_mjpeg.seq;
    rc = 0;

out:
    pthread_mutex_unlock(&g_mjpeg.lock);
    return rc;
}

static void handle_mjpeg_stream(int fd)
{
    uint64_t last_seq = 0;
    unsigned char *local_buf = NULL;
    size_t local_cap = 0;

    {
        const char *header =
            "HTTP/1.1 200 OK\r\n"
            "Cache-Control: no-cache\r\n"
            "Pragma: no-cache\r\n"
            "Connection: close\r\n"
            "Content-Type: multipart/x-mixed-replace; boundary=" MJPEG_BOUNDARY "\r\n"
            "\r\n";
        if (send_all(fd, header, strlen(header)) < 0) {
            return;
        }
    }

    while (g_http_running) {
        char part_header[256];
        size_t jpeg_len = 0;
        int n;
        int ret;

        ret = wait_and_copy_next_jpeg(&last_seq, &local_buf, &local_cap, &jpeg_len);
        if (ret < 0) {
            break;
        }
        if (ret > 0) {
            /* mode switched away from monitor */
            break;
        }

        n = snprintf(part_header, sizeof(part_header),
                     "--%s\r\n"
                     "Content-Type: image/jpeg\r\n"
                     "Content-Length: %zu\r\n"
                     "\r\n",
                     MJPEG_BOUNDARY, jpeg_len);
        if (n <= 0) {
            break;
        }

        if (send_all(fd, part_header, (size_t)n) < 0) {
            break;
        }
        if (send_all(fd, local_buf, jpeg_len) < 0) {
            break;
        }
        if (send_all(fd, "\r\n", 2) < 0) {
            break;
        }
    }

    free(local_buf);
}

static void handle_client(int client_fd)
{
    char req[4096];
    char method[16];
    char raw_target[1024];
    char path[1024];
    int n;

    memset(req, 0, sizeof(req));
    memset(method, 0, sizeof(method));
    memset(raw_target, 0, sizeof(raw_target));
    memset(path, 0, sizeof(path));

    n = read(client_fd, req, sizeof(req) - 1);
    if (n <= 0) {
        close(client_fd);
        return;
    }

    if (sscanf(req, "%15s %1023s", method, raw_target) != 2) {
        send_json(client_fd, 400, "Bad Request", "{\"error\":\"bad request\"}");
        close(client_fd);
        return;
    }

    snprintf(path, sizeof(path), "%s", raw_target);
    strip_query_string(path);

    if (strcmp(method, "GET") == 0 && strcmp(path, "/") == 0) {
        send_file_path(client_fd, WEB_INDEX_PATH);
    } else if (strcmp(method, "GET") == 0 && strcmp(path, "/stream.mjpg") == 0) {
        handle_mjpeg_stream(client_fd);
        close(client_fd);
        return;
    } else if (strcmp(method, "GET") == 0 && strncmp(path, "/api/status", 11) == 0) {
        handle_status(client_fd);
    } else if (strcmp(method, "POST") == 0 && strncmp(path, "/api/mode", 9) == 0) {
        handle_mode(client_fd, raw_target);
    } else if (strcmp(method, "GET") == 0 && strncmp(path, "/api/logs", 9) == 0) {
        handle_logs(client_fd);
    } else if (strcmp(method, "GET") == 0 &&
               strncmp(path, "/output/", 8) == 0 &&
               is_safe_path(path)) {
        char local_path[1024];

        if (strlen(path) + 2 > sizeof(local_path)) {
            send_json(client_fd, 400, "Bad Request", "{\"error\":\"path too long\"}");
        } else {
            snprintf(local_path, sizeof(local_path), ".%s", path);
            send_file_path(client_fd, local_path);
        }
    } else {
        send_json(client_fd, 404, "Not Found", "{\"error\":\"not found\"}");
    }

    close(client_fd);
}

static void *client_thread_main(void *arg)
{
    client_arg_t *carg = (client_arg_t *)arg;
    int client_fd;

    if (!carg) {
        return NULL;
    }

    client_fd = carg->client_fd;
    free(carg);

    handle_client(client_fd);
    return NULL;
}

static void *http_thread_main(void *arg)
{
    int server_fd;
    struct sockaddr_in addr;

    (void)arg;

    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("socket");
        return NULL;
    }

    {
        int opt = 1;
        setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    }

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)g_ctx.port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(server_fd);
        return NULL;
    }

    if (listen(server_fd, 16) < 0) {
        perror("listen");
        close(server_fd);
        return NULL;
    }

    printf("[INFO] http server listening on 0.0.0.0:%d\n", g_ctx.port);

    while (g_http_running) {
        fd_set rfds;
        struct timeval tv;
        int ret;

        FD_ZERO(&rfds);
        FD_SET(server_fd, &rfds);

        tv.tv_sec = 0;
        tv.tv_usec = 200000;

        ret = select(server_fd + 1, &rfds, NULL, NULL, &tv);
        if (ret < 0) {
            if (errno == EINTR) {
                continue;
            }
            perror("select");
            continue;
        }

        if (ret == 0) {
            continue;
        }

        if (FD_ISSET(server_fd, &rfds)) {
            int client_fd = accept(server_fd, NULL, NULL);
            if (client_fd < 0) {
                if (errno == EINTR) {
                    continue;
                }
                perror("accept");
                continue;
            }

            {
                pthread_t tid;
                client_arg_t *carg = malloc(sizeof(*carg));
                if (!carg) {
                    close(client_fd);
                    continue;
                }

                carg->client_fd = client_fd;

                if (pthread_create(&tid, NULL, client_thread_main, carg) != 0) {
                    close(client_fd);
                    free(carg);
                    continue;
                }

                pthread_detach(tid);
            }
        }
    }

    close(server_fd);
    return NULL;
}

int http_server_publish_jpeg(const void *jpeg, size_t len)
{
    if (!jpeg || len == 0) {
        return -1;
    }

    pthread_mutex_lock(&g_mjpeg.lock);

    if (g_mjpeg.cap < len) {
        unsigned char *new_buf = realloc(g_mjpeg.buf, len);
        if (!new_buf) {
            pthread_mutex_unlock(&g_mjpeg.lock);
            return -1;
        }
        g_mjpeg.buf = new_buf;
        g_mjpeg.cap = len;
    }

    memcpy(g_mjpeg.buf, jpeg, len);
    g_mjpeg.len = len;
    g_mjpeg.valid = 1;
    g_mjpeg.seq++;

    pthread_cond_broadcast(&g_mjpeg.cond);
    pthread_mutex_unlock(&g_mjpeg.lock);
    return 0;
}

void http_server_clear_mjpeg(void)
{
    pthread_mutex_lock(&g_mjpeg.lock);
    g_mjpeg.valid = 0;
    g_mjpeg.len = 0;
    g_mjpeg.seq++;
    pthread_cond_broadcast(&g_mjpeg.cond);
    pthread_mutex_unlock(&g_mjpeg.lock);
}

int http_server_start(http_server_ctx_t *ctx)
{
    if (!ctx || !ctx->state || !ctx->log_path) {
        return -1;
    }

    g_ctx = *ctx;
    g_http_running = 1;

    if (pthread_create(&g_http_thread, NULL, http_thread_main, NULL) != 0) {
        g_http_running = 0;
        return -1;
    }

    return 0;
}

void http_server_stop(void)
{
    if (!g_http_running) {
        return;
    }

    g_http_running = 0;

    pthread_mutex_lock(&g_mjpeg.lock);
    pthread_cond_broadcast(&g_mjpeg.cond);
    pthread_mutex_unlock(&g_mjpeg.lock);

    pthread_join(g_http_thread, NULL);
}