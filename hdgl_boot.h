/*
 * hdgl_boot.h — Boot Engine API
 */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <sys/syscall.h>

/* E820 memory map entry (20 bytes, matches Linux struct e820_entry) */
typedef struct __attribute__((packed)) {
    uint64_t base;
    uint64_t size;
    uint32_t type; /* 1=RAM 2=reserved 3=ACPI 4=ACPI NVS */
} e820_entry_t;

/* Boot method */
typedef enum {
    BOOT_METHOD_AUTO         = 0,
    BOOT_METHOD_KEXEC_SYSCALL = 1,  /* kexec_file_load() syscall — fastest */
    BOOT_METHOD_KEXEC_UTIL   = 2,   /* /sbin/kexec utility */
    BOOT_METHOD_QEMU_KERNEL  = 3,   /* qemu -kernel (substrate outside VM) */
} boot_method_t;

typedef struct {
    const char    *kernel_path;   /* path to bzImage, or NULL to load from disk */
    const char    *initrd_path;   /* path to initramfs, or NULL to load from disk */
    const char    *extra_args;    /* appended to cmdline */
    uint32_t       genome_fp;     /* base genome fingerprint */
    uint32_t       tick;          /* base tick */
    int            kernel_lba;    /* LBA in disk image if kernel_path=NULL */
    int            initrd_lba;    /* LBA in disk image if initrd_path=NULL */
    boot_method_t  force_method;  /* BOOT_METHOD_AUTO = auto-detect */
    int            no_fallback;   /* 0 = try next method on failure */
    int            use_live_substrate; /* 1 = use live SHM genome_fp */
    int            mem_mb;        /* RAM for QEMU method (default 512) */
} boot_config_t;

#define BOOT_CONFIG_DEFAULTS { \
    .kernel_path        = NULL, \
    .initrd_path        = NULL, \
    .extra_args         = NULL, \
    .genome_fp          = 0x88888888, \
    .tick               = 0, \
    .kernel_lba         = 512, \
    .initrd_lba         = 16896, \
    .force_method       = BOOT_METHOD_AUTO, \
    .no_fallback        = 0, \
    .use_live_substrate = 1, \
    .mem_mb             = 512 \
}

/* API */
void          boot_build_cmdline(char *buf, size_t bufsz,
                                  uint32_t genome_fp, uint32_t tick,
                                  const char *extra);
int           boot_build_e820(e820_entry_t *entries, int max_entries);
boot_method_t boot_detect_method(void);
const char   *boot_method_name(boot_method_t m);
int           boot_run(boot_config_t *cfg);
void          boot_print_status(void);
