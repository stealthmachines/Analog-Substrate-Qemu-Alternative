/*
 * phi_substrate.h — HDGL Analog Substrate API
 */
#pragma once
#include <stdint.h>

#define N_DSLOTS    32
#define N_STRANDS    8
#define SQRT_PHI     1.2720196495140690

/* Glyph symmetry classes */
typedef enum {
    SYM_VOID=0, SYM_2FOLD=1, SYM_3FOLD=2,
    SYM_4FOLD=3, SYM_6FOLD=4, SYM_8FOLD=5
} GlyphSym;

/* Shared memory — mirrors hdgl_smp_substrate.asm layout at 0x7000 */
typedef struct {
    uint64_t magic;           /* 0x48444C535542530 "HDGLSUBS" */
    uint32_t status;          /* 0=uninit 1=ready 2=ticking */
    uint32_t tick;            /* basin step counter */
    uint32_t d_bits;          /* 32-bit binary pool (e.g. 0xFFFF0000) */
    uint32_t dn_agg;          /* 8-nibble Dn aggregate */
    double   d_slots[N_DSLOTS]; /* D1..D32 eigenmode amplitudes */
    double   theta[8];        /* Kuramoto phases (0..2π) */
    uint32_t kura_step;       /* Kuramoto integration step */
    double   kura_R;          /* order parameter R ∈ [0,1] */
    uint32_t kura_phase;      /* PLUCK/SUSTAIN/FINETUNE/LOCK */
    int      active_modes;    /* slots above √φ */
    GlyphSym glyph_sym;       /* emergent symmetry class */
    uint64_t timestamp_ns;    /* monotonic ns at last update */
} hdgl_shm_t;

/* Substrate configuration */
typedef struct {
    uint32_t    genome_fp;        /* hardware DNA fingerprint */
    uint32_t    tick;             /* initial tick value */
    const char *slots_dir;        /* where to write /lattice/slots/ */
    int         settle_steps;     /* basin settle steps before ticking */
    int         steps_per_write;  /* basin steps between slot writes */
    int         slot_write_every; /* write count interval for slot files */
    int         interval_us;      /* µs sleep between writes */
    int         verbose;
} substrate_config_t;

/* Defaults */
#define SUBSTRATE_DEFAULTS { \
    .genome_fp       = 0x88888888, \
    .tick            = 0, \
    .slots_dir       = "/lattice/slots", \
    .settle_steps    = 300, \
    .steps_per_write = 50, \
    .slot_write_every = 1, \
    .interval_us     = 20000, \
    .verbose         = 1 \
}

/* API */
int          substrate_start(substrate_config_t *cfg);
void         substrate_stop(void);
hdgl_shm_t  *substrate_shm(void);
void         substrate_lock(void);
void         substrate_unlock(void);
const char  *substrate_phase_name(void);
