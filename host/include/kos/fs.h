#pragma once
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
typedef int file_t;
#define fs_open open
#define fs_close close
#define fs_read read
static inline ssize_t fs_total(file_t fd) {
    struct stat st;
    return fstat(fd, &st) == 0 ? st.st_size : -1;
}
