/*
 * hdgl_disk.h — Disk Image Reader API
 */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <sys/types.h>

/* bzImage header information */
typedef struct {
    uint8_t  setup_sects;   /* number of 512B setup sectors (0x1F1) */
    uint32_t setup_size;    /* = (setup_sects+1) × 512 */
    uint32_t syssize;       /* protected-mode size in 16B units (0x1F4) */
    uint32_t pm_size;       /* = syssize × 16 (bytes) */
    uint16_t xloadflags;    /* 0x236: bit0=XLF_KERNEL_64 */
    uint16_t boot_flag;     /* 0x1FE: 0xAA55 */
} bzimage_info_t;

/* Disk scan results */
typedef struct {
    int      has_mbr;
    int      has_runtime;
    int      has_kernel;
    int      has_initrd;
    int      kernel_lba;
    int      initrd_lba;
    uint32_t kernel_size;    /* protected-mode bytes */
    int      kernel_64bit;
    uint8_t  kernel_setup;
} disk_scan_t;

/* Default LBAs */
#define DISK_KERNEL_LBA_DEFAULT  512
#define DISK_INITRD_LBA_DEFAULT  16896

/* Open/close */
int          disk_open(const char *path);
void         disk_close(void);
int          disk_is_open(void);
const char  *disk_image_path(void);

/* Sector I/O */
int disk_read(uint64_t lba, uint32_t count, void *buf);

/* Kernel/initrd loading (allocates buffer, caller frees) */
int      disk_validate_bzimage(const uint8_t *hdr, size_t hdr_size,
                                bzimage_info_t *info);
ssize_t  disk_load_kernel(uint64_t lba, uint8_t **out_buf, size_t *out_size,
                           bzimage_info_t *info);
ssize_t  disk_load_initrd(uint64_t lba, uint8_t **out_buf, size_t *out_size);
ssize_t  disk_read_firmware_source(uint8_t **out_buf, size_t *out_size);

/* Scan disk for content */
disk_scan_t disk_scan(void);
