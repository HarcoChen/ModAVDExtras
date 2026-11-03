/*
 * Copyright 2026 qiyinxi
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */
#include <stdarg.h>
#include <stdio.h>

#include "mx_internal.h"

static modavd_log_fn g_log_fn;
static void* g_log_user;

void modavd_set_log(modavd_log_fn fn, void* user)
{
    g_log_user = user;
    g_log_fn = fn;
}

void mx_log(int level, const char* fmt, ...)
{
    modavd_log_fn fn = g_log_fn;
    if (!fn) {
        return;
    }
    char msg[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    fn(level, msg, g_log_user);
}
