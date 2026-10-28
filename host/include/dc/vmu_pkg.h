#pragma once
#include <stdint.h>
#define VMUPKG_EC_NONE 0
typedef struct vmu_pkg {
    char desc_short[20];
    char desc_long[36];
    char app_id[20];
    int icon_cnt, icon_anim_speed, eyecatch_type, data_len;
    uint16_t icon_pal[16];
    uint8_t* icon_data;
    uint8_t* eyecatch_data;
    const uint8_t* data;
} vmu_pkg_t;
