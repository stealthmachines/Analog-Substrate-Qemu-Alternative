/*
 * hdgl_shell.h — HDGL Shell API
 */
#pragma once
#include <stdint.h>

typedef struct {
    uint32_t    genome_fp;
    const char *slots_dir;
    int         running;
} shell_config_t;

void shell_run(shell_config_t *cfg);
void shell_term_raw(void);
void shell_term_restore(void);
void cmd_kexec(const char *kernel, const char *initrd,
               uint32_t genome_fp, uint32_t tick);
