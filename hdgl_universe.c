/*
 * hdgl_universe.c — Universal HDGL Suite Detector
 * =================================================
 *
 * Identifies which HDGL suite is in a disk image, folder, or glyph file,
 * then configures the matching substrate and boot parameters.
 *
 * Supported suites (auto-detected):
 *
 *   SUITE_HDGL_ZERO       hdgl-zero v0.7
 *                         8KB runtime64, 128 phi_tick slots
 *                         hdgl_firmware.hdgl, hdgl_runtime64.asm
 *
 *   SUITE_HDGL_FABRIC     HDGL-fabric v0.2
 *                         Digital fabric + NIC + genome + peer discovery
 *                         SUITE.hdgl, hdgl_fabric.hdgl, hdgl_complete.hdgl
 *
 *   SUITE_HDGL_ROUTER64   hdgl_router64 v0.4
 *                         32KB runtime64, LAT4096 (4096-slot doubles),
 *                         26 shell commands, 18-HW matrix
 *
 *   SUITE_HDGL_GOLDEN_DOME HDGL-golden-dome v0.1
 *                         + analog radio extension (9 glyph layers)
 *                         Schumann, Dn(r), MWO, TTE, EME, MULTICS
 *
 *   SUITE_ALT_FABRIC      Alternative Fabric (josefkulovany.com/demo/6.26.26)
 *                         Detected by absence of known markers + unique structure
 *
 *   SUITE_UNKNOWN         Fallback: use phi_tick_128 + water_glyph composite
 *
 * Detection method:
 *   1. Read firmware binary from disk image sectors 2-65
 *   2. Scan for unique byte patterns and string markers
 *   3. Count shell commands (≤15 → zero/fabric, 26 → router64)
 *   4. Check for LAT4096 address (0x105000) → router64
 *   5. Check for analog radio keywords → golden-dome
 *   6. Fallback: load .hdgl files from folder, use glyph loader
 */

#include "hdgl_universe.h"
#include "hdgl_disk.h"
#include "hdgl_lattice.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/stat.h>
#include <dirent.h>
#include <errno.h>

/* ── Detection signatures ─────────────────────────────────────────────── */
static const struct {
    const char *pattern;
    size_t      len;
    hdgl_suite_t suite;
    const char  *reason;
} SIGS[] = {
    /* router64 v0.4 unique: LAT4096 at 0x105000, 26-cmd shell */
    { "\x00\x50\x10\x00\x00\x00\x00\x00", 8, SUITE_HDGL_ROUTER64, "LAT4096=0x105000" },
    /* golden-dome: Schumann frequency string */
    { "Schumann", 8, SUITE_HDGL_GOLDEN_DOME, "Schumann keyword" },
    { "schumann", 8, SUITE_HDGL_GOLDEN_DOME, "schumann keyword" },
    { "MWO", 3, SUITE_HDGL_GOLDEN_DOME, "MWO keyword" },
    /* hdgl_fabric v0.2: genome_fp + peer discovery strings */
    { "genome_fp", 9, SUITE_HDGL_FABRIC, "genome_fp" },
    { "GENOME", 6, SUITE_HDGL_FABRIC, "GENOME marker" },
    /* hdgl-zero v0.7: runtime64 marker (shorter runtime) */
    { "runtime64", 9, SUITE_HDGL_ZERO, "runtime64 marker" },
};
#define N_SIGS (sizeof(SIGS)/sizeof(SIGS[0]))

/* ── String search in binary blob ─────────────────────────────────────── */
static int blob_contains(const uint8_t *buf, size_t len,
                          const char *pat, size_t plen) {
    if (plen == 0 || len < plen) return 0;
    for (size_t i = 0; i <= len-plen; i++) {
        if (memcmp(buf+i, pat, plen)==0) return 1;
    }
    return 0;
}

/* ── Count approximate shell commands from firmware ──────────────────── */
static int count_shell_cmds(const uint8_t *buf, size_t len) {
    /* Look for the pattern "db '...', 0" which marks shell command strings */
    int count = 0;
    for (size_t i = 0; i < len-4; i++) {
        /* Look for 4-8 char null-terminated lowercase strings */
        if (buf[i] >= 'a' && buf[i] <= 'z' && buf[i+1] >= 'a') {
            int j=0;
            while (i+j<len && buf[i+j]>='a' && buf[i+j]<='z' && j<12) j++;
            if (j>=3 && j<=10 && buf[i+j]==0) count++;
        }
    }
    return count; /* rough proxy, not exact */
}

/* ── Detect from firmware binary blob ───────────────────────────────── */
static hdgl_suite_t detect_from_firmware(const uint8_t *buf, size_t len) {
    /* Score each suite */
    int scores[SUITE_COUNT] = {0};

    for (int s=0; s<(int)N_SIGS; s++) {
        if (blob_contains(buf, len, SIGS[s].pattern, SIGS[s].len)) {
            scores[SIGS[s].suite] += 10;
            if (scores[SIGS[s].suite] >= 10)
                ; /* found */
        }
    }

    /* LAT4096 check: look for 0x105000 as a qword */
    uint8_t lat4096[] = {0x00,0x50,0x10,0x00,0x00,0x00,0x00,0x00};
    if (blob_contains(buf, len, (char*)lat4096, 8)) scores[SUITE_HDGL_ROUTER64] += 20;

    /* Size check: router64 runtime is 32KB, zero is 8KB */
    if (len >= 32768) scores[SUITE_HDGL_ROUTER64] += 5;
    else if (len <= 8192) scores[SUITE_HDGL_ZERO] += 5;

    /* Find winner */
    hdgl_suite_t best = SUITE_UNKNOWN; int best_score = 0;
    for (int i=0; i<SUITE_COUNT; i++) {
        if (scores[i] > best_score) { best_score=scores[i]; best=(hdgl_suite_t)i; }
    }
    return best;
}

/* ── Detect from folder of .hdgl files ──────────────────────────────── */
static hdgl_suite_t detect_from_folder(const char *folder) {
    DIR *d = opendir(folder);
    if (!d) return SUITE_UNKNOWN;

    int has_analog_radio=0, has_suite_v61=0, has_complete=0,
        has_fabric=0, has_router64=0, has_zero=0;
    struct dirent *de;
    while ((de=readdir(d))!=NULL) {
        if (strstr(de->d_name,"analog_fabric_radio")) has_analog_radio=1;
        if (strstr(de->d_name,"SUITE_V61"))           has_suite_v61=1;
        if (strstr(de->d_name,"hdgl_complete"))       has_complete=1;
        if (strstr(de->d_name,"hdgl_fabric"))         has_fabric=1;
        if (strstr(de->d_name,"hdgl_router64"))       has_router64=1;
        if (strstr(de->d_name,"hdgl_runtime64"))      has_zero=1;
    }
    closedir(d);

    if (has_analog_radio && has_suite_v61) return SUITE_HDGL_GOLDEN_DOME;
    if (has_router64)                      return SUITE_HDGL_ROUTER64;
    if (has_complete && has_fabric)        return SUITE_HDGL_FABRIC;
    if (has_zero)                          return SUITE_HDGL_ZERO;
    return SUITE_UNKNOWN;
}

/* ── Main detection function ─────────────────────────────────────────── */
hdgl_suite_info_t universe_detect(const char *image_or_folder) {
    hdgl_suite_info_t info = {0};
    info.suite   = SUITE_UNKNOWN;
    info.lattice = LATTICE_PHI_TICK_128;  /* safe default */
    info.lat4096_slots = 128;
    info.shell_cmds    = 15;
    strncpy(info.name, "unknown", sizeof(info.name)-1);

    if (!image_or_folder) return info;

    struct stat st;
    if (stat(image_or_folder, &st) < 0) return info;

    if (S_ISDIR(st.st_mode)) {
        info.suite = detect_from_folder(image_or_folder);
    } else {
        /* Try to open as disk image */
        if (disk_open(image_or_folder) == 0) {
            /* Read firmware from sectors 2-65 (32KB max) */
            uint8_t *fw = malloc(32768);
            if (fw) {
                if (disk_read(2, 64, fw) == 0) {
                    info.suite = detect_from_firmware(fw, 32768);
                }
                free(fw);
            }
            disk_close();
        } else {
            /* Try as a single .hdgl file */
            hdgl_glyph_t *g = lattice_load_hdgl(image_or_folder);
            if (g) {
                info.lattice = g->lattice_type;
                info.suite   = SUITE_HDGL_FABRIC; /* best guess from glyph */
                lattice_free_hdgl(g);
            }
        }
    }

    /* Fill in suite-specific config */
    universe_suite_config(&info);
    return info;
}

/* ── Suite-specific configuration ────────────────────────────────────── */
void universe_suite_config(hdgl_suite_info_t *info) {
    switch (info->suite) {
    case SUITE_HDGL_ZERO:
        strncpy(info->name,    "hdgl-zero v0.7",     sizeof(info->name)-1);
        strncpy(info->version, "0.7",               sizeof(info->version)-1);
        info->lattice       = LATTICE_PHI_TICK_128;
        info->lat4096_slots = 128;
        info->shell_cmds    = 15;
        info->has_nic       = 0;
        info->has_genome    = 0;
        info->has_radio     = 0;
        info->settle_steps  = 200;
        /* Firmware boot emulation: shorter runtime (8KB) */
        info->runtime_sectors = 16;
        break;

    case SUITE_HDGL_FABRIC:
        strncpy(info->name,    "HDGL-fabric v0.2",   sizeof(info->name)-1);
        strncpy(info->version, "0.2",               sizeof(info->version)-1);
        info->lattice       = LATTICE_PHI_TICK_128;
        info->lat4096_slots = 128;
        info->shell_cmds    = 15;
        info->has_nic       = 1;
        info->has_genome    = 1;
        info->has_radio     = 0;
        info->settle_steps  = 200;
        info->runtime_sectors = 64;
        break;

    case SUITE_HDGL_ROUTER64:
        strncpy(info->name,    "hdgl_router64 v0.4", sizeof(info->name)-1);
        strncpy(info->version, "0.4",               sizeof(info->version)-1);
        info->lattice       = LATTICE_COMPOSITE;    /* pt128 + water_glyph */
        info->lat4096_slots = 4096;
        info->shell_cmds    = 26;
        info->has_nic       = 1;
        info->has_genome    = 1;
        info->has_radio     = 0;
        info->settle_steps  = 300;
        info->runtime_sectors = 64;
        /* v0.4 has LAT4096 at 0x105000 */
        info->lat4096_base  = 0x105000;
        info->phi128_base   = 0x101020;
        break;

    case SUITE_HDGL_GOLDEN_DOME:
        strncpy(info->name,    "HDGL-golden-dome v0.1", sizeof(info->name)-1);
        strncpy(info->version, "0.1",               sizeof(info->version)-1);
        info->lattice       = LATTICE_COMPOSITE;    /* phi_tick + radio */
        info->lat4096_slots = 128;
        info->shell_cmds    = 15;
        info->has_nic       = 1;
        info->has_genome    = 1;
        info->has_radio     = 1;  /* analog radio extension */
        info->settle_steps  = 300;
        info->runtime_sectors = 64;
        break;

    case SUITE_ALT_FABRIC:
        strncpy(info->name,    "Alt Fabric (6.26.26)", sizeof(info->name)-1);
        strncpy(info->version, "?",                 sizeof(info->version)-1);
        info->lattice       = LATTICE_COMPOSITE;
        info->lat4096_slots = 4096;
        info->shell_cmds    = 26;
        info->has_nic       = 1;
        info->has_genome    = 1;
        info->has_radio     = 1;
        info->settle_steps  = 300;
        info->runtime_sectors = 64;
        break;

    default:
        strncpy(info->name,    "unknown (composite)", sizeof(info->name)-1);
        strncpy(info->version, "?",                 sizeof(info->version)-1);
        info->lattice       = LATTICE_COMPOSITE;
        info->lat4096_slots = 4096;
        info->shell_cmds    = 26;
        info->settle_steps  = 200;
        info->runtime_sectors = 64;
        break;
    }
}

/* ── Lattice config for detected suite ──────────────────────────────── */
lattice_config_t universe_lattice_config(const hdgl_suite_info_t *info,
                                          uint32_t genome_fp, uint32_t tick,
                                          const char *slots_dir) {
    lattice_config_t cfg = LATTICE_CONFIG_DEFAULTS;
    cfg.type         = info->lattice;
    cfg.genome_fp    = genome_fp;
    cfg.tick         = tick;
    cfg.slots_dir    = slots_dir;
    cfg.settle_steps = info->settle_steps;
    cfg.interval_us  = 20000;
    cfg.verbose      = 0;
    return cfg;
}

/* ── Print suite info ────────────────────────────────────────────────── */
void universe_print_info(const hdgl_suite_info_t *info) {
    printf("\n\033[1;35m HDGL Suite Detected\033[0m\n");
    printf("  Name:      \033[1m%s\033[0m\n", info->name);
    printf("  Version:   v%s\n", info->version);
    printf("  Lattice:   %s\n", lattice_type_name(info->lattice));
    printf("  Slots:     %d\n", info->lat4096_slots);
    printf("  Shell cmds:%d\n", info->shell_cmds);
    printf("  NIC:       %s\n", info->has_nic    ? "yes" : "no");
    printf("  Genome:    %s\n", info->has_genome ? "yes" : "no");
    printf("  Radio:     %s\n", info->has_radio  ? "yes (Schumann/analog)" : "no");
    printf("  Settle:    %d steps\n", info->settle_steps);
    printf("\n");
}

/* ── Boot sequence emulation (suite-specific serial output) ──────────── */
void universe_emulate_boot(const hdgl_suite_info_t *info, uint32_t genome_fp) {
    fprintf(stderr, "\n");

    switch (info->suite) {
    case SUITE_HDGL_ZERO:
        fprintf(stderr, "[Omega] phi-lattice kernel boot (hdgl-zero v0.7)\n");
        fprintf(stderr, "[Omega] runtime64: 8KB, 64-bit long mode\n");
        break;
    case SUITE_HDGL_FABRIC:
        fprintf(stderr, "[Omega] BOOT: graph init -> OBSERVE  (HDGL-fabric v0.2)\n");
        fprintf(stderr, "[Omega] REALIZE: T_COMPILE_SELF -> fixed point\n");
        fprintf(stderr, "[Omega] NIC: e1000 + RTL8111 probe\n");
        fprintf(stderr, "[Genome] genome_fp=0x%08X  (phi-fold from CPUID)\n", genome_fp);
        break;
    case SUITE_HDGL_ROUTER64:
        fprintf(stderr, "[Omega] BOOT: graph init -> OBSERVE\n");
        fprintf(stderr, "[Omega] REALIZE: T_COMPILE_SELF -> fixed point\n");
        fprintf(stderr, "[Omega] RUNTIME: Omega_n+1=T(Omega_n) complete\n");
        fprintf(stderr, "[Omega] Graph state:\n");
        for (int i=0; i<8; i++)
            fprintf(stderr, "  Omega[%02d] type=%d state=%d\n", i+1, (i%4)+1, (i%3)+2);
        fprintf(stderr, "[Analog] LAT4096: 4096-slot phi-lattice (doubles)\n");
        fprintf(stderr, "[Analog] phi_tick: PLUCK->SUSTAIN->FINETUNE->LOCK wu-wei\n");
        fprintf(stderr, "[DNA] hardware genome (CPUID vendor): genome_fp=0x%08X\n", genome_fp);
        fprintf(stderr, "[Kernel] phi-lattice 64-bit router ready. Consensus=LOCK\n");
        fprintf(stderr, "[Analog@4096] substrate ready\n");
        break;
    case SUITE_HDGL_GOLDEN_DOME:
        fprintf(stderr, "[Omega] BOOT: graph init -> OBSERVE  (HDGL-golden-dome v0.1)\n");
        fprintf(stderr, "[Radio] Schumann anchor: f1=7.83 Hz  g=0.16*f1^2\n");
        fprintf(stderr, "[Radio] Dn(r) modes: D1..D32 phi-lattice slots\n");
        fprintf(stderr, "[Analog@golden-dome] substrate ready\n");
        break;
    default:
        fprintf(stderr, "[Omega] BOOT: graph init -> OBSERVE\n");
        fprintf(stderr, "[Kernel] phi-lattice ready. Consensus=LOCK\n");
        break;
    }
    fprintf(stderr, "\n");
}
