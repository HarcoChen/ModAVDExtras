/*
 * The emulator's shared frame: a named shared memory region per console port,
 * created by `screenrecord webrtc start` (or -share-vid). Opened read-only.
 *
 * Copyright 2026 qiyinxi
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */
#include <stdio.h>
#include <string.h>

#include "mx_internal.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <errno.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

void mx_shm_name(int port, char* buf, size_t cap)
{
#ifdef _WIN32
    /* The emulator's SharedMemory_win32 prefixes the handle with "SHM_". */
    snprintf(buf, cap, "SHM_videmulator%d", port);
#else
    /* SharedMemory_posix passes the handle to shm_open() as is. */
    snprintf(buf, cap, "videmulator%d", port);
#endif
}

#ifdef _WIN32

int mx_shm_open(int port, mx_shm* out)
{
    char name[64];
    wchar_t wname[64];
    mx_shm_name(port, name, sizeof(name));
    for (size_t i = 0;; ++i) {
        wname[i] = (wchar_t)(unsigned char)name[i];
        if (!name[i]) {
            break;
        }
    }

    HANDLE h = OpenFileMappingW(FILE_MAP_READ, FALSE, wname);
    if (!h) {
        DWORD err = GetLastError();
        if (err == ERROR_FILE_NOT_FOUND) {
            return MODAVD_E_NOT_READY;
        }
        mx_log(MODAVD_LOG_WARN, "open %s failed (%lu)", name, (unsigned long)err);
        return MODAVD_E_IO;
    }
    const uint8_t* view = (const uint8_t*)MapViewOfFile(h, FILE_MAP_READ, 0, 0, 0);
    if (!view) {
        mx_log(MODAVD_LOG_WARN, "map %s failed (%lu)", name, (unsigned long)GetLastError());
        CloseHandle(h);
        return MODAVD_E_IO;
    }
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery(view, &mbi, sizeof(mbi))) {
        UnmapViewOfFile(view);
        CloseHandle(h);
        return MODAVD_E_IO;
    }
    out->handle = h;
    out->view = view;
    out->size = mbi.RegionSize;
    return MODAVD_OK;
}

void mx_shm_close(mx_shm* shm)
{
    if (shm->view) {
        UnmapViewOfFile(shm->view);
    }
    if (shm->handle) {
        CloseHandle((HANDLE)shm->handle);
    }
    memset(shm, 0, sizeof(*shm));
}

#else

int mx_shm_open(int port, mx_shm* out)
{
    char name[64];
    mx_shm_name(port, name, sizeof(name));
    int fd = shm_open(name, O_RDONLY, 0);
    if (fd < 0) {
        if (errno == ENOENT) {
            return MODAVD_E_NOT_READY;
        }
        mx_log(MODAVD_LOG_WARN, "open %s failed (%d)", name, errno);
        return MODAVD_E_IO;
    }
    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size <= 0) {
        close(fd);
        return MODAVD_E_IO;
    }
    void* view = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_SHARED, fd, 0);
    close(fd);
    if (view == MAP_FAILED) {
        mx_log(MODAVD_LOG_WARN, "map %s failed (%d)", name, errno);
        return MODAVD_E_IO;
    }
    out->handle = NULL;
    out->view = (const uint8_t*)view;
    out->size = (size_t)st.st_size;
    return MODAVD_OK;
}

void mx_shm_close(mx_shm* shm)
{
    if (shm->view) {
        munmap((void*)shm->view, shm->size);
    }
    memset(shm, 0, sizeof(*shm));
}

#endif
