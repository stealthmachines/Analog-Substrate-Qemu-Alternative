/*
 * hdgl_lattice.h — Multi-Lattice API
 *
 * Supported lattice types:
 *   LATTICE_PHI_TICK_128     — HDGL-fabric v0.2 (128-slot phi_tick recurrence)
 *   LATTICE_WATER_GLYPH_4096 — Chladni basin (our work, phi_pool)
 *   LATTICE_KURAMOTO_8D      — ll_analog 8D Kuramoto (Analog-Prime)
 *   LATTICE_COMPOSITE        — all three concurrent (default)
 */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>

typedef enum {
    LATTICE_PHI_TICK_128     = 0,   /* HDGL-fabric v0.2 */
    LATTICE_WATER_GLYPH_4096 = 1,   /* Chladni water glyph */
    LATTICE_KURAMOTO_8D      = 2,   /* ll_analog */
    LATTICE_COMPOSITE        = 3,   /* all three (default) */
} lattice_type_t;

typedef struct {
    lattice_type_t  type;
    uint32_t        genome_fp;
    uint32_t        tick;
    const char     *slots_dir;
    int             settle_steps;
    int             steps_per_write;
    int             interval_us;
    int             verbose;
} lattice_config_t;

#define LATTICE_CONFIG_DEFAULTS { \
    .type           = LATTICE_COMPOSITE, \
    .genome_fp      = 0x88888888, \
    .tick           = 0, \
    .slots_dir      = "/lattice/slots", \
    .settle_steps   = 200, \
    .steps_per_write = 50, \
    .interval_us    = 20000, \
    .verbose        = 0 \
}

typedef struct {
    uint64_t    phi_tick;
    double      kura_R;
    int         kura_phase;        /* 0=PLUCK 1=SUSTAIN 2=FINETUNE 3=LOCK */
    int         consensus;         /* 1 = APA_FLAG_CONSENSUS set */
    uint32_t    slot_sample[8];    /* first 8 phi_tick slots */
    double      theta[8];          /* Kuramoto phases */
} lattice_status_t;

/* Parsed .hdgl file info */
#define MAX_GLYPH_NAMES 64
typedef struct {
    char            source_path[256];
    lattice_type_t  lattice_type;
    uint32_t        genome_fp;
    double          phi;
    int             version;       /* 2 = v0.2, 3 = our work */
    int             n_glyphs;
    char            glyph_names[MAX_GLYPH_NAMES][64];
} hdgl_glyph_t;

/* Lattice thread control */
int              lattice_start(lattice_config_t *cfg);
void             lattice_stop(void);
lattice_status_t lattice_status(void);
const char      *lattice_type_name(lattice_type_t t);

/* HDGL glyph loader */
hdgl_glyph_t    *lattice_load_hdgl(const char *path);
void             lattice_free_hdgl(hdgl_glyph_t *g);

/* Parse lattice type from string */
static inline lattice_type_t lattice_type_from_str(const char *s) {
    if (!s) return LATTICE_COMPOSITE;
    if (strstr(s,"phi_tick") || strstr(s,"128") || strstr(s,"v0.2"))
        return LATTICE_PHI_TICK_128;
    if (strstr(s,"water") || strstr(s,"chladni") || strstr(s,"4096") || strstr(s,"basin"))
        return LATTICE_WATER_GLYPH_4096;
    if (strstr(s,"kuramoto") || strstr(s,"8d") || strstr(s,"ll_analog"))
        return LATTICE_KURAMOTO_8D;
    return LATTICE_COMPOSITE;
}
