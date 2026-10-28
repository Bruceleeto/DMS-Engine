#pragma once
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
typedef int file_t;
#define FILEHND_INVALID (-1)
static inline file_t fs_open(const char* path, int mode) { return open(path, mode, 0644); }
#define fs_close close
#define fs_read read
#define fs_write write
static inline ssize_t fs_total(file_t fd) {
    struct stat st;
    return fstat(fd, &st) == 0 ? st.st_size : -1;
}
