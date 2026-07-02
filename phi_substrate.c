/*
 * phi_substrate.c — Continuous Analog Substrate Engine
 * ======================================================
 *
 * This is the substrate that QEMU was emulating.
 * It runs natively — no x86 instruction translation, no TCG overhead.
 *
 * TWO LAYERS, both running concurrently in a pthread:
 *
 * LAYER A — Water Glyph Basin (Chladni eigenmodes)
 *   64×64 = 4096 cell circular wave equation
 *   genome_fp bytes → DNA bases → Bessel angular modes J_n
 *   Stable eigenmodes → D1..D32 slot values
 *   Step time: ~50µs native  (vs ~2.5ms in QEMU TCG)
 *
 * LAYER B — 8D Kuramoto Oscillator (ll_analog architecture)
 *   8 coupled oscillators, phi-seeded natural frequencies
 *   Pluck→Sustain→FineTune→Lock phase progression (wu-wei)
 *   Lock state: CV < 0.05, order parameter R → 1
 *   Writes theta[0..7] to /lattice/slots/0..7
 *
 * SHARED MEMORY (hdgl_shm_t):
 *   Both layers write to the same struct.
 *   The shell, Alpine, and phi_analog_module.ko all read from it.
 *   Layout mirrors hdgl_smp_substrate.asm SHM at 0x7000.
 */

#include "phi_substrate.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <pthread.h>
#include <unistd.h>
#include <stdint.h>
#include <signal.h>
#include <time.h>
#include <sys/stat.h>
#include <fcntl.h>

/* ── Basin constants (Layer A) ─────────────────────────────────────────── */
#define GRID        64
#define CX          32
#define CY          32
#define R_BASIN     30.0
#define C_WAVE      0.35
#define DX          1.0
#define DT          0.4
#define DAMP        0.005
#define CF2         ((C_WAVE*DT/DX)*(C_WAVE*DT/DX))
#define N_BASES     16

/* ── Kuramoto constants (Layer B) ──────────────────────────────────────── */
#define ANA_DIMS        8
#define ANA_DT          0.01
#define ANA_K_PLUCK     5.0
#define ANA_K_SUSTAIN   3.0
#define ANA_K_FINETUNE  2.0
#define ANA_K_LOCK      1.8
#define ANA_G_PLUCK     0.005
#define ANA_G_SUSTAIN   0.008
#define ANA_G_FINETUNE  0.010
#define ANA_G_LOCK      0.012
#define ANA_CV_SUSTAIN  0.50
#define ANA_CV_FINETUNE 0.30
#define ANA_CV_LOCK     0.05
#define ANA_LOCK_WINDOW 50

/* ── Constants ─────────────────────────────────────────────────────────── */
#define PHI         1.6180339887498948
#define SQRT_PHI    1.2720196495140690
#define PI          3.14159265358979323846
#define PHI32       2654435769U
#define FIB32       2654435761U
#define SQPHI32     2654136499U

/* ── Bessel zeros BZ[n][m] for n=0..7, m=0..3 ─────────────────────────── */
static const double BZ[8][4] = {
    { 2.4048, 5.5201,  8.6537, 11.7915 },
    { 3.8317, 7.0156, 10.1735, 13.3237 },
    { 5.1356, 8.4172, 11.6198, 14.7960 },
    { 6.3802, 9.7610, 13.0152, 16.2235 },
    { 7.5883,11.0647, 14.3725, 17.6160 },
    { 8.7715,12.3386, 15.7002, 18.9801 },
    { 9.9361,13.5893, 17.0038, 20.3208 },
    {11.0864,14.7960, 18.2876, 21.6415 },
};
static double bz_r[8][4];

/* ── Basin state (Layer A) ─────────────────────────────────────────────── */
typedef struct {
    float field[GRID*GRID];
    float fprev[GRID*GRID];
    float drive[GRID*GRID];
    double dslots[N_DSLOTS];
    uint32_t genome_fp;
    uint32_t tick;
    uint64_t step;
} Basin;

/* ── Kuramoto state (Layer B) ──────────────────────────────────────────── */
typedef enum { PLUCK, SUSTAIN, FINETUNE, LOCK } APhase;

typedef struct {
    double theta[ANA_DIMS];
    double omega[ANA_DIMS];
    double re[ANA_DIMS];
    double im[ANA_DIMS];
    double k, gamma;
    APhase phase;
    double cv_hist[ANA_LOCK_WINDOW];
    int cv_pos;
    uint64_t step;
    double R;   /* order parameter */
} Kuramoto;

/* ── Global state ──────────────────────────────────────────────────────── */
static Basin     g_basin;
static Kuramoto  g_kura;
static hdgl_shm_t g_shm;
static pthread_mutex_t g_shm_mutex = PTHREAD_MUTEX_INITIALIZER;
static volatile int g_running = 1;

hdgl_shm_t *substrate_shm(void) { return &g_shm; }

/* ─── Basin helpers ────────────────────────────────────────────────────── */
static inline int in_basin(int x, int y) {
    double dx = x - CX, dy = y - CY;
    return dx*dx + dy*dy < R_BASIN*R_BASIN;
}

static void init_bz_radii(void) {
    for (int n = 0; n < 8; n++) {
        double rm = BZ[n][3];
        for (int m = 0; m < 4; m++)
            bz_r[n][m] = (BZ[n][m] / rm) * (R_BASIN * 0.92);
    }
}

static void basin_build_drive(Basin *b) {
    memset(b->drive, 0, sizeof(b->drive));
    int bases[N_BASES];
    for (int k = 0; k < N_BASES; k++)
        bases[k] = (b->genome_fp >> (k*2)) & 0x03;
    double tick_phase = (b->tick & 0xFF) / 255.0 * 2.0 * PI;

    for (int y = 0; y < GRID; y++) {
        for (int x = 0; x < GRID; x++) {
            if (!in_basin(x,y)) continue;
            double cx = x-CX, cy = y-CY;
            double r = sqrt(cx*cx+cy*cy);
            double theta = atan2(cy, cx);
            double dist = R_BASIN - r;
            if (dist > 6.0 || dist < 0.0) continue;
            double ew = exp(-dist*dist/4.0);
            double d = 0;
            for (int k = 0; k < N_BASES; k++) {
                double th_k = k*(2.0*PI/N_BASES) + tick_phase;
                int n = bases[k];
                double fib_n  = (double)(int[]){1,1,2,3,5,8,13,21}[n];
                double prm_n  = (double)(int[]){2,3,5,7,11,13,17,19}[n];
                double omega_n = pow(PHI, -7.0*(n+1));
                double dn = sqrt(PHI * fib_n * pow(2.0,n+1) * prm_n * omega_n);
                d += cos((double)n*(theta-th_k)) * dn * 0.15;
            }
            b->drive[y*GRID+x] = (float)(d * ew);
        }
    }
}

static void basin_step(Basin *b, double amp) {
    float tmp[GRID*GRID];
    memset(tmp, 0, sizeof(tmp));
    for (int y = 1; y < GRID-1; y++) {
        for (int x = 1; x < GRID-1; x++) {
            if (!in_basin(x,y)) { tmp[y*GRID+x]=0; continue; }
            float u  = b->field[y*GRID+x];
            float up = b->fprev[y*GRID+x];
            float lap = b->field[y*GRID+x+1] + b->field[y*GRID+x-1]
                      + b->field[(y+1)*GRID+x] + b->field[(y-1)*GRID+x]
                      - 4.0f*u;
            tmp[y*GRID+x] = (2.0f-DAMP)*u - (1.0f-DAMP)*up
                           + (float)CF2*lap
                           + (float)(amp * b->drive[y*GRID+x]);
        }
    }
    memcpy(b->fprev, b->field, sizeof(b->field));
    memcpy(b->field, tmp, sizeof(tmp));
    b->step++;
}

static void basin_extract(Basin *b) {
    for (int n = 0; n < 8; n++) {
        for (int m = 0; m < 4; m++) {
            double rt = bz_r[n][m], dr = 1.5, sum = 0; int cnt = 0;
            for (int y = 0; y < GRID; y++) {
                for (int x = 0; x < GRID; x++) {
                    if (!in_basin(x,y)) continue;
                    double cx=x-CX, cy=y-CY;
                    double r = sqrt(cx*cx+cy*cy);
                    if (fabs(r-rt) < dr) {
                        double w = fabs(cos((double)n*atan2(cy,cx)))+0.1;
                        sum += fabs(b->field[y*GRID+x])*w; cnt++;
                    }
                }
            }
            b->dslots[n*4+m] = cnt>0 ? sum/cnt : 0.0;
        }
    }
    /* normalise: scale so max ≈ 2.0 (above √φ for high-n strands) */
    double mx = 1e-10;
    for (int i = 0; i < N_DSLOTS; i++) if (b->dslots[i]>mx) mx=b->dslots[i];
    double sc = mx>1e-10 ? 2.0/mx : 1.0;
    for (int i = 0; i < N_DSLOTS; i++) b->dslots[i] *= sc;
}

/* ─── Kuramoto helpers ─────────────────────────────────────────────────── */
static void kura_init(Kuramoto *k, uint32_t genome_fp, uint32_t tick) {
    k->k = ANA_K_PLUCK; k->gamma = ANA_G_PLUCK; k->phase = PLUCK;
    k->cv_pos = 0; k->step = tick; k->R = 0.0;
    memset(k->cv_hist, 0, sizeof(k->cv_hist));
    for (int i = 0; i < ANA_DIMS; i++) {
        uint64_t s = (uint64_t)i*PHI32 ^ (uint64_t)genome_fp*FIB32 ^ tick;
        k->theta[i] = (double)(s&0xFFFFFFFF)/4294967295.0 * 2.0*PI;
        k->omega[i] = PHI*pow(PHI,(double)i) * (1.0+(double)(s>>32&0xFF)/2560.0);
        k->re[i] = cos(k->theta[i]); k->im[i] = sin(k->theta[i]);
    }
}

static void kura_step(Kuramoto *k) {
    double ss=0, sc=0;
    for (int i=0; i<ANA_DIMS; i++) { ss+=sin(k->theta[i]); sc+=cos(k->theta[i]); }
    k->R = sqrt(ss*ss+sc*sc)/ANA_DIMS;
    double Psi = atan2(ss, sc);
    double k1[ANA_DIMS], k2[ANA_DIMS], k3[ANA_DIMS], k4[ANA_DIMS];
    for (int i=0;i<ANA_DIMS;i++)
        k1[i]=k->omega[i]+k->k*k->R*sin(Psi-k->theta[i])-k->gamma*k->re[i]*sin(k->theta[i]);
    for (int i=0;i<ANA_DIMS;i++) { double t=k->theta[i]+k1[i]*ANA_DT*0.5; k2[i]=k->omega[i]+k->k*k->R*sin(Psi-t); }
    for (int i=0;i<ANA_DIMS;i++) { double t=k->theta[i]+k2[i]*ANA_DT*0.5; k3[i]=k->omega[i]+k->k*k->R*sin(Psi-t); }
    for (int i=0;i<ANA_DIMS;i++) { double t=k->theta[i]+k3[i]*ANA_DT;      k4[i]=k->omega[i]+k->k*k->R*sin(Psi-t); }
    for (int i=0;i<ANA_DIMS;i++) {
        k->theta[i] += (k1[i]+2*k2[i]+2*k3[i]+k4[i])*ANA_DT/6.0;
        while(k->theta[i]>2*PI) k->theta[i]-=2*PI;
        while(k->theta[i]<0)    k->theta[i]+=2*PI;
        k->re[i]=cos(k->theta[i]); k->im[i]=sin(k->theta[i]);
    }
    double cv = 1.0-k->R;
    k->cv_hist[k->cv_pos%ANA_LOCK_WINDOW]=cv; k->cv_pos++;
    double cvm=0; int n=(k->cv_pos<ANA_LOCK_WINDOW)?k->cv_pos:ANA_LOCK_WINDOW;
    for (int i=0;i<n;i++) cvm+=k->cv_hist[i];
    cvm/=n;
    switch(k->phase) {
    case PLUCK:
        if(cvm<ANA_CV_SUSTAIN)  { k->phase=SUSTAIN;  k->k=ANA_K_SUSTAIN;  k->gamma=ANA_G_SUSTAIN;  }
        break;
    case SUSTAIN:
        if(cvm<ANA_CV_FINETUNE) { k->phase=FINETUNE; k->k=ANA_K_FINETUNE; k->gamma=ANA_G_FINETUNE; }
        break;
    case FINETUNE:
        if(cvm<ANA_CV_LOCK)     { k->phase=LOCK;     k->k=ANA_K_LOCK;     k->gamma=ANA_G_LOCK;     }
        break;
    case LOCK:
        break;
    }
    k->step++;
}

/* ─── SHM update ───────────────────────────────────────────────────────── */
static void shm_update(Basin *b, Kuramoto *k) {
    pthread_mutex_lock(&g_shm_mutex);
    g_shm.magic     = 0x48444C535542530ULL;
    g_shm.status    = 2; /* TICKING */
    g_shm.tick      = (uint32_t)b->step;
    g_shm.kura_step = (uint32_t)k->step;
    g_shm.kura_R    = k->R;
    g_shm.kura_phase = (uint32_t)k->phase;

    /* D-slots: basin eigenmodes */
    uint32_t dbits = 0;
    for (int i = 0; i < N_DSLOTS; i++) {
        g_shm.d_slots[i] = b->dslots[i];
        if (b->dslots[i] > SQRT_PHI) dbits |= (1u<<i);
    }
    g_shm.d_bits    = dbits;
    g_shm.dn_agg    = dbits; /* simplified: dn_agg = d_bits for now */

    /* Kuramoto theta values in slots 0..7 */
    for (int i = 0; i < ANA_DIMS; i++)
        g_shm.theta[i] = k->theta[i];

    /* Glyph symmetry */
    int active = __builtin_popcount(dbits);
    g_shm.active_modes = active;
    g_shm.glyph_sym =
        active < 3  ? SYM_VOID   :
        active < 8  ? SYM_2FOLD  :
        active < 14 ? SYM_3FOLD  :
        active < 20 ? SYM_4FOLD  :
        active < 26 ? SYM_6FOLD  : SYM_8FOLD;

    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    g_shm.timestamp_ns = (uint64_t)ts.tv_sec*1000000000ULL + ts.tv_nsec;
    pthread_mutex_unlock(&g_shm_mutex);
}

/* ─── Slot file writer ─────────────────────────────────────────────────── */
static void write_slot(const char *dir, int n, double v) {
    char path[256]; snprintf(path,sizeof(path),"%s/%d",dir,n);
    FILE *f = fopen(path,"w"); if(f){fprintf(f,"%.16f\n",v);fclose(f);}
}

static void write_state_file(Basin *b, Kuramoto *k) {
    static const char *phase_names[] = {"PLUCK","SUSTAIN","FINETUNE","LOCK"};
    FILE *f = fopen("/run/lattice/state","w"); if(!f) return;
    fprintf(f,"LATTICE_N=4096\nLATTICE_STEPS=%llu\n",
            (unsigned long long)b->step);
    fprintf(f,"LATTICE_GENOME_FP=0x%08X\nLATTICE_TICK=0x%08X\n",
            b->genome_fp, b->tick);
    fprintf(f,"LATTICE_D_BITS=0x%08X\n", g_shm.d_bits);
    fprintf(f,"LATTICE_SUBSTRATE=hdgl_runtime\n");
    fprintf(f,"LATTICE_BASIN_CELLS=4096\n");
    fprintf(f,"LATTICE_KURA_PHASE=%s\nLATTICE_KURA_R=%.6f\n",
            phase_names[k->phase], k->R);
    fprintf(f,"LATTICE_GLYPH_ACTIVE_MODES=%d\n", g_shm.active_modes);
    for(int i=0;i<8;i++) fprintf(f,"THETA_%d=%.8f\n",i,k->theta[i]);
    for(int i=0;i<8;i++) fprintf(f,"D_SLOT_%d=%.8f\n",i+1,b->dslots[i]);
    fclose(f);
}

/* ─── Substrate thread ─────────────────────────────────────────────────── */
static void *substrate_thread(void *arg) {
    substrate_config_t *cfg = (substrate_config_t*)arg;

    init_bz_radii();

    /* Init basin */
    memset(&g_basin, 0, sizeof(g_basin));
    g_basin.genome_fp = cfg->genome_fp;
    g_basin.tick      = cfg->tick;
    g_basin.field[CY*GRID+CX] = 0.1f; /* seed centre */
    basin_build_drive(&g_basin);

    /* Init Kuramoto */
    kura_init(&g_kura, cfg->genome_fp, cfg->tick);

    /* Init SHM */
    memset(&g_shm, 0, sizeof(g_shm));
    g_shm.status = 1; /* READY */

    mkdir("/lattice", 0755);
    mkdir("/lattice/slots", 0755);
    mkdir("/run/lattice", 0755);

    if (cfg->verbose)
        fprintf(stderr,"[substrate] basin %d×%d=%d cells  genome=0x%08X  tick=0x%08X\n",
                GRID, GRID, GRID*GRID, cfg->genome_fp, cfg->tick);

    /* Settle: let eigenmodes develop */
    for (int s = 0; s < cfg->settle_steps && g_running; s++) {
        double amp = sin(2.0*PI*s/(cfg->settle_steps/4.0))*0.08+0.04;
        basin_step(&g_basin, amp);
        kura_step(&g_kura);
    }

    if (cfg->verbose)
        fprintf(stderr,"[substrate] eigenmodes settled — ticking\n");

    uint64_t write_count = 0;
    uint64_t last_slot_write = 0;

    while (g_running) {
        /* Layer A: basin wave steps */
        for (int s = 0; s < cfg->steps_per_write; s++) {
            double amp = 0.06 + 0.02*sin(2.0*PI*g_basin.step/512.0);
            basin_step(&g_basin, amp);
        }
        basin_extract(&g_basin);

        /* Layer B: Kuramoto steps */
        for (int s = 0; s < cfg->steps_per_write*10; s++)
            kura_step(&g_kura);

        /* Tick drive */
        g_basin.tick++;
        if (g_basin.tick % 16 == 0)
            basin_build_drive(&g_basin);

        /* Update SHM */
        shm_update(&g_basin, &g_kura);

        /* Write slot files */
        if (write_count - last_slot_write >= (uint64_t)cfg->slot_write_every) {
            /* D1..D32 from basin eigenmodes */
            for (int i = 0; i < N_DSLOTS; i++)
                write_slot(cfg->slots_dir, i+1, g_basin.dslots[i]);
            /* Slots 0..7: Kuramoto theta/(2π) */
            for (int i = 0; i < ANA_DIMS; i++)
                write_slot(cfg->slots_dir, i, g_kura.theta[i]/(2.0*PI));
            /* Slots 33..4096: phi-fold extensions */
            for (int i = 33; i <= 4096; i++) {
                int d = (i-1) % N_DSLOTS;
                double v = g_basin.dslots[d]*0.5 + g_basin.dslots[(d+1)%N_DSLOTS]*0.5;
                write_slot(cfg->slots_dir, i, v);
            }
            write_state_file(&g_basin, &g_kura);
            last_slot_write = write_count;
        }
        write_count++;

        /* Pace */
        struct timespec ts = { 0, (long)(cfg->interval_us * 1000L) };
        nanosleep(&ts, NULL);
    }

    if (cfg->verbose)
        fprintf(stderr,"[substrate] stopped at basin_step=%llu kura_step=%llu\n",
                (unsigned long long)g_basin.step,
                (unsigned long long)g_kura.step);
    return NULL;
}

/* ─── Public API ───────────────────────────────────────────────────────── */

int substrate_start(substrate_config_t *cfg) {
    pthread_t tid;
    int rc = pthread_create(&tid, NULL, substrate_thread, cfg);
    if (rc == 0) pthread_detach(tid);
    return rc;
}

void substrate_stop(void) { g_running = 0; }

void substrate_lock(void)   { pthread_mutex_lock(&g_shm_mutex); }
void substrate_unlock(void) { pthread_mutex_unlock(&g_shm_mutex); }

const char *substrate_phase_name(void) {
    static const char *names[] = {"PLUCK","SUSTAIN","FINETUNE","LOCK"};
    return names[g_shm.kura_phase < 4 ? g_shm.kura_phase : 3];
}
