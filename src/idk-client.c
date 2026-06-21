/*
 * idk-client.c — Standalone binary for sending overlay frames to idk-overlay
 *
 * Links against libidk-client (from subprojects/idk-client/) for socket IPC.
 * This binary handles CLI parsing, INI config, and frame I/O.
 *
 * Usage:
 *   # Send a single frame from a binary file
 *   idk-client --socket /tmp/idk-overlay-1234 --width 640 --height 480 \
 *              --x 100 --y 100 --id 1 --visible \
 *              frame.bin
 *
 *   # Send from stdin (pipe)
 *   cat frame.bin | idk-client --socket /tmp/idk-overlay-1234 --width 640 \
 *              --height 480 --x 0 --y 0 --id 1
 *
 *   # Loop mode (repeatedly send frame every 33ms ≈ 30fps)
 *   idk-client --socket /tmp/idk-overlay-1234 --loop --width 640 \
 *              --height 480 --x 0 --y 0 --id 1 frame.bin
 *
 *   # Read config from INI file (like imgoverlay)
 *   idk-client --config idkclient.conf
 *
 * Config file format (INI):
 *   [General]
 *   Socket=/tmp/idk-overlay-1234
 *
 *   [Overlay_1]
 *   Width=640
 *   Height=480
 *   X=100
 *   Y=50
 *   Id=1
 *   Visible=true
 *   Source=frame.bin        # or - for stdin
 *   FPS=30                  # loop interval (0 = once)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>

#include "idk_client.h"
#include "idk_log.h"

/* ── Config ───────────────────────────────────────────────────────────── */

typedef struct overlay_config {
    uint32_t id;
    uint32_t x;
    uint32_t y;
    uint32_t width;
    uint32_t height;
    uint8_t  visible;
    char     source[1024];  /* file path or "-" for stdin */
    int      loop_fps;      /* 0 = send once, >0 = loop at this FPS */
} overlay_config_t;

typedef struct {
    char     socket[1024];
    int      num_overlays;
    overlay_config_t overlays[16];
} config_t;

/* ── INI parser (simple, no dependencies) ─────────────────────────────── */

static void trim(char *s) {
    while (*s == ' ' || *s == '\t') s++;
    char *end = s + strlen(s) - 1;
    while (end > s && (*end == ' ' || *end == '\t' || *end == '\n' || *end == '\r'))
        *end-- = '\0';
    memmove(s, s, strlen(s) + 1);
}

static int parse_config_file(const char *path, config_t *cfg) {
    FILE *f = fopen(path, "r");
    if (!f) {
        IDK_ERR("client", "Cannot open config: %s\n", path);
        return -1;
    }

    char line[1024];
    char current_group[256] = "";
    int overlay_idx = -1;

    /* Initialize defaults */
    snprintf(cfg->socket, sizeof(cfg->socket), "/tmp/idk-overlay");
    cfg->num_overlays = 0;

    while (fgets(line, sizeof(line), f)) {
        /* Remove trailing newline */
        line[strcspn(line, "\r\n")] = '\0';
        trim(line);

        /* Skip empty lines and comments */
        if (line[0] == '\0' || line[0] == '#' || line[0] == ';') continue;

        /* Section header */
        if (line[0] == '[') {
            char *end = strchr(line, ']');
            if (end) {
                *end = '\0';
                char *group = line + 1;
                trim(group);
                strcpy(current_group, group);

                /* Parse overlay group name: "Overlay_N" or "Overlay_1" */
                if (strncmp(group, "Overlay_", 8) == 0) {
                    int id = atoi(group + 8);
                    if (id < 1 || id > 16) {
                        IDK_ERR("client", "Invalid overlay ID: %d\n", id);
                        fclose(f);
                        return -1;
                    }
                    overlay_idx = id - 1;
                    if (overlay_idx >= cfg->num_overlays) {
                        cfg->num_overlays = overlay_idx + 1;
                    }
                    memset(&cfg->overlays[overlay_idx], 0, sizeof(overlay_config_t));
                    cfg->overlays[overlay_idx].id = (uint32_t)id;
                    cfg->overlays[overlay_idx].loop_fps = 0;
                }
            }
            continue;
        }

        /* Key=value */
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        char *key = line;
        char *val = eq + 1;
        trim(key);
        trim(val);

        if (strcmp(key, "Socket") == 0) {
            snprintf(cfg->socket, sizeof(cfg->socket), "%s", val);
        } else if (overlay_idx >= 0) {
            overlay_config_t *o = &cfg->overlays[overlay_idx];
            if (strcmp(key, "Width") == 0) o->width = (uint32_t)atoi(val);
            else if (strcmp(key, "Height") == 0) o->height = (uint32_t)atoi(val);
            else if (strcmp(key, "X") == 0) o->x = (uint32_t)atoi(val);
            else if (strcmp(key, "Y") == 0) o->y = (uint32_t)atoi(val);
            else if (strcmp(key, "Id") == 0) o->id = (uint32_t)atoi(val);
            else if (strcmp(key, "Visible") == 0) o->visible = (strcmp(val, "true") == 0 || strcmp(val, "1") == 0) ? 1 : 0;
            else if (strcmp(key, "Source") == 0) snprintf(o->source, sizeof(o->source), "%s", val);
            else if (strcmp(key, "FPS") == 0) o->loop_fps = atoi(val);
        }
    }

    fclose(f);
    return 0;
}

/* ── Read file to buffer ──────────────────────────────────────────────── */

static void *read_file_to_buffer(const char *path, size_t *out_size) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        IDK_ERR("client", "Cannot open %s: %s\n", path, strerror(errno));
        return NULL;
    }

    struct stat st;
    if (fstat(fd, &st) < 0) {
        close(fd);
        return NULL;
    }
    *out_size = st.st_size;

    void *buf = malloc(st.st_size);
    if (!buf) {
        close(fd);
        return NULL;
    }

    ssize_t n = read(fd, buf, (size_t)st.st_size);
    close(fd);
    if ((size_t)n != (size_t)st.st_size) {
        IDK_ERR("client", "Read %zd/%zu bytes\n", n, (size_t)st.st_size);
        free(buf);
        return NULL;
    }

    return buf;
}

/* ── Read from stdin ──────────────────────────────────────────────────── */

static void *read_stdin_to_buffer(size_t *out_size) {
    /* Read until EOF, doubling buffer as needed */
    size_t cap = 4096;
    size_t used = 0;
    uint8_t *buf = malloc(cap);
    if (!buf) return NULL;

    size_t chunk;
    while ((chunk = fread(buf + used, 1, cap - used, stdin)) > 0) {
        used += chunk;
        if (used == cap) {
            cap *= 2;
            uint8_t *tmp = realloc(buf, cap);
            if (!tmp) { free(buf); return NULL; }
            buf = tmp;
        }
    }

    *out_size = used;
    return buf;
}

/* ── Send one frame from config ───────────────────────────────────────── */

static int send_overlay_frame(overlay_config_t *o) {
    if (o->width == 0 || o->height == 0) {
        IDK_ERR("client", "Overlay %u: width/height not set\n", o->id);
        return -1;
    }

    size_t size = 0;
    void *pixels = NULL;

    if (strcmp(o->source, "-") == 0) {
        /* Read from stdin */
        pixels = read_stdin_to_buffer(&size);
        if (!pixels) {
            IDK_ERR("client", "Failed to read from stdin\n");
            return -1;
        }
        IDK_LOG("client", "Read %zu bytes from stdin\n", size);
    } else {
        pixels = read_file_to_buffer(o->source, &size);
        if (!pixels) {
            IDK_ERR("client", "Cannot read %s\n", o->source);
            return -1;
        }
        IDK_LOG("client", "Read %zu bytes from %s\n", size, o->source);
    }

    /* Validate size matches expected */
    size_t expected = (size_t)o->width * (size_t)o->height * 4;
    if (size < expected) {
        IDK_ERR("client", "File size %zu < expected %zu (%dx%d*4)\n",
                size, expected, o->width, o->height);
    }

    /* Send frame */
    idk_client_frame_t frame = {
        .width    = o->width,
        .height   = o->height,
        .x        = o->x,
        .y        = o->y,
        .id       = (uint8_t)o->id,
        .visible  = (uint8_t)o->visible,
        .nfd      = 1,
        .type     = IDK_FRAME_TYPE_SHM,
    };

    int rc = idk_client_send_pixels(pixels, &frame);
    free(pixels);
    return rc;
}

/* ── Usage ────────────────────────────────────────────────────────────── */

static void usage(const char *prog) {
    fprintf(stderr,
        "Usage: %s [options]\n"
        "\n"
        "Single frame mode:\n"
        "  %s --socket PATH --width W --height H --x X --y Y --id N \\\n"
        "       [--visible] [--loop] FRAMEFILE\n"
        "\n"
        "Config file mode:\n"
        "  %s --config config.ini\n"
        "\n"
        "Options:\n"
        "  --socket PATH   Socket path (default: /tmp/idk-overlay)\n"
        "  --config FILE   Read overlay config from INI file\n"
        "  --width W       Frame width in pixels\n"
        "  --height H      Frame height in pixels\n"
        "  --x X           Overlay X position\n"
        "  --y Y           Overlay Y position\n"
        "  --id N          Overlay ID (1-based)\n"
        "  --visible       Make overlay visible\n"
        "  --loop          Repeatedly send frame (default FPS: 30)\n"
        "  --fps N         Loop FPS (default: 30)\n"
        "  --format FMT    Frame format: abgr8888 (default), bgra8888\n"
        "  -h, --help      Show this help\n"
        "\n"
        "Config file format (INI):\n"
        "  [General]\n"
        "  Socket=/tmp/idk-overlay-1234\n"
        "\n"
        "  [Overlay_1]\n"
        "  Width=640\n"
        "  Height=480\n"
        "  X=100\n"
        "  Y=50\n"
        "  Id=1\n"
        "  Visible=true\n"
        "  Source=frame.bin        (use - for stdin)\n"
        "  FPS=30\n",
        prog, prog, prog);
}

/* ── Main ─────────────────────────────────────────────────────────────── */

int main(int argc, char **argv) {
    const char *config_path = NULL;
    const char *sockpath = NULL;
    uint32_t width = 0, height = 0, x = 0, y = 0, id = 1;
    uint8_t visible = 1;
    int loop = 0;
    int loop_fps = 30;
    const char *frame_file = NULL;
    int single_overlay = 0;

    for (int i = 1; i < argc; i++) {
        if ((strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0)) {
            usage(argv[0]);
            return 0;
        } else if (strcmp(argv[i], "--socket") == 0 && i + 1 < argc) {
            sockpath = argv[++i];
        } else if (strcmp(argv[i], "--config") == 0 && i + 1 < argc) {
            config_path = argv[++i];
        } else if (strcmp(argv[i], "--width") == 0 && i + 1 < argc) {
            width = (uint32_t)atoi(argv[++i]);
            single_overlay = 1;
        } else if (strcmp(argv[i], "--height") == 0 && i + 1 < argc) {
            height = (uint32_t)atoi(argv[++i]);
            single_overlay = 1;
        } else if (strcmp(argv[i], "--x") == 0 && i + 1 < argc) {
            x = (uint32_t)atoi(argv[++i]);
            single_overlay = 1;
        } else if (strcmp(argv[i], "--y") == 0 && i + 1 < argc) {
            y = (uint32_t)atoi(argv[++i]);
            single_overlay = 1;
        } else if (strcmp(argv[i], "--id") == 0 && i + 1 < argc) {
            id = (uint32_t)atoi(argv[++i]);
            single_overlay = 1;
        } else if (strcmp(argv[i], "--visible") == 0) {
            visible = 1;
            single_overlay = 1;
        } else if (strcmp(argv[i], "--loop") == 0) {
            loop = 1;
        } else if (strcmp(argv[i], "--fps") == 0 && i + 1 < argc) {
            loop_fps = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--format") == 0 && i + 1 < argc) {
            (void)argv[++i]; /* Format selection reserved for future use */
        } else if (frame_file == NULL && argv[i][0] != '-') {
            frame_file = argv[i];
            single_overlay = 1;
        }
    }

    config_t cfg;
    memset(&cfg, 0, sizeof(cfg));

    /* Mode 1: Config file */
    if (config_path) {
        if (parse_config_file(config_path, &cfg) < 0) {
            IDK_ERR("client", "Failed to parse config: %s\n", config_path);
            return 1;
        }
        sockpath = cfg.socket;
    }
    /* Mode 2: CLI args */
    else if (single_overlay) {
        if (!sockpath) sockpath = "/tmp/idk-overlay";
        if (!frame_file) {
            IDK_ERR("client", "No frame file specified\n");
            usage(argv[0]);
            return 1;
        }

        cfg.overlays[0] = (overlay_config_t){
            .id = id,
            .x = x, .y = y,
            .width = width, .height = height,
            .visible = visible,
            .loop_fps = loop ? loop_fps : 0,
        };
        snprintf(cfg.overlays[0].source, sizeof(cfg.overlays[0].source), "%s", frame_file);
        snprintf(cfg.socket, sizeof(cfg.socket), "%s", sockpath);
        cfg.num_overlays = 1;
    } else {
        usage(argv[0]);
        return 1;
    }

    /* Connect to socket */
    if (idk_client_init(cfg.socket, 0) < 0) {
        IDK_ERR("client", "Failed to connect to %s\n", cfg.socket);
        return 1;
    }

    IDK_LOG("client",
            "Sending %d overlay(s) to %s\n",
            cfg.num_overlays, cfg.socket);

    /* Send frames (loop or single) */
    for (int o = 0; o < cfg.num_overlays; o++) {
        overlay_config_t *overlay = &cfg.overlays[o];
        int rc;

        if (overlay->loop_fps > 0) {
            int interval_ms = 1000 / overlay->loop_fps;
            IDK_LOG("client",
                    "Overlay %u: looping at %d FPS (%dms interval)\n",
                    overlay->id, overlay->loop_fps, interval_ms);

            while (1) {
                rc = send_overlay_frame(overlay);
                if (rc < 0) {
                    IDK_ERR("client", "Send failed\n");
                    break;
                }
                usleep(interval_ms * 1000);
            }
        } else {
            rc = send_overlay_frame(overlay);
            if (rc < 0) {
                IDK_ERR("client", "Send failed\n");
                idk_client_shutdown();
                return 1;
            }
        }
    }

    idk_client_shutdown();
    IDK_LOG("client", "Done.\n");
    return 0;
}
