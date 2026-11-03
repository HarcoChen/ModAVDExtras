/*
 * Copyright 2026 qiyinxi
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include "mx_internal.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
static void sleep_ms(int ms) { Sleep((DWORD)ms); }
#else
#include <time.h>
static void sleep_ms(int ms)
{
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}
#endif

#ifndef MODAVD_EXTRAS_VERSION
#define MODAVD_EXTRAS_VERSION "0.0.0"
#endif

uint32_t modavd_api_version(void) { return MODAVD_API_VERSION; }

const char* modavd_version_string(void) { return MODAVD_EXTRAS_VERSION; }

const char* modavd_strerror(int err)
{
    switch (err) {
    case MODAVD_OK: return "ok";
    case MODAVD_E_INVALID: return "invalid argument";
    case MODAVD_E_NOT_FOUND: return "no emulator console on that port";
    case MODAVD_E_NOT_READY: return "frame sharing is not ready";
    case MODAVD_E_RESIZED: return "the shared frame changed size, reopen";
    case MODAVD_E_BUFFER: return "buffer too small";
    case MODAVD_E_IO: return "system call failed";
    case MODAVD_E_UNSUPPORTED: return "not supported";
    default: return "unknown error";
    }
}

/* Parses a decimal port 1..65535 that makes up all of s. */
static int parse_port(const char* s)
{
    if (!s || !*s) {
        return MODAVD_E_INVALID;
    }
    long v = 0;
    for (const char* p = s; *p; ++p) {
        if (!isdigit((unsigned char)*p) || v > 65535) {
            return MODAVD_E_INVALID;
        }
        v = v * 10 + (*p - '0');
    }
    return (v >= 1 && v <= 65535) ? (int)v : MODAVD_E_INVALID;
}

int modavd_parse_console_port(const char* adb_address)
{
    if (!adb_address) {
        return MODAVD_E_INVALID;
    }
    static const char kSerial[] = "emulator-";
    if (strncmp(adb_address, kSerial, sizeof(kSerial) - 1) == 0) {
        return parse_port(adb_address + sizeof(kSerial) - 1);
    }
    const char* colon = strrchr(adb_address, ':');
    if (!colon || colon == adb_address) {
        return MODAVD_E_INVALID;
    }
    int adb_port = parse_port(colon + 1);
    return adb_port > 1 ? adb_port - 1 : MODAVD_E_INVALID;
}

int modavd_open(const modavd_open_params* params, modavd_handle* out)
{
    if (!params || params->size < sizeof(modavd_open_params) || !out) {
        return MODAVD_E_INVALID;
    }
    *out = NULL;
    int port = params->console_port > 0 ? params->console_port : modavd_parse_console_port(params->adb_address);
    if (port <= 0 || port > 65535) {
        return MODAVD_E_INVALID;
    }
    int timeout_ms = params->timeout_ms > 0 ? params->timeout_ms : MX_DEFAULT_TIMEOUT_MS;

    mx_shm shm = { 0 };
    int rc = mx_shm_open(port, &shm);
    if (rc == MODAVD_E_NOT_READY && !(params->flags & MODAVD_OPEN_NO_START)) {
        int console = mx_console_command(port, "screenrecord webrtc start", 3000);
        if (console == MODAVD_E_NOT_FOUND) {
            return MODAVD_E_NOT_FOUND;
        }
        for (int waited = 0; rc == MODAVD_E_NOT_READY && waited < timeout_ms; waited += 50) {
            sleep_ms(50);
            rc = mx_shm_open(port, &shm);
        }
    }
    if (rc != MODAVD_OK) {
        mx_log(MODAVD_LOG_WARN, "port=%d: shared memory unavailable", port);
        return rc;
    }

    struct modavd_instance* inst = (struct modavd_instance*)calloc(1, sizeof(*inst));
    if (!inst) {
        mx_shm_close(&shm);
        return MODAVD_E_IO;
    }
    inst->console_port = port;
    inst->shm = shm;
    const volatile mx_video_info* vi = (const volatile mx_video_info*)shm.view;
    mx_log(MODAVD_LOG_INFO, "open port=%d %ux%u fps=%u frame=%u size=%zu client=%s", port, vi->width, vi->height,
           vi->fps, vi->frame_number, shm.size, params->client_name ? params->client_name : "-");
    *out = inst;
    return MODAVD_OK;
}

void modavd_close(modavd_handle h)
{
    if (!h) {
        return;
    }
    mx_shm_close(&h->shm);
    free(h);
}

uint32_t modavd_capabilities(modavd_handle h) { return h ? MODAVD_CAP_SCREENCAP : 0; }

int modavd_console_port(modavd_handle h) { return h ? h->console_port : MODAVD_E_INVALID; }

static int bytes_per_pixel(uint32_t format)
{
    switch (format) {
    case MODAVD_FMT_BGRA:
    case MODAVD_FMT_RGBA: return 4;
    case MODAVD_FMT_BGR: return 3;
    default: return 0;
    }
}

static void convert_row(const uint8_t* src, uint8_t* dst, uint32_t width, uint32_t format)
{
    switch (format) {
    case MODAVD_FMT_BGRA:
        memcpy(dst, src, (size_t)width * 4);
        break;
    case MODAVD_FMT_RGBA:
        for (uint32_t x = 0; x < width; ++x, src += 4, dst += 4) {
            dst[0] = src[2];
            dst[1] = src[1];
            dst[2] = src[0];
            dst[3] = src[3];
        }
        break;
    case MODAVD_FMT_BGR:
        for (uint32_t x = 0; x < width; ++x, src += 4, dst += 3) {
            dst[0] = src[0];
            dst[1] = src[1];
            dst[2] = src[2];
        }
        break;
    default:
        break;
    }
}

int mx_copy_frame(const mx_shm* shm, uint32_t format, uint32_t flags, void* buf, size_t buf_size,
                  modavd_frame_info* info)
{
    int bpp = bytes_per_pixel(format);
    if (!bpp || !info || info->size < sizeof(modavd_frame_info)) {
        return MODAVD_E_INVALID;
    }
    const volatile mx_video_info* vi = (const volatile mx_video_info*)shm->view;
    uint32_t w = vi->width;
    uint32_t h = vi->height;
    info->width = (int32_t)w;
    info->height = (int32_t)h;
    info->stride = (int32_t)(w * (uint32_t)bpp);
    info->format = format;
    info->frame_number = vi->frame_number;
    info->timestamp_us = vi->ts_us;
    if (w == 0 || h == 0) {
        return MODAVD_E_NOT_READY;
    }
    if (MX_HEADER_SIZE + (uint64_t)w * h * 4 > shm->size) {
        return MODAVD_E_RESIZED;
    }
    if (!buf || buf_size == 0) {
        return MODAVD_OK;
    }
    if (buf_size < (size_t)info->stride * h) {
        return MODAVD_E_BUFFER;
    }

    const uint8_t* src = shm->view + MX_HEADER_SIZE;
    uint8_t* dst = (uint8_t*)buf;
    size_t src_stride = (size_t)w * 4;
    /* No lock on the writer's side: copy again if a new frame was published meanwhile. */
    for (int attempt = 0; attempt < MX_MAX_COPY_ATTEMPTS; ++attempt) {
        uint32_t before = vi->frame_number;
        for (uint32_t y = 0; y < h; ++y) {
            uint32_t sy = (flags & MODAVD_CAPTURE_BOTTOM_UP) ? h - 1 - y : y;
            convert_row(src + (size_t)sy * src_stride, dst + (size_t)y * (size_t)info->stride, w, format);
        }
        uint32_t after = vi->frame_number;
        info->frame_number = after;
        info->timestamp_us = vi->ts_us;
        if (after == before) {
            break;
        }
    }
    return MODAVD_OK;
}

int modavd_capture(modavd_handle h, int display, uint32_t format, uint32_t flags, void* buf, size_t buf_size,
                   modavd_frame_info* info)
{
    if (!h) {
        return MODAVD_E_INVALID;
    }
    if (display != 0) {
        return MODAVD_E_UNSUPPORTED;
    }
    return mx_copy_frame(&h->shm, format, flags, buf, buf_size, info);
}

/* Input arrives in the next version. */
int modavd_touch_down(modavd_handle h, int display, int contact, int x, int y, int pressure)
{
    (void)h, (void)display, (void)contact, (void)x, (void)y, (void)pressure;
    return MODAVD_E_UNSUPPORTED;
}

int modavd_touch_move(modavd_handle h, int display, int contact, int x, int y, int pressure)
{
    (void)h, (void)display, (void)contact, (void)x, (void)y, (void)pressure;
    return MODAVD_E_UNSUPPORTED;
}

int modavd_touch_up(modavd_handle h, int display, int contact)
{
    (void)h, (void)display, (void)contact;
    return MODAVD_E_UNSUPPORTED;
}

int modavd_touch_release_all(modavd_handle h)
{
    (void)h;
    return MODAVD_E_UNSUPPORTED;
}

int modavd_key(modavd_handle h, int display, int android_keycode, int down)
{
    (void)h, (void)display, (void)android_keycode, (void)down;
    return MODAVD_E_UNSUPPORTED;
}

int modavd_input_text(modavd_handle h, const char* utf8)
{
    (void)h, (void)utf8;
    return MODAVD_E_UNSUPPORTED;
}
