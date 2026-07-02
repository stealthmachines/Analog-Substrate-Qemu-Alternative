/*
 * hdgl_disk.c — Disk Image Reader
 * ================================
 *
 * Reads sectors from a raw disk image (hdgl_router64.img) and provides
 * the same interface that the firmware's ATA PIO routines use.
 *
 * Disk layout (from build.sh):
 *   Sector 0:     MBR (512B)
 *   Sector 1:     Stage2 (512B)
 *   Sectors 2-65: Runtime64 (32KB)
 *   Sector 66+:   Firmware HDGL source
 *   Sector 512:   Alpine kernel bzImage
 *   Sector 16896: initramfs
 *   (+ precomputed tables at build_tables.sh output LBAs)
 *
 * Implements:
 *   disk_open()        — open disk image
 *   disk_read()        — read N sectors from LBA into buffer
 *   disk_scan()        — probe disk for kernel/initrd/genome_fp
 *   disk_load_kernel() — load bzImage kernel
 *   disk_load_initrd() — load initramfs
 *   disk_validate_bzimage() — validate HdrS header
 */

#include "hdgl_disk.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

#define SECTOR_SIZE     512
#define MBR_LBA         0
#define STAGE2_LBA      1
#define RUNTIME64_LBA   2
#define RUNTIME64_SECTS 64       /* 32KB */
#define FIRMWARE_LBA    66
#define KERNEL_LBA_DEF  512
#define INITRD_LBA_DEF  16896

/* bzImage header offsets */
#define BZIMG_BOOT_FLAG   0x1FE  /* must be 0xAA55 */
#define BZIMG_HEADER      0x202  /* must be "HdrS" */
#define BZIMG_SETUP_SECTS 0x1F1  /* number of 512B setup sectors */
#define BZIMG_SYSSIZE     0x1F4  /* protected-mode code size in 16B units */
#define BZIMG_XLOADFLAGS  0x236  /* XLF_KERNEL_64 = bit 0 */
#define BZIMG_XLF_64BIT   0x0001

static int g_fd = -1;
static char g_image_path[512];
static uint64_t g_image_sectors;

/* ── Open disk image ──────────────────────────────────────────────────── */
int disk_open(const char *path) {
    struct stat st;
    if (stat(path, &st) < 0) {
        fprintf(stderr, "[disk] cannot stat '%s': %s\n", path, strerror(errno));
        return -1;
    }
    g_fd = open(path, O_RDONLY);
    if (g_fd < 0) {
        fprintf(stderr, "[disk] cannot open '%s': %s\n", path, strerror(errno));
        return -1;
    }
    g_image_sectors = (uint64_t)st.st_size / SECTOR_SIZE;
    strncpy(g_image_path, path, sizeof(g_image_path)-1);
    fprintf(stderr, "[disk] opened '%s': %llu sectors (%llu MB)\n",
            path,
            (unsigned long long)g_image_sectors,
            (unsigned long long)(g_image_sectors * SECTOR_SIZE / (1024*1024)));
    return 0;
}

void disk_close(void) {
    if (g_fd >= 0) { close(g_fd); g_fd = -1; }
}

int disk_is_open(void) { return g_fd >= 0; }
const char *disk_image_path(void) { return g_image_path; }

/* ── Read sectors ─────────────────────────────────────────────────────── */
int disk_read(uint64_t lba, uint32_t count, void *buf) {
    if (g_fd < 0) {
        fprintf(stderr, "[disk] disk_read: no image open\n");
        return -1;
    }
    if (lba + count > g_image_sectors) {
        fprintf(stderr, "[disk] disk_read: LBA %llu+%u exceeds image size %llu\n",
                (unsigned long long)lba, count,
                (unsigned long long)g_image_sectors);
        return -1;
    }
    off_t offset = (off_t)lba * SECTOR_SIZE;
    if (lseek(g_fd, offset, SEEK_SET) < 0) {
        fprintf(stderr, "[disk] seek to LBA %llu failed: %s\n",
                (unsigned long long)lba, strerror(errno));
        return -1;
    }
    size_t total = (size_t)count * SECTOR_SIZE;
    ssize_t got = read(g_fd, buf, total);
    if (got < 0) {
        fprintf(stderr, "[disk] read LBA %llu count %u failed: %s\n",
                (unsigned long long)lba, count, strerror(errno));
        return -1;
    }
    if ((size_t)got < total) {
        fprintf(stderr, "[disk] short read: got %zu of %zu bytes at LBA %llu\n",
                (size_t)got, total, (unsigned long long)lba);
        /* Zero-pad the remainder */
        memset((uint8_t*)buf + got, 0, total - got);
    }
    return 0;
}

/* ── Validate bzImage ─────────────────────────────────────────────────── */
int disk_validate_bzimage(const uint8_t *hdr, size_t hdr_size, bzimage_info_t *info) {
    if (hdr_size < 0x240) {
        fprintf(stderr, "[disk] bzImage header too small: %zu bytes\n", hdr_size);
        return -1;
    }
    /* boot_flag at 0x1FE */
    uint16_t boot_flag = *(uint16_t*)(hdr + BZIMG_BOOT_FLAG);
    if (boot_flag != 0xAA55) {
        fprintf(stderr, "[disk] bad boot_flag: 0x%04X (expected 0xAA55)\n", boot_flag);
        return -1;
    }
    /* HdrS magic at 0x202 */
    if (memcmp(hdr + BZIMG_HEADER, "HdrS", 4) != 0) {
        fprintf(stderr, "[disk] bad HdrS magic at 0x202: %02X %02X %02X %02X\n",
                hdr[0x202], hdr[0x203], hdr[0x204], hdr[0x205]);
        return -1;
    }
    /* setup_sects at 0x1F1 */
    uint8_t setup_sects = hdr[BZIMG_SETUP_SECTS];
    if (setup_sects == 0) setup_sects = 4; /* spec default */

    /* xloadflags at 0x236 */
    uint16_t xlf = *(uint16_t*)(hdr + BZIMG_XLOADFLAGS);
    if (!(xlf & BZIMG_XLF_64BIT)) {
        fprintf(stderr, "[disk] kernel lacks XLF_KERNEL_64 (xloadflags=0x%04X)\n", xlf);
        return -1;
    }
    /* syssize at 0x1F4 (units of 16 bytes) */
    uint32_t syssize = *(uint32_t*)(hdr + BZIMG_SYSSIZE);

    if (info) {
        info->setup_sects  = setup_sects;
        info->setup_size   = (uint32_t)(setup_sects + 1) * SECTOR_SIZE;
        info->syssize      = syssize;
        info->pm_size      = (uint32_t)syssize * 16;
        info->xloadflags   = xlf;
        info->boot_flag    = boot_flag;
    }
    return 0;
}

/* ── Load kernel from disk image ─────────────────────────────────────── */
/*
 * Reads the bzImage from the disk image at lba.
 * Allocates *out_buf (caller must free).
 * Returns size of protected-mode kernel (excluding setup code), or -1.
 */
ssize_t disk_load_kernel(uint64_t lba, uint8_t **out_buf, size_t *out_size,
                         bzimage_info_t *info) {
    if (g_fd < 0) { fprintf(stderr,"[disk] no image\n"); return -1; }

    /* Read first 2 sectors to get the header */
    uint8_t hdr[1024];
    if (disk_read(lba, 2, hdr) < 0) return -1;

    bzimage_info_t bi;
    if (disk_validate_bzimage(hdr, sizeof(hdr), &bi) < 0) return -1;
    if (info) *info = bi;

    /* Protected-mode kernel starts after setup code:
     * PM start = lba + setup_sects + 1 */
    uint64_t pm_lba = lba + bi.setup_sects + 1;
    uint32_t pm_sectors = (bi.pm_size + SECTOR_SIZE - 1) / SECTOR_SIZE;

    /* Sanity: cap at 32MB */
    if (pm_sectors > 65536) {
        fprintf(stderr, "[disk] kernel too large: %u sectors\n", pm_sectors);
        return -1;
    }
    /* Also read the setup code (first part, for boot_params copying) */
    uint32_t setup_sectors = bi.setup_sects + 1;
    uint32_t total_sectors = setup_sectors + pm_sectors;
    size_t   total_bytes   = (size_t)total_sectors * SECTOR_SIZE;

    uint8_t *buf = malloc(total_bytes);
    if (!buf) { perror("[disk] malloc kernel buffer"); return -1; }

    /* Read setup code first */
    if (disk_read(lba, setup_sectors, buf) < 0) { free(buf); return -1; }
    /* Read protected-mode code after setup */
    if (disk_read(pm_lba, pm_sectors, buf + (size_t)setup_sectors*SECTOR_SIZE) < 0) {
        free(buf); return -1;
    }

    fprintf(stderr, "[disk] kernel: setup=%u sects  pm=%u sects (%u MB)  xlf=0x%04X\n",
            setup_sectors, pm_sectors, bi.pm_size/(1024*1024), bi.xloadflags);

    *out_buf  = buf;
    *out_size = total_bytes;
    return (ssize_t)total_bytes;
}

/* ── Load initrd from disk image ──────────────────────────────────────── */
ssize_t disk_load_initrd(uint64_t lba, uint8_t **out_buf, size_t *out_size) {
    if (g_fd < 0) { fprintf(stderr,"[disk] no image\n"); return -1; }

    /* Read first sector to detect if there's actually data here */
    uint8_t probe[512];
    if (disk_read(lba, 1, probe) < 0) return -1;

    /* Check for gzip magic (initrd is gzipped cpio) */
    int is_gz   = (probe[0]==0x1F && probe[1]==0x8B);
    /* Check for cpio newc magic */
    int is_cpio = (memcmp(probe, "070701", 6)==0 || memcmp(probe, "070702", 6)==0);

    if (!is_gz && !is_cpio) {
        fprintf(stderr,"[disk] initrd at LBA %llu: no gzip/cpio magic "
                "(bytes: %02X %02X %02X %02X)\n",
                (unsigned long long)lba,
                probe[0], probe[1], probe[2], probe[3]);
        fprintf(stderr,"[disk] trying anyway — may be uncompressed or different format\n");
    }

    /* Read up to 64MB */
    uint32_t max_sects = 131072; /* 64MB */
    uint32_t avail = (uint32_t)(g_image_sectors - lba);
    if (avail < max_sects) max_sects = avail;

    /* Find end of initrd by scanning for non-zero content */
    /* Efficient: read in 4MB chunks, stop at first all-zero chunk */
    size_t chunk = 8192; /* 4MB chunk in sectors */
    uint8_t *buf = NULL;
    size_t   total = 0;
    uint64_t cur_lba = lba;

    while (cur_lba < lba + max_sects) {
        uint32_t n = (uint32_t)((lba + max_sects) - cur_lba);
        if (n > chunk) n = chunk;
        uint8_t *tmp = realloc(buf, total + n*SECTOR_SIZE);
        if (!tmp) { free(buf); perror("[disk] realloc"); return -1; }
        buf = tmp;
        if (disk_read(cur_lba, n, buf + total) < 0) { free(buf); return -1; }
        /* Check if this chunk is all zero (end of initrd) */
        int all_zero = 1;
        for (size_t i = total; i < total + n*SECTOR_SIZE; i++) {
            if (buf[i]) { all_zero = 0; break; }
        }
        total += n * SECTOR_SIZE;
        cur_lba += n;
        if (all_zero && total > SECTOR_SIZE) break; /* found end */
        if (total > 64*1024*1024) break; /* safety cap */
    }

    /* Trim trailing zeros */
    while (total > 0 && buf[total-1] == 0) total--;
    /* Align to sector */
    if (total % SECTOR_SIZE) total = ((total / SECTOR_SIZE) + 1) * SECTOR_SIZE;

    fprintf(stderr, "[disk] initrd: %zu bytes (%zu KB) at LBA %llu\n",
            total, total/1024, (unsigned long long)lba);

    *out_buf  = buf;
    *out_size = total;
    return (ssize_t)total;
}

/* ── Scan disk image for content ──────────────────────────────────────── */
disk_scan_t disk_scan(void) {
    disk_scan_t result = {0};
    result.kernel_lba = KERNEL_LBA_DEF;
    result.initrd_lba = INITRD_LBA_DEF;

    if (g_fd < 0) return result;

    /* Verify MBR signature */
    uint8_t mbr[512];
    if (disk_read(0, 1, mbr) == 0) {
        result.has_mbr = (mbr[510]==0x55 && mbr[511]==0xAA);
    }

    /* Verify Runtime64 signature: check for HDGL magic or valid firmware */
    uint8_t rt[512];
    if (disk_read(2, 1, rt) == 0) {
        /* Runtime64 starts with some jump + OMEGA section */
        result.has_runtime = (rt[0]==0xEB || rt[0]==0xE9); /* JMP opcode */
    }

    /* Check for kernel at default LBA */
    if (g_image_sectors > KERNEL_LBA_DEF + 2) {
        uint8_t khdr[1024];
        if (disk_read(KERNEL_LBA_DEF, 2, khdr) == 0) {
            result.has_kernel = (khdr[0x1FE]==0x55 && khdr[0x1FF]==0xAA &&
                                 memcmp(khdr+0x202,"HdrS",4)==0);
            if (result.has_kernel) {
                bzimage_info_t bi;
                if (disk_validate_bzimage(khdr, sizeof(khdr), &bi) == 0) {
                    result.kernel_size   = bi.pm_size;
                    result.kernel_64bit  = (bi.xloadflags & BZIMG_XLF_64BIT);
                    result.kernel_setup  = bi.setup_sects;
                }
            }
        }
    }

    /* Check for initrd at default LBA */
    if (g_image_sectors > INITRD_LBA_DEF + 1) {
        uint8_t ihdr[512];
        if (disk_read(INITRD_LBA_DEF, 1, ihdr) == 0) {
            result.has_initrd = (ihdr[0]==0x1F && ihdr[1]==0x8B) || /* gzip */
                                (memcmp(ihdr,"070701",6)==0);         /* cpio */
        }
    }

    fprintf(stderr,"[disk] scan: MBR=%d Runtime64=%d Kernel@%d=%d Initrd@%d=%d\n",
            result.has_mbr, result.has_runtime,
            result.kernel_lba, result.has_kernel,
            result.initrd_lba, result.has_initrd);
    return result;
}

/* ── Read firmware HDGL source (sector 66+) ──────────────────────────── */
ssize_t disk_read_firmware_source(uint8_t **out_buf, size_t *out_size) {
    if (g_fd < 0) return -1;
    uint32_t avail = (uint32_t)(g_image_sectors - FIRMWARE_LBA);
    if (avail > 512) avail = 512; /* read up to 256KB of source */
    size_t sz = (size_t)avail * SECTOR_SIZE;
    uint8_t *buf = malloc(sz);
    if (!buf) return -1;
    if (disk_read(FIRMWARE_LBA, avail, buf) < 0) { free(buf); return -1; }
    /* Find end of text content (null or 0x1A/EOF marker) */
    size_t end = sz;
    for (size_t i = 0; i < sz-1; i++) {
        if (buf[i]==0 && buf[i+1]==0) { end = i; break; }
    }
    *out_buf  = buf;
    *out_size = end;
    return (ssize_t)end;
}
