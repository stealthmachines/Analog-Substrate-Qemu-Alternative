/*
 * hdgl_universe.h — Universal HDGL Suite Detection API
 */
#pragma once
#include <stdint.h>
#include "hdgl_lattice.h"

typedef enum {
    SUITE_HDGL_ZERO        = 0,   /* hdgl-zero v0.7           */
    SUITE_HDGL_FABRIC      = 1,   /* HDGL-fabric v0.2         */
    SUITE_HDGL_ROUTER64    = 2,   /* hdgl_router64 v0.4       */
    SUITE_HDGL_GOLDEN_DOME = 3,   /* HDGL-golden-dome v0.1    */
    SUITE_ALT_FABRIC       = 4,   /* Alt Fabric 6.26.26       */
    SUITE_UNKNOWN          = 5,
    SUITE_COUNT            = 6,
} hdgl_suite_t;

typedef struct {
    hdgl_suite_t    suite;
    char            name[64];
    char            version[16];
    lattice_type_t  lattice;
    int             lat4096_slots;   /* 128 or 4096             */
    int             shell_cmds;      /* number of shell commands */
    int             has_nic;
    int             has_genome;
    int             has_radio;       /* analog radio extension  */
    int             settle_steps;
    int             runtime_sectors; /* firmware sectors on disk */
    uint64_t        lat4096_base;    /* LAT4096 phys addr       */
    uint64_t        phi128_base;     /* phi-tick phys addr      */
} hdgl_suite_info_t;

/* API */
hdgl_suite_info_t  universe_detect(const char *image_or_folder);
void               universe_suite_config(hdgl_suite_info_t *info);
lattice_config_t   universe_lattice_config(const hdgl_suite_info_t *info,
                                            uint32_t genome_fp, uint32_t tick,
                                            const char *slots_dir);
void               universe_print_info(const hdgl_suite_info_t *info);
void               universe_emulate_boot(const hdgl_suite_info_t *info,
                                          uint32_t genome_fp);
