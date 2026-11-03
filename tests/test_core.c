/*
 * Library tests that need no emulator: address parsing, error codes, and
 * capture from a fake shared frame in every format.
 *
 * Copyright 2026 qiyinxi
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fake_frame.h"
#include "modavd_extras.h"

static int g_failures;

#define CHECK(cond)                                                                                                   \
    do {                                                                                                              \
        if (!(cond)) {                                                                                                \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);                                  \
            ++g_failures;                                                                                             \
        }                                                                                                             \
    } while (0)

#define CHECK_EQ(a, b)                                                                                                \
    do {                                                                                                              \
        long long va_ = (long long)(a), vb_ = (long long)(b);                                                         \
        if (va_ != vb_) {                                                                                             \
            fprintf(stderr, "%s:%d: CHECK_EQ failed: %s = %lld, %s = %lld\n", __FILE__, __LINE__, #a, va_, #b, vb_);  \
            ++g_failures;                                                                                             \
        }                                                                                                             \
    } while (0)

static void test_parse_console_port(void)
{
    CHECK_EQ(modavd_parse_console_port("emulator-20010"), 20010);
    CHECK_EQ(modavd_parse_console_port("emulator-5554"), 5554);
    CHECK_EQ(modavd_parse_console_port("127.0.0.1:20011"), 20010);
    CHECK_EQ(modavd_parse_console_port("localhost:5555"), 5554);
    CHECK_EQ(modavd_parse_console_port("[::1]:5555"), 5554);
    CHECK_EQ(modavd_parse_console_port(NULL), MODAVD_E_INVALID);
    CHECK_EQ(modavd_parse_console_port(""), MODAVD_E_INVALID);
    CHECK_EQ(modavd_parse_console_port("emulator-"), MODAVD_E_INVALID);
    CHECK_EQ(modavd_parse_console_port("emulator-70000"), MODAVD_E_INVALID);
    CHECK_EQ(modavd_parse_console_port("emulator-12ab"), MODAVD_E_INVALID);
    CHECK_EQ(modavd_parse_console_port("127.0.0.1:"), MODAVD_E_INVALID);
    CHECK_EQ(modavd_parse_console_port("127.0.0.1:1"), MODAVD_E_INVALID);
    CHECK_EQ(modavd_parse_console_port(":5555"), MODAVD_E_INVALID);
    CHECK_EQ(modavd_parse_console_port("R5CT20ABCDE"), MODAVD_E_INVALID);
}

static void test_version_and_errors(void)
{
    CHECK_EQ(modavd_api_version(), MODAVD_API_VERSION);
    CHECK(strlen(modavd_version_string()) > 0);
    CHECK(strcmp(modavd_strerror(MODAVD_OK), "ok") == 0);
    CHECK(modavd_strerror(-1234) != NULL);
    CHECK_EQ(modavd_capabilities(NULL), 0);
    CHECK_EQ(modavd_console_port(NULL), MODAVD_E_INVALID);
}

static void test_open_errors(void)
{
    modavd_handle h = (modavd_handle)(uintptr_t)1; /* must be reset to NULL on failure */
    CHECK_EQ(modavd_open(NULL, &h), MODAVD_E_INVALID);

    modavd_open_params p;
    memset(&p, 0, sizeof(p));
    p.size = sizeof(p);
    CHECK_EQ(modavd_open(&p, &h), MODAVD_E_INVALID); /* neither address nor port */
    CHECK(h == NULL);
    p.adb_address = "R5CT20ABCDE"; /* a phone's serial: no console port in it */
    CHECK_EQ(modavd_open(&p, &h), MODAVD_E_INVALID);
    p.adb_address = NULL;

    p.size = 4; /* a caller from a future version would never send less */
    p.console_port = 65010;
    CHECK_EQ(modavd_open(&p, &h), MODAVD_E_INVALID);

    /* No frame on this port and starting it is not allowed. */
    p.size = sizeof(p);
    p.flags = MODAVD_OPEN_NO_START;
    CHECK_EQ(modavd_open(&p, &h), MODAVD_E_NOT_READY);

    /* No frame, and no console to start it: normally nothing listens here (NOT_FOUND);
     * if something unrelated does, it will not answer like a console (NOT_READY). */
    p.flags = 0;
    p.console_port = 65020;
    p.timeout_ms = 200;
    int rc = modavd_open(&p, &h);
    CHECK(rc == MODAVD_E_NOT_FOUND || rc == MODAVD_E_NOT_READY);
    CHECK(h == NULL);
}

static uint8_t expected_channel(uint32_t format, int channel, uint32_t x, uint32_t y)
{
    uint8_t b = (uint8_t)x, g = (uint8_t)y, r = (uint8_t)(x ^ y);
    switch (format) {
    case MODAVD_FMT_BGRA: return (uint8_t[]){ b, g, r, 0xFF }[channel];
    case MODAVD_FMT_RGBA: return (uint8_t[]){ r, g, b, 0xFF }[channel];
    default: return (uint8_t[]){ b, g, r }[channel];
    }
}

static void check_capture(modavd_handle h, uint32_t format, int bpp, uint32_t flags, uint32_t w, uint32_t hgt)
{
    modavd_frame_info info;
    memset(&info, 0, sizeof(info));
    info.size = sizeof(info);
    size_t size = (size_t)w * hgt * (size_t)bpp;
    uint8_t* buf = (uint8_t*)malloc(size);
    CHECK_EQ(modavd_capture(h, 0, format, flags, buf, size, &info), MODAVD_OK);
    CHECK_EQ(info.width, w);
    CHECK_EQ(info.height, hgt);
    CHECK_EQ(info.stride, w * (uint32_t)bpp);
    CHECK_EQ(info.format, format);
    CHECK_EQ(info.frame_number, 7);
    CHECK_EQ(info.timestamp_us, 123456789ull);
    int bad = 0;
    for (uint32_t row = 0; row < hgt && !bad; ++row) {
        uint32_t y = (flags & MODAVD_CAPTURE_BOTTOM_UP) ? hgt - 1 - row : row;
        for (uint32_t x = 0; x < w && !bad; ++x) {
            for (int c = 0; c < bpp; ++c) {
                if (buf[(size_t)row * info.stride + (size_t)x * bpp + c] != expected_channel(format, c, x, y)) {
                    fprintf(stderr, "format %u flags %u: pixel (%u,%u) channel %d differs\n", format, flags, x, y, c);
                    bad = 1;
                    break;
                }
            }
        }
    }
    CHECK(!bad);
    free(buf);
}

static void test_capture_from_fake_frame(void)
{
    const int port = 65001;
    const uint32_t w = 64, hgt = 32;
    fake_frame f;
    if (fake_frame_create(&f, port, w, hgt, w, hgt) != 0) {
        fprintf(stderr, "cannot create the fake frame\n");
        ++g_failures;
        return;
    }

    modavd_open_params p;
    memset(&p, 0, sizeof(p));
    p.size = sizeof(p);
    p.adb_address = "127.0.0.1:65002"; /* adb port = console + 1 */
    p.flags = MODAVD_OPEN_NO_START;
    p.client_name = "test_core";
    modavd_handle h = NULL;
    CHECK_EQ(modavd_open(&p, &h), MODAVD_OK);
    if (!h) {
        fake_frame_destroy(&f);
        return;
    }
    CHECK_EQ(modavd_console_port(h), port);
    CHECK_EQ(modavd_capabilities(h), MODAVD_CAP_SCREENCAP);

    check_capture(h, MODAVD_FMT_BGRA, 4, 0, w, hgt);
    check_capture(h, MODAVD_FMT_BGR, 3, 0, w, hgt);
    check_capture(h, MODAVD_FMT_RGBA, 4, 0, w, hgt);
    check_capture(h, MODAVD_FMT_RGBA, 4, MODAVD_CAPTURE_BOTTOM_UP, w, hgt);
    check_capture(h, MODAVD_FMT_BGR, 3, MODAVD_CAPTURE_BOTTOM_UP, w, hgt);

    modavd_frame_info info;
    memset(&info, 0, sizeof(info));
    info.size = sizeof(info);
    /* size probe */
    CHECK_EQ(modavd_capture(h, 0, MODAVD_FMT_BGR, 0, NULL, 0, &info), MODAVD_OK);
    CHECK_EQ(info.stride * info.height, w * 3 * hgt);
    /* small buffer */
    uint8_t small[16];
    CHECK_EQ(modavd_capture(h, 0, MODAVD_FMT_BGR, 0, small, sizeof(small), &info), MODAVD_E_BUFFER);
    CHECK_EQ(info.width, w);
    /* bad arguments */
    CHECK_EQ(modavd_capture(h, 1, MODAVD_FMT_BGR, 0, NULL, 0, &info), MODAVD_E_UNSUPPORTED);
    CHECK_EQ(modavd_capture(h, 0, 99, 0, NULL, 0, &info), MODAVD_E_INVALID);
    CHECK_EQ(modavd_capture(h, 0, MODAVD_FMT_BGR, 0, NULL, 0, NULL), MODAVD_E_INVALID);
    info.size = 4;
    CHECK_EQ(modavd_capture(h, 0, MODAVD_FMT_BGR, 0, NULL, 0, &info), MODAVD_E_INVALID);
    info.size = sizeof(info);
    CHECK_EQ(modavd_capture(NULL, 0, MODAVD_FMT_BGR, 0, NULL, 0, &info), MODAVD_E_INVALID);

    /* nothing composed yet */
    fake_frame_set_size(&f, 0, 0);
    CHECK_EQ(modavd_capture(h, 0, MODAVD_FMT_BGR, 0, NULL, 0, &info), MODAVD_E_NOT_READY);
    /* the display grew past the region */
    fake_frame_set_size(&f, w * 4, hgt * 4);
    CHECK_EQ(modavd_capture(h, 0, MODAVD_FMT_BGR, 0, NULL, 0, &info), MODAVD_E_RESIZED);
    fake_frame_set_size(&f, w, hgt);
    CHECK_EQ(modavd_capture(h, 0, MODAVD_FMT_BGR, 0, NULL, 0, &info), MODAVD_OK);

    /* input is not in this version */
    CHECK_EQ(modavd_touch_down(h, 0, 0, 1, 1, 1), MODAVD_E_UNSUPPORTED);
    CHECK_EQ(modavd_touch_release_all(h), MODAVD_E_UNSUPPORTED);
    CHECK_EQ(modavd_key(h, 0, 4, 1), MODAVD_E_UNSUPPORTED);
    CHECK_EQ(modavd_input_text(h, "a"), MODAVD_E_UNSUPPORTED);

    modavd_close(h);
    modavd_close(NULL);
    fake_frame_destroy(&f);
}

static int g_log_lines;
static void count_log(int level, const char* msg, void* user)
{
    (void)level;
    (void)msg;
    (void)user;
    ++g_log_lines;
}

static void test_log_callback(void)
{
    modavd_set_log(count_log, NULL);
    modavd_open_params p;
    memset(&p, 0, sizeof(p));
    p.size = sizeof(p);
    p.console_port = 65011;
    p.flags = MODAVD_OPEN_NO_START;
    modavd_handle h = NULL;
    CHECK_EQ(modavd_open(&p, &h), MODAVD_E_NOT_READY);
    CHECK(g_log_lines > 0);
    modavd_set_log(NULL, NULL);
}

int main(void)
{
    test_parse_console_port();
    test_version_and_errors();
    test_open_errors();
    test_capture_from_fake_frame();
    test_log_callback();
    if (g_failures) {
        fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    printf("test_core: all checks passed\n");
    return 0;
}
