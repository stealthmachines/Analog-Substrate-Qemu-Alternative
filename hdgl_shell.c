/*
 * hdgl_shell.c — HDGL Router64 Shell (Native C)
 * ==============================================
 *
 * Every command that runs in the Router64 ASM shell is reproduced here
 * as a native C function. No x86 instruction translation. No TCG overhead.
 *
 * Commands:
 *   help          list all commands
 *   analog        phi-lattice field display (D1..D32, fire indicators)
 *   dn            Dn aggregate (8-nibble hex)
 *   prismatic     4096-strand tally (prismatic field summary)
 *   pool          binary pool state (D-bits, per-strand)
 *   wave          wave table (32 entries, period-3 pattern)
 *   glyph         Chladni glyph symmetry + Kuramoto phase
 *   substrate     full substrate status
 *   tick N        advance substrate N steps
 *   genome HEX    re-seed with new genome fingerprint
 *   alpine K I    boot kernel from disk at LBA K, initrd at LBA I
 *   kexec FILE    boot kernel FILE via kexec (no VM needed)
 *   slots [N]     show slot N (or first 32)
 *   quit / exit   stop the runtime
 */

#include "hdgl_shell.h"
#include "phi_substrate.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include <unistd.h>
#include <fcntl.h>
#include <termios.h>
#include <sys/stat.h>

#define PHI      1.6180339887498948
#define SQRT_PHI 1.2720196495140690

static const char *PROMPT = "\033[1;35mRouter64\033[0m\033[1;32m>\033[0m ";

/* ── Terminal helpers ──────────────────────────────────────────────────── */
static struct termios g_orig_term;
static int g_raw = 0;

void shell_term_raw(void) {
    struct termios t;
    tcgetattr(STDIN_FILENO, &g_orig_term);
    t = g_orig_term;
    t.c_lflag &= ~(ICANON|ECHO);
    t.c_cc[VMIN] = 1; t.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSANOW, &t);
    g_raw = 1;
}

void shell_term_restore(void) {
    if (g_raw) tcsetattr(STDIN_FILENO, TCSANOW, &g_orig_term);
}

/* ── Field bar helper ──────────────────────────────────────────────────── */
static void print_bar(double v, double threshold) {
    int w = 20;
    int fill = (int)(v / 2.0 * w);
    if (fill < 0) fill = 0;
    if (fill > w) fill = w;
    printf("[");
    for (int i = 0; i < w; i++) {
        if (i < fill) {
            if (v > threshold)
                printf("\033[1;31m█\033[0m");   /* red: FIRE */
            else
                printf("\033[0;34m▒\033[0m");   /* blue: below threshold */
        } else printf(" ");
    }
    printf("]");
}

/* ── cmd_help ──────────────────────────────────────────────────────────── */
static void cmd_help(void) {
    printf("\n\033[1;35m HDGL Runtime — Router64 (Native C)\033[0m\n");
    printf(" \033[0;90m────────────────────────────────────────────────\033[0m\n");
    printf("  \033[1manalog\033[0m          phi-lattice D1..D32 field\n");
    printf("  \033[1mdn\033[0m              Dn aggregate (8-nibble hex)\n");
    printf("  \033[1mprismatic\033[0m       4096-strand tally\n");
    printf("  \033[1mpool\033[0m            binary pool (32-bit, e.g. 0xFFFF0000)\n");
    printf("  \033[1mwave\033[0m            wave table (period-3)\n");
    printf("  \033[1mglyph\033[0m           Chladni symmetry + Kuramoto phase\n");
    printf("  \033[1msubstrate\033[0m       full substrate status\n");
    printf("  \033[1mslots [N]\033[0m       show slot N or first 32\n");
    printf("  \033[1mtick N\033[0m          info: substrate auto-ticks\n");
    printf("  \033[1mgenome HEX\033[0m      re-seed genome fingerprint\n");
    printf("  \033[1malpine K I\033[0m      boot kernel@LBA-K initrd@LBA-I\n");
    printf("  \033[1mkexec FILE INITRD\033[0m  boot via kexec (no VM)\n");
    printf("  \033[1mquit\033[0m / \033[1mexit\033[0m    stop\n");
    printf("\n");
}

/* ── cmd_analog ────────────────────────────────────────────────────────── */
static void cmd_analog(void) {
    hdgl_shm_t *s = substrate_shm();
    substrate_lock();
    printf("\n\033[1;36m Phi-Lattice Field  [threshold=√φ=%.4f]\033[0m\n", SQRT_PHI);
    printf(" \033[0;90m────────────────────────────────────────────────────\033[0m\n");
    static const char *strand_names[] = {"A","B","C","D","E","F","G","H"};
    for (int n = 0; n < 8; n++) {
        printf(" Strand \033[1m%s\033[0m  Ω=1/φ^%d  r_dim=0.%d\n",
               strand_names[n], (n+1)*7, 3+n);
        for (int m = 0; m < 4; m++) {
            int i = n*4+m;
            double v = s->d_slots[i];
            int fire = v > SQRT_PHI;
            printf("   D%-2d = %7.4f  ", i+1, v);
            print_bar(v, SQRT_PHI);
            if (fire) printf("  \033[1;31mFIRE\033[0m");
            printf("\n");
        }
    }
    printf(" Dn=0x%08X  d_bits=0x%08X  step=%u\n",
           s->dn_agg, s->d_bits, s->tick);
    substrate_unlock();
    printf("\n");
}

/* ── cmd_dn ────────────────────────────────────────────────────────────── */
static void cmd_dn(void) {
    hdgl_shm_t *s = substrate_shm();
    substrate_lock();
    uint32_t dn = s->dn_agg;
    uint32_t db = s->d_bits;
    printf("\n\033[1;33m Dn Aggregate:\033[0m  0x%08X\n", dn);
    printf(" D-bits:        0x%08X\n", db);
    printf(" Binary pool:   ");
    for (int i = 0; i < 32; i++) {
        printf("%d", (db>>i)&1);
        if ((i+1)%4==0 && i<31) printf(" ");
    }
    printf("\n");
    /* 8-nibble ternary tally */
    printf(" 8-nibble:      ");
    for (int n = 0; n < 8; n++) {
        uint8_t nibble = (dn>>(n*4))&0xF;
        int fire = nibble > 8;
        int gnd  = nibble < 8;
        if (fire)     printf("\033[1;31m%X\033[0m", nibble);
        else if (gnd) printf("\033[0;34m%X\033[0m", nibble);
        else          printf("\033[0;32m%X\033[0m", nibble);
    }
    printf("  (red=excited  blue=grounded  green=balanced)\n");
    substrate_unlock();
    printf("\n");
}

/* ── cmd_prismatic ─────────────────────────────────────────────────────── */
static void cmd_prismatic(void) {
    hdgl_shm_t *s = substrate_shm();
    substrate_lock();
    uint32_t db = s->d_bits;
    int fire = __builtin_popcount(db);
    int gnd  = 32 - fire;
    printf("\n\033[1;35m Prismatic Field (4096-strand tally)\033[0m\n");
    printf(" \033[0;90m────────────────────────────────────────────────\033[0m\n");
    printf("  Excited (>√φ): \033[1;31m%2d\033[0m slots (D-bits=1)\n", fire);
    printf("  Grounded      : \033[0;34m%2d\033[0m slots (D-bits=0)\n", gnd);
    printf("  Balance ratio : %.3f  (1.0=all fire, 0.0=all ground)\n",
           fire / 32.0);
    printf("  Glyph sym     : ");
    static const char *sym[] = {"void","2-fold (linear)","3-fold (trifoil)",
                                 "4-fold (cross)","6-fold (hex)","8-fold (octave)"};
    printf("\033[1;36m%s\033[0m\n", sym[s->glyph_sym < 6 ? s->glyph_sym : 5]);
    printf("  Active modes  : %d / 32\n", s->active_modes);
    printf("  D-bits        : 0x%08X\n", db);
    printf("\n  Strand summary:\n");
    static const char *sn[] = {"A","B","C","D","E","F","G","H"};
    for (int n = 0; n < 8; n++) {
        uint8_t sb = (db>>(n*4))&0xF;
        printf("   %s [%d%d%d%d]  ",sn[n],
               (sb>>0)&1,(sb>>1)&1,(sb>>2)&1,(sb>>3)&1);
        if (sb==0)    printf("\033[0;34mGROUNDED\033[0m");
        else if(sb==0xF) printf("\033[1;31mFULL FIRE\033[0m");
        else          printf("\033[0;33mPARTIAL (%d/4)\033[0m",__builtin_popcount(sb));
        printf("\n");
    }
    substrate_unlock();
    printf("\n");
}

/* ── cmd_pool ──────────────────────────────────────────────────────────── */
static void cmd_pool(void) {
    hdgl_shm_t *s = substrate_shm();
    substrate_lock();
    uint32_t db = s->d_bits;
    printf("\n\033[1;33m Binary Pool  (32-bit, D1=LSB D32=MSB)\033[0m\n");
    printf("  0x%08X\n  ", db);
    for (int i = 0; i < 32; i++) {
        int bit = (db>>i)&1;
        if (bit) printf("\033[1;31m1\033[0m"); else printf("\033[0;34m0\033[0m");
        if ((i+1)%4==0 && i<31) printf(" ");
    }
    printf("\n  D1..D12=0 (grounded)  D13..D32=1 (excited) → 0xFFFF0000 target\n");
    printf("  Actual: 0x%08X  Active: %d/32\n", db, __builtin_popcount(db));
    substrate_unlock();
    printf("\n");
}

/* ── cmd_wave ──────────────────────────────────────────────────────────── */
static void cmd_wave(void) {
    printf("\n\033[1;36m Wave Table (period-3, 32 entries)\033[0m\n");
    double wave[32];
    for (int i = 0; i < 32; i++) {
        int p = i % 3;
        wave[i] = p==0 ? 0.3 : p==1 ? 0.0 : -0.3;
    }
    for (int i = 0; i < 32; i++) {
        printf("  w[%2d] = %+.1f  ", i, wave[i]);
        if (wave[i] > 0) printf("\033[1;32m+\033[0m");
        else if (wave[i] < 0) printf("\033[1;31m-\033[0m");
        else printf("\033[0;90m·\033[0m");
        printf("\n");
    }
    printf("\n");
}

/* ── cmd_glyph ─────────────────────────────────────────────────────────── */
static void cmd_glyph(void) {
    hdgl_shm_t *s = substrate_shm();
    substrate_lock();
    static const char *pname[] = {"PLUCK","SUSTAIN","FINETUNE","LOCK"};
    static const char *sym[] = {"void","2-fold","3-fold","4-fold","6-fold","8-fold"};
    static const char *pcol[] = {"\033[0;33m","\033[0;36m","\033[1;34m","\033[1;32m"};
    printf("\n\033[1;35m Chladni Glyph State\033[0m\n");
    printf("  Basin: 64×64 = 4096 cells  R=30  threshold=√φ=%.4f\n", SQRT_PHI);
    printf("  Glyph symmetry: \033[1;36m%s\033[0m  (%d active modes)\n",
           sym[s->glyph_sym<6?s->glyph_sym:5], s->active_modes);
    printf("  Kuramoto: %s%s\033[0m  R=%.4f  step=%u\n",
           pcol[s->kura_phase<4?s->kura_phase:3],
           pname[s->kura_phase<4?s->kura_phase:3],
           s->kura_R, s->kura_step);
    printf("  Phases θ[0..7]: ");
    for (int i = 0; i < 8; i++) printf("%.3f ", s->theta[i]);
    printf("\n");
    printf("  D-bits: 0x%08X  Dn: 0x%08X\n", s->d_bits, s->dn_agg);
    /* ASCII Chladni art (8×4 grid of fire/gnd) */
    printf("\n  Chladni pattern (strand×radial mode, ■=FIRE ·=ground):\n  ");
    static const char *sn[] = {"A","B","C","D","E","F","G","H"};
    printf("     m1    m2    m3    m4\n");
    for (int n = 0; n < 8; n++) {
        printf("  %s  ", sn[n]);
        for (int m = 0; m < 4; m++) {
            int i = n*4+m;
            int fire = s->d_slots[i] > SQRT_PHI;
            if (fire) printf("\033[1;31m■ \033[0m    ");
            else      printf("\033[0;34m· \033[0m    ");
        }
        printf("\n");
    }
    substrate_unlock();
    printf("\n");
}

/* ── cmd_substrate ─────────────────────────────────────────────────────── */
static void cmd_substrate(void) {
    hdgl_shm_t *s = substrate_shm();
    substrate_lock();
    printf("\n\033[1;36m HDGL Substrate Status\033[0m\n");
    printf(" \033[0;90m────────────────────────────────────────────────────\033[0m\n");
    printf("  Engine:    Native C (no x86 translation, no TCG)\n");
    printf("  Basin:     64×64 = 4096 cells (wave eq. at host speed)\n");
    printf("  Kuramoto:  8D oscillator (ll_analog architecture)\n");
    printf("  Thread:    pthread, concurrent with shell\n");
    printf("\n");
    printf("  Basin step:    %u\n", s->tick);
    printf("  Kura step:     %u  phase=%s\n", s->kura_step,
           (const char*[]){"PLUCK","SUSTAIN","FINETUNE","LOCK"}
           [s->kura_phase<4?s->kura_phase:3]);
    printf("  Kura R:        %.4f  (1.0=fully locked)\n", s->kura_R);
    printf("  D-bits:        0x%08X\n", s->d_bits);
    printf("  Dn aggregate:  0x%08X\n", s->dn_agg);
    printf("  Active modes:  %d / 32\n", s->active_modes);
    printf("  Slots dir:     /lattice/slots/\n");
    printf("  State file:    /run/lattice/state\n");
    printf("  Status:        %s\n", s->status==2?"TICKING":s->status==1?"READY":"INIT");
    substrate_unlock();
    printf("\n");
}

/* ── cmd_slots ─────────────────────────────────────────────────────────── */
static void cmd_slots(int n) {
    if (n > 0) {
        char path[256]; snprintf(path,sizeof(path),"/lattice/slots/%d",n);
        FILE *f = fopen(path,"r");
        if (f) {
            double v; fscanf(f,"%lf",&v); fclose(f);
            printf("  slot[%d] = %.16f  %s\n", n, v,
                   v>SQRT_PHI ? "\033[1;31mFIRE\033[0m":"");
        } else {
            printf("  slot[%d] not found\n", n);
        }
    } else {
        hdgl_shm_t *s = substrate_shm();
        substrate_lock();
        printf("\n  D-slots (D1..D32 from basin eigenmodes):\n");
        for (int i = 0; i < N_DSLOTS; i++) {
            double v = s->d_slots[i];
            printf("  D%-2d = %.6f  %s\n", i+1, v,
                   v>SQRT_PHI?"\033[1;31m■\033[0m":"\033[0;34m·\033[0m");
        }
        substrate_unlock();
    }
    printf("\n");
}

/* ── cmd_kexec ─────────────────────────────────────────────────────────── */
void cmd_kexec(const char *kernel, const char *initrd,
               uint32_t genome_fp, uint32_t tick) {
    printf("\n\033[1;35m[kexec]\033[0m Booting on analog substrate\n");
    printf("  Kernel: %s\n", kernel);
    printf("  Initrd: %s\n", initrd);
    printf("  genome_fp: 0x%08X  tick: 0x%08X\n", genome_fp, tick);
    printf("  Substrate continues on this core after handoff.\n");
    printf("  Alpine reads /lattice/slots/ for live field state.\n\n");

    char cmdline[512];
    snprintf(cmdline, sizeof(cmdline),
             "console=ttyS0,9600n8 rdinit=/init quiet "
             "hdgl.dn=%08X hdgl.tick=%08X",
             genome_fp, tick);

    /* Try kexec utility */
    char kexec_load[1024], kexec_exec[256];
    snprintf(kexec_load, sizeof(kexec_load),
             "kexec -l '%s' --initrd='%s' --append='%s' 2>&1",
             kernel, initrd, cmdline);
    snprintf(kexec_exec, sizeof(kexec_exec),
             "kexec -e 2>&1");

    printf("  Running: %s\n", kexec_load);
    int rc = system(kexec_load);
    if (rc != 0) {
        printf("\033[1;33m  kexec load failed (rc=%d) — trying direct exec\033[0m\n", rc);
        /* Fallback: exec qemu-system-x86_64 with -kernel */
        char qemu_cmd[2048];
        snprintf(qemu_cmd, sizeof(qemu_cmd),
                 "exec qemu-system-x86_64 "
                 "-kernel '%s' -initrd '%s' "
                 "-append '%s' "
                 "-m 512M -serial stdio -no-reboot -display none",
                 kernel, initrd, cmdline);
        printf("  Fallback: %s\n\n", qemu_cmd);
        system(qemu_cmd);
        return;
    }
    printf("  Running: %s\n\n", kexec_exec);
    printf("  Substrate state at handoff:\n");
    cmd_substrate();
    system(kexec_exec); /* does not return if successful */
    printf("\033[1;31m  kexec exec failed — substrate still running\033[0m\n\n");
}

/* ── Input line reader ─────────────────────────────────────────────────── */
static int read_line(char *buf, int max) {
    int pos = 0; buf[0] = 0;
    while (1) {
        int c = getchar();
        if (c == EOF || c == '\n' || c == '\r') {
            buf[pos] = 0;
            printf("\n"); fflush(stdout);
            return pos;
        }
        if (c == 127 || c == '\b') { /* backspace */
            if (pos > 0) {
                pos--;
                printf("\b \b"); fflush(stdout);
            }
            continue;
        }
        if (c < 32) continue; /* other control chars */
        if (pos < max-1) {
            buf[pos++] = (char)c;
            putchar(c); fflush(stdout);
        }
    }
}

/* ── Shell main loop ───────────────────────────────────────────────────── */
void shell_run(shell_config_t *cfg) {
    char line[256];

    /* Startup banner */
    printf("\033[2J\033[H"); /* clear screen */
    printf("\033[1;35m");
    printf("╔══════════════════════════════════════════════════════╗\n");
    printf("║   HDGL Runtime — Continuous Analog Substrate        ║\n");
    printf("║   Native C  |  No x86 translation  |  No TCG       ║\n");
    printf("╚══════════════════════════════════════════════════════╝\n");
    printf("\033[0m");
    printf(" genome_fp=0x%08X  slots=%s\n", cfg->genome_fp, cfg->slots_dir);
    printf(" Type \033[1mhelp\033[0m for commands.  Substrate ticking in background.\n\n");

    while (cfg->running) {
        printf("%s", PROMPT); fflush(stdout);
        if (read_line(line, sizeof(line)) < 0) break;
        if (!line[0]) continue;

        char cmd[64]="", a1[128]="", a2[128]="";
        sscanf(line, "%63s %127s %127s", cmd, a1, a2);

        if      (!strcmp(cmd,"help"))      cmd_help();
        else if (!strcmp(cmd,"analog"))    cmd_analog();
        else if (!strcmp(cmd,"dn"))        cmd_dn();
        else if (!strcmp(cmd,"prismatic")) cmd_prismatic();
        else if (!strcmp(cmd,"pool"))      cmd_pool();
        else if (!strcmp(cmd,"wave"))      cmd_wave();
        else if (!strcmp(cmd,"glyph"))     cmd_glyph();
        else if (!strcmp(cmd,"substrate")) cmd_substrate();
        else if (!strcmp(cmd,"slots")) {
            int n = a1[0] ? atoi(a1) : 0;
            cmd_slots(n);
        }
        else if (!strcmp(cmd,"genome") && a1[0]) {
            cfg->genome_fp = (uint32_t)strtoul(a1, NULL, 16);
            printf("  genome_fp updated to 0x%08X (rebuild drive map on next tick)\n\n",
                   cfg->genome_fp);
        }
        else if (!strcmp(cmd,"alpine") && a1[0] && a2[0]) {
            printf("\n  alpine command: use --alpine flag or kexec command\n");
            printf("  LBA boot requires disk image + direct ATA read\n");
            printf("  Use: kexec /boot/vmlinuz bin/hdgl_initrd.img\n\n");
        }
        else if (!strcmp(cmd,"kexec") && a1[0]) {
            hdgl_shm_t *s = substrate_shm();
            substrate_lock();
            uint32_t gfp = s->d_bits ^ cfg->genome_fp; /* live field state */
            uint32_t tk  = s->tick;
            substrate_unlock();
            cmd_kexec(a1, a2[0]?a2:"bin/hdgl_initrd.img", gfp, tk);
        }
        else if (!strcmp(cmd,"quit") || !strcmp(cmd,"exit")) {
            printf("  Stopping substrate...\n");
            cfg->running = 0;
            substrate_stop();
            break;
        }
        else {
            printf("  Unknown command: \033[1;31m%s\033[0m  (type help)\n\n", cmd);
        }
    }
}
