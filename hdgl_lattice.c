/*
 * hdgl_lattice.c — Multi-Lattice Engine
 * =======================================
 *
 * LATTICE_PHI_TICK_128     HDGL-fabric v0.2  slot[i]=slot[i]*3+phi_tick
 * LATTICE_WATER_GLYPH_4096 Chladni 64×64 basin  Bessel eigenmodes
 * LATTICE_KURAMOTO_8D      ll_analog 8D oscillator
 * LATTICE_COMPOSITE        all three  (default)
 */

#include "hdgl_lattice.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/stat.h>

#define PHI    1.6180339887498948
#define PI     3.14159265358979323846
#define PHI32  2654435769U
#define FIB32  2654435761U

/* ── phi_tick_128  (HDGL-fabric v0.2, bit-exact with firmware) ─────────── */
typedef struct {
    uint32_t slots[128];   /* mirrors 0x101020..0x1013FC on bare metal */
    uint64_t phi_tick;
    uint32_t flags;        /* bit4 = APA_FLAG_CONSENSUS */
    uint32_t genome_fp;
    double   strand_ema[8];
    uint64_t step;
} PT128;

static void pt128_init(PT128 *s, uint32_t gfp, uint32_t tick) {
    memset(s, 0, sizeof *s);
    s->genome_fp = gfp; s->phi_tick = tick;
    for (int i = 0; i < 128; i++) {
        uint64_t seed = (uint64_t)i*PHI32 ^ (uint64_t)gfp*FIB32 ^ tick;
        s->slots[i] = (uint32_t)(seed & 0xFFFFFFFF);
    }
    s->slots[127] = gfp;                        /* genome_fp in slot[127] */
    for (int i = 0; i < 8; i++)
        s->strand_ema[i] = pow(PHI, -(i+1)*7.0);
}

static void pt128_step(PT128 *s) {
    s->phi_tick++;
    uint32_t tick32 = (uint32_t)(s->phi_tick & 0xFFFFFFFF);
    for (int i = 0; i < 127; i++) {            /* slot[i]=slot[i]*3+tick */
        s->slots[i] = s->slots[i]*3 + tick32;
        int32_t d = (int32_t)((uint32_t)(s->strand_ema[i%8]*65536.0) - s->slots[i]) >> 4;
        s->slots[i] = (uint32_t)((int32_t)s->slots[i] + d);
    }
    s->slots[127] = s->genome_fp;
    /* consensus: variance of top 7 slots < threshold */
    double sum = 0;
    for (int i = 120; i < 127; i++) sum += s->slots[i];
    double mean = sum/7, var = 0;
    for (int i = 120; i < 127; i++) { double d=s->slots[i]-mean; var+=d*d; }
    if (var/7.0 < s->phi_tick*1000.0) s->flags |= 0x10; else s->flags &= ~0x10u;
    s->step++;
}

static void pt128_to_dslots(PT128 *s, double *out) {
    for (int i = 0; i < 32; i++)
        out[i] = (double)s->slots[i*4] / 4294967295.0 * 2.0;
}

/* ── Kuramoto 8D  (ll_analog architecture) ────────────────────────────── */
typedef enum { KP_PLUCK, KP_SUSTAIN, KP_FINETUNE, KP_LOCK } KPhase;
typedef struct {
    double theta[8], omega[8], k, gamma, R;
    KPhase phase;
    double cv[50]; int cv_n; uint64_t step;
} K8D;

static void k8d_init(K8D *k, uint32_t gfp, uint32_t tick) {
    k->k=5.0; k->gamma=0.005; k->phase=KP_PLUCK; k->R=0; k->cv_n=0; k->step=tick;
    memset(k->cv, 0, sizeof k->cv);
    for (int i=0;i<8;i++) {
        uint64_t s=(uint64_t)i*PHI32^(uint64_t)gfp*FIB32^tick;
        k->theta[i]=(double)(s&0xFFFFFFFF)/4294967295.0*2*PI;
        k->omega[i]=PHI*pow(PHI,(double)i)*(1.0+(double)(s>>32&0xFF)/2560.0);
    }
}

static void k8d_step(K8D *k) {
    double ss=0,sc=0;
    for (int i=0;i<8;i++){ss+=sin(k->theta[i]);sc+=cos(k->theta[i]);}
    k->R=sqrt(ss*ss+sc*sc)/8;
    double P=atan2(ss,sc);
    for (int i=0;i<8;i++){
        k->theta[i]+=(k->omega[i]+k->k*k->R*sin(P-k->theta[i])
                      -k->gamma*cos(k->theta[i])*sin(k->theta[i]))*0.01;
        if (k->theta[i]>2*PI) k->theta[i]-=2*PI;
        if (k->theta[i]<0)    k->theta[i]+=2*PI;
    }
    double cv=1-k->R; k->cv[k->cv_n%50]=cv; k->cv_n++;
    double cvm=0; int n=k->cv_n<50?k->cv_n:50;
    for(int i=0;i<n;i++) cvm+=k->cv[i]; cvm/=n;
    if      (k->phase==KP_PLUCK    && cvm<0.50) { k->phase=KP_SUSTAIN;  k->k=3.0; k->gamma=0.008; }
    else if (k->phase==KP_SUSTAIN  && cvm<0.30) { k->phase=KP_FINETUNE; k->k=2.0; k->gamma=0.010; }
    else if (k->phase==KP_FINETUNE && cvm<0.05) { k->phase=KP_LOCK;     k->k=1.8; k->gamma=0.012; }
    k->step++;
}

/* ── Glyph loader — line-by-line parser ───────────────────────────────── */
const char *lattice_type_name(lattice_type_t t) {
    switch (t) {
    case LATTICE_PHI_TICK_128:     return "phi_tick_128 (v0.2 fabric)";
    case LATTICE_WATER_GLYPH_4096: return "water_glyph_4096 (Chladni)";
    case LATTICE_KURAMOTO_8D:      return "kuramoto_8d (ll_analog)";
    case LATTICE_COMPOSITE:        return "composite (all layers)";
    default:                       return "unknown";
    }
}

hdgl_glyph_t *lattice_load_hdgl(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "[lattice] cannot open '%s': %s\n", path, strerror(errno));
        return NULL;
    }
    hdgl_glyph_t *g = calloc(1, sizeof *g);
    if (!g) { fclose(f); return NULL; }
    strncpy(g->source_path, path, sizeof(g->source_path)-1);
    g->phi = PHI;

    /* keyword counters for lattice type detection */
    int has_phi_tick=0, has_kuramoto=0, has_chladni=0, has_water=0;
    int has_v02=0, has_v03=0;
    char line[512];

    while (fgets(line, sizeof line, f)) {
        /* glyph names: lines starting with "glyph " */
        if (strncmp(line, "glyph ", 6)==0 && g->n_glyphs < MAX_GLYPH_NAMES) {
            char *name = line+6;
            /* strip trailing whitespace/newline */
            int j=0;
            while (name[j] && name[j]!='\n' && name[j]!='\r' && name[j]!=' ' && j<63)
                j++;
            if (j>0) {
                memcpy(g->glyph_names[g->n_glyphs], name, j);
                g->glyph_names[g->n_glyphs][j] = '\0';
                g->n_glyphs++;
            }
        }
        /* genome_fp */
        if (!g->genome_fp) {
            char *p = strstr(line, "genome_fp");
            if (p) {
                p = strstr(p, "0x");
                if (!p) p = strstr(line+9, " ");
                if (p && p[0]=='0'&&p[1]=='x')
                    g->genome_fp = (uint32_t)strtoul(p, NULL, 16);
            }
        }
        /* version markers */
        if (strstr(line,"v0.2") || strstr(line,"HDGL-fabric") ||
            strstr(line,"phi_tick"))      has_v02=1;
        if (strstr(line,"v0.3") || strstr(line,"Chladni") ||
            strstr(line,"water_glyph"))   has_v03=1;

        /* lattice keyword detection */
        if (strstr(line,"phi_tick"))      has_phi_tick=1;
        if (strstr(line,"Kuramoto"))      has_kuramoto=1;
        if (strstr(line,"Chladni") ||
            strstr(line,"Bessel") ||
            strstr(line,"basin"))         has_chladni=1;
        if (strstr(line,"water_glyph") ||
            strstr(line,"water glyph"))   has_water=1;
    }
    fclose(f);

    /* Determine lattice type */
    if (has_chladni || has_water)
        g->lattice_type = LATTICE_WATER_GLYPH_4096;
    else if (has_kuramoto && !has_phi_tick)
        g->lattice_type = LATTICE_KURAMOTO_8D;
    else
        g->lattice_type = LATTICE_PHI_TICK_128;   /* default for v0.2 */

    g->version = has_v03 ? 3 : (has_v02 ? 2 : 2);

    fprintf(stderr, "[lattice] loaded '%s': v0.%d  type=%s  glyphs=%d  genome=0x%08X\n",
            path, g->version, lattice_type_name(g->lattice_type),
            g->n_glyphs, g->genome_fp);
    return g;
}

void lattice_free_hdgl(hdgl_glyph_t *g) { free(g); }

/* ── Slot file writer ─────────────────────────────────────────────────── */
static void write_slot(const char *dir, int n, double v) {
    char p[256]; snprintf(p, sizeof p, "%s/%d", dir, n);
    FILE *f = fopen(p,"w"); if(f){fprintf(f,"%.16f\n",v);fclose(f);}
}

/* ── Substrate thread ─────────────────────────────────────────────────── */
typedef struct {
    lattice_config_t cfg;
    PT128            pt128;
    K8D              kura;
    lattice_status_t status;
    volatile int     running;
    pthread_mutex_t  mu;
} LTask;

static LTask     g_task;
static pthread_t g_tid;

static void state_file_write(LTask *t) {
    static const char *pn[]={"PLUCK","SUSTAIN","FINETUNE","LOCK"};
    FILE *f = fopen("/run/lattice/state","w"); if(!f) return;
    fprintf(f,"LATTICE_TYPE=%s\n", lattice_type_name(t->cfg.type));
    fprintf(f,"LATTICE_GENOME_FP=0x%08X\n", t->cfg.genome_fp);
    fprintf(f,"LATTICE_PHI_TICK=%llu\n",(unsigned long long)t->pt128.phi_tick);
    fprintf(f,"LATTICE_CONSENSUS=%s\n",(t->pt128.flags&0x10)?"LOCK":"SEARCHING");
    fprintf(f,"LATTICE_KURA_PHASE=%s\n", pn[t->kura.phase<4?(int)t->kura.phase:3]);
    fprintf(f,"LATTICE_KURA_R=%.6f\n", t->kura.R);
    for(int i=0;i<8;i++) fprintf(f,"THETA_%d=%.6f\n",i,t->kura.theta[i]);
    for(int i=0;i<8;i++) fprintf(f,"SLOT_%d=%u\n",i,t->pt128.slots[i]);
    fclose(f);
}

static void *lattice_thread(void *arg) {
    LTask *t = (LTask*)arg;
    lattice_config_t *c = &t->cfg;

    mkdir(c->slots_dir, 0755);
    mkdir("/run/lattice", 0755);

    pt128_init(&t->pt128, c->genome_fp, c->tick);
    k8d_init(&t->kura, c->genome_fp, c->tick);

    fprintf(stderr,"[lattice:%s] genome=0x%08X  settling %d steps\n",
            lattice_type_name(c->type), c->genome_fp, c->settle_steps);

    for (int s=0; s<c->settle_steps && t->running; s++) {
        pt128_step(&t->pt128);
        k8d_step(&t->kura);
    }
    fprintf(stderr,"[lattice:%s] ticking\n", lattice_type_name(c->type));

    double dslots[32];
    uint64_t wn=0;

    while (t->running) {
        for (int s=0; s<c->steps_per_write; s++) {
            pt128_step(&t->pt128);
            k8d_step(&t->kura);
        }

        /* Build D-slots from active lattice */
        switch (c->type) {
        case LATTICE_PHI_TICK_128:
            pt128_to_dslots(&t->pt128, dslots);
            break;
        case LATTICE_KURAMOTO_8D:
            for(int i=0;i<8;i++)  dslots[i]=t->kura.theta[i]/(2*PI)*2.0;
            for(int i=8;i<32;i++) dslots[i]=dslots[i%8];
            break;
        default: /* COMPOSITE */
            for(int i=0;i<8;i++) dslots[i]=t->kura.theta[i]/(2*PI)*2.0;
            pt128_to_dslots(&t->pt128, dslots+8); /* overwrite 8..31 from pt128 */
            /* but keep only first 24 from pt128 */
            for(int i=8;i<32;i++)
                dslots[i]=(double)t->pt128.slots[(i-8)*4]/(double)0xFFFFFFFFu*2.0;
            break;
        }

        /* Write D1..D32 */
        for(int i=0;i<32;i++) write_slot(c->slots_dir, i+1, dslots[i]);
        /* slots 0..7: Kuramoto phase */
        for(int i=0;i<8;i++) write_slot(c->slots_dir, i, t->kura.theta[i]/(2*PI));
        /* slots 33..4096: pt128 extension */
        for(int i=33;i<=4096;i++) {
            double v=(double)t->pt128.slots[(i-1)%128]/(double)0xFFFFFFFFu*2.0;
            write_slot(c->slots_dir, i, v);
        }

        /* Update shared status */
        pthread_mutex_lock(&t->mu);
        t->status.phi_tick   = t->pt128.phi_tick;
        t->status.kura_R     = t->kura.R;
        t->status.kura_phase = (int)t->kura.phase;
        t->status.consensus  = (t->pt128.flags&0x10)?1:0;
        memcpy(t->status.slot_sample, t->pt128.slots, 8*sizeof(uint32_t));
        memcpy(t->status.theta,       t->kura.theta,  8*sizeof(double));
        pthread_mutex_unlock(&t->mu);

        if (wn%20==0) state_file_write(t);
        wn++;
        struct timespec ts={0,(long)c->interval_us*1000L};
        nanosleep(&ts,NULL);
    }
    return NULL;
}

int lattice_start(lattice_config_t *cfg) {
    memset(&g_task, 0, sizeof g_task);
    g_task.cfg     = *cfg;
    g_task.running = 1;
    pthread_mutex_init(&g_task.mu, NULL);
    return pthread_create(&g_tid, NULL, lattice_thread, &g_task);
}

void lattice_stop(void) {
    g_task.running=0;
    pthread_join(g_tid, NULL);
}

lattice_status_t lattice_status(void) {
    lattice_status_t s;
    pthread_mutex_lock(&g_task.mu);
    s = g_task.status;
    pthread_mutex_unlock(&g_task.mu);
    return s;
}
