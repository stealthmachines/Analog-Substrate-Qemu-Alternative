/*
 * hdgl_boot.c — Reliable Kernel Boot Engine
 * ===========================================
 *
 * Loads kernel + initrd (from disk image or filesystem path) and boots
 * them using the most reliable method available on this system:
 *
 *   Method 1: kexec_file_load() syscall (requires root, Linux >= 3.17)
 *   Method 2: kexec utility (kexec -l + kexec -e, requires kexec-tools)
 *   Method 3: qemu-system-x86_64 with -kernel -initrd (no full VM for substrate)
 *
 * The genome_fp and tick in the cmdline come from the LIVE substrate SHM,
 * not from a static config value. The kernel receives the actual field state
 * at the moment of boot.
 *
 * E820 map is built from /proc/iomem to match the real memory layout.
 *
 * Alpine's /init reads hdgl.dn and hdgl.tick from /proc/cmdline.
 * phi_pool starts and reads /lattice/slots/ for the settled eigenmode values.
 */

#include "hdgl_boot.h"
#include "hdgl_disk.h"
#include "phi_substrate.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/reboot.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <linux/reboot.h>

/* kexec_file_load syscall number (x86_64) */
#ifndef __NR_kexec_file_load
#define __NR_kexec_file_load 320
#endif
#define KEXEC_FILE_NO_INITRAMFS  0x4

/* ── Build kernel cmdline ─────────────────────────────────────────────── */
void boot_build_cmdline(char *buf, size_t bufsz,
                        uint32_t genome_fp, uint32_t tick,
                        const char *extra) {
    snprintf(buf, bufsz,
             "earlyprintk=ttyS0,9600 console=ttyS0,9600n8 "
             "rdinit=/init quiet "
             "hdgl.dn=%08X hdgl.tick=%08X"
             "%s%s",
             genome_fp, tick,
             extra && extra[0] ? " " : "",
             extra ? extra : "");
}

/* ── Read /proc/iomem → E820-style entries ───────────────────────────── */
/*
 * Parses /proc/iomem to build an E820 map matching the system's real memory.
 * This is what the firmware reads from BIOS at boot — we get the same info
 * from the running kernel's iomem.
 *
 * Returns number of entries written (≤ max_entries), or -1 on failure.
 */
int boot_build_e820(e820_entry_t *entries, int max_entries) {
    FILE *f = fopen("/proc/iomem", "r");
    if (!f) {
        /* Fallback: synthesize minimal E820 for 512MB */
        if (max_entries >= 3) {
            entries[0].base = 0x0000;      entries[0].size = 0x9F000;  entries[0].type = 1;
            entries[1].base = 0x9F000;     entries[1].size = 0x1000;   entries[1].type = 2;
            entries[2].base = 0x100000;    entries[2].size = 0x1FF00000; entries[2].type = 1;
        }
        return 3;
    }

    int n = 0;
    char line[256];
    while (fgets(line, sizeof(line), f) && n < max_entries) {
        uint64_t base, end;
        char name[128];
        /* Format: "  00000000-0009ffff : System RAM" */
        if (sscanf(line, " %llx-%llx : %127[^\n]",
                   (unsigned long long*)&base,
                   (unsigned long long*)&end, name) != 3) continue;
        /* Only top-level entries (no sub-entries like "Kernel code") */
        if (line[0] == ' ' && line[1] == ' ' && line[2] != ' ') {
            uint64_t size = end - base + 1;
            uint32_t type = 2; /* reserved by default */
            if (strstr(name, "System RAM") ||
                strstr(name, "RAM buffer"))      type = 1;
            else if (strstr(name, "ACPI Tables")) type = 3;
            else if (strstr(name, "ACPI Non-volatile")) type = 4;
            entries[n].base = base;
            entries[n].size = size;
            entries[n].type = type;
            n++;
        }
    }
    fclose(f);
    if (n == 0) {
        /* Last-resort fallback */
        entries[0].base = 0; entries[0].size = 640*1024; entries[0].type = 1;
        n = 1;
    }
    return n;
}

/* ── Check kexec availability ─────────────────────────────────────────── */
boot_method_t boot_detect_method(void) {
    /* Check root */
    if (geteuid() != 0) {
        /* Non-root: can't use kexec_file_load or kexec utility directly */
        /* Check if kexec is setuid */
        struct stat st;
        if (stat("/sbin/kexec", &st)==0 && (st.st_mode & S_ISUID)) {
            return BOOT_METHOD_KEXEC_UTIL;
        }
        /* Fall through to QEMU */
        return BOOT_METHOD_QEMU_KERNEL;
    }
    /* Root: try kexec_file_load syscall first */
    /* Test by calling with NULL args — will fail with EINVAL if supported */
    long rc = syscall(__NR_kexec_file_load, -1, -1, 0, NULL, 0);
    if (rc < 0 && (errno == EINVAL || errno == EBADF || errno == EPERM)) {
        return BOOT_METHOD_KEXEC_SYSCALL;
    }
    /* Try kexec utility */
    if (access("/sbin/kexec", X_OK)==0 || access("/usr/sbin/kexec", X_OK)==0) {
        return BOOT_METHOD_KEXEC_UTIL;
    }
    return BOOT_METHOD_QEMU_KERNEL;
}

const char *boot_method_name(boot_method_t m) {
    switch (m) {
    case BOOT_METHOD_KEXEC_SYSCALL: return "kexec_file_load() syscall";
    case BOOT_METHOD_KEXEC_UTIL:    return "kexec utility";
    case BOOT_METHOD_QEMU_KERNEL:   return "qemu-system-x86_64 -kernel";
    default:                        return "unknown";
    }
}

/* ── Write tempfile ───────────────────────────────────────────────────── */
static int write_tmpfile(const char *prefix, const uint8_t *data, size_t size,
                          char *path_out, size_t path_size) {
    snprintf(path_out, path_size, "/tmp/%s_XXXXXX", prefix);
    int fd = mkstemp(path_out);
    if (fd < 0) { perror("mkstemp"); return -1; }
    size_t written = 0;
    while (written < size) {
        ssize_t n = write(fd, data+written, size-written);
        if (n<0 && errno==EINTR) continue;
        if (n<=0) { perror("write tmpfile"); close(fd); return -1; }
        written += n;
    }
    close(fd);
    return 0;
}

/* ── Method 1: kexec_file_load() syscall ─────────────────────────────── */
static int boot_kexec_syscall(const char *kernel_path, const char *initrd_path,
                               const char *cmdline) {
    int kfd = open(kernel_path, O_RDONLY);
    if (kfd<0) { fprintf(stderr,"[boot] open kernel '%s': %s\n",kernel_path,strerror(errno)); return -1; }

    int ifd = -1;
    unsigned long flags = 0;
    if (initrd_path && initrd_path[0]) {
        ifd = open(initrd_path, O_RDONLY);
        if (ifd<0) { fprintf(stderr,"[boot] open initrd '%s': %s\n",initrd_path,strerror(errno)); close(kfd); return -1; }
    } else {
        flags |= KEXEC_FILE_NO_INITRAMFS;
    }

    size_t cmdlen = strlen(cmdline)+1;
    fprintf(stderr,"[boot] kexec_file_load: kernel=%s initrd=%s\n",kernel_path,initrd_path?initrd_path:"(none)");
    fprintf(stderr,"[boot] cmdline: %s\n", cmdline);

    long rc = syscall(__NR_kexec_file_load, kfd, ifd, cmdlen, cmdline, flags);
    close(kfd); if (ifd>=0) close(ifd);
    if (rc<0) { fprintf(stderr,"[boot] kexec_file_load failed: %s\n",strerror(errno)); return -1; }

    fprintf(stderr,"[boot] kernel loaded. Executing kexec...\n");
    rc = reboot(LINUX_REBOOT_CMD_KEXEC);
    fprintf(stderr,"[boot] kexec reboot: %s\n", strerror(errno));
    return (int)rc; /* does not return on success */
}

/* ── Method 2: kexec utility ─────────────────────────────────────────── */
static int boot_kexec_util(const char *kernel_path, const char *initrd_path,
                            const char *cmdline) {
    const char *kexec = access("/sbin/kexec",X_OK)==0 ? "/sbin/kexec" : "/usr/sbin/kexec";
    char load_cmd[2048], exec_cmd[256];

    if (initrd_path && initrd_path[0]) {
        snprintf(load_cmd, sizeof(load_cmd),
                 "%s -l '%s' --initrd='%s' --append='%s' 2>&1",
                 kexec, kernel_path, initrd_path, cmdline);
    } else {
        snprintf(load_cmd, sizeof(load_cmd),
                 "%s -l '%s' --append='%s' 2>&1",
                 kexec, kernel_path, cmdline);
    }
    snprintf(exec_cmd, sizeof(exec_cmd), "%s -e 2>&1", kexec);

    fprintf(stderr,"[boot] %s\n", load_cmd);
    int rc = system(load_cmd);
    if (rc != 0) { fprintf(stderr,"[boot] kexec load failed (rc=%d)\n",rc); return -1; }

    fprintf(stderr,"[boot] %s\n", exec_cmd);
    system(exec_cmd); /* does not return on success */
    return -1;
}

/* ── Method 3: QEMU -kernel (substrate outside VM) ───────────────────── */
static int boot_qemu_kernel(const char *kernel_path, const char *initrd_path,
                             const char *cmdline, int mem_mb) {
    char *args[32];
    int  na = 0;
    char mem_str[32], append_str[1024];

    snprintf(mem_str,   sizeof(mem_str),   "%dM", mem_mb);
    snprintf(append_str, sizeof(append_str), "%s", cmdline);

    char qemu[256];
    if (access("/usr/bin/qemu-system-x86_64",X_OK)==0)
        strncpy(qemu,"/usr/bin/qemu-system-x86_64",sizeof(qemu));
    else strncpy(qemu,"qemu-system-x86_64",sizeof(qemu));

    args[na++] = qemu;
    args[na++] = "-kernel";  args[na++] = (char*)kernel_path;
    if (initrd_path && initrd_path[0]) {
        args[na++] = "-initrd"; args[na++] = (char*)initrd_path;
    }
    args[na++] = "-append";   args[na++] = append_str;
    args[na++] = "-m";        args[na++] = mem_str;
    args[na++] = "-serial";   args[na++] = "stdio";
    args[na++] = "-no-reboot";
    args[na++] = "-display";  args[na++] = "none";
    args[na++] = NULL;

    fprintf(stderr,"[boot] qemu -kernel '%s' -initrd '%s'\n",
            kernel_path, initrd_path?initrd_path:"");
    fprintf(stderr,"[boot] cmdline: %s\n", cmdline);
    fprintf(stderr,"[boot] Note: analog substrate runs in THIS process while QEMU boots Alpine\n");

    pid_t pid = fork();
    if (pid<0) { perror("fork"); return -1; }
    if (pid==0) {
        execvp(qemu, args);
        perror("execvp qemu"); _exit(1);
    }
    int status;
    waitpid(pid, &status, 0);
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

/* ── Main boot function ───────────────────────────────────────────────── */
int boot_run(boot_config_t *cfg) {
    char cmdline[1024];
    char kernel_tmp[256]="", initrd_tmp[256]="";
    const char *kernel_path = cfg->kernel_path;
    const char *initrd_path = cfg->initrd_path;
    int cleanup_kernel=0, cleanup_initrd=0;

    /* Get live genome_fp and tick from substrate */
    uint32_t genome_fp = cfg->genome_fp;
    uint32_t tick      = cfg->tick;
    if (cfg->use_live_substrate) {
        hdgl_shm_t *shm = substrate_shm();
        substrate_lock();
        genome_fp = shm->d_bits ^ cfg->genome_fp; /* field-perturbed identity */
        tick      = shm->tick;
        int active = shm->active_modes;
        substrate_unlock();
        fprintf(stderr,"[boot] live substrate: genome_fp=0x%08X tick=%u active=%d\n",
                genome_fp, tick, active);
    }

    boot_build_cmdline(cmdline, sizeof(cmdline), genome_fp, tick, cfg->extra_args);

    /* If kernel is a disk image LBA reference, extract to tempfile */
    if (!kernel_path || !kernel_path[0]) {
        if (!disk_is_open()) {
            fprintf(stderr,"[boot] no kernel path and no disk image open\n");
            return -1;
        }
        uint8_t *kbuf=NULL; size_t ksz=0; bzimage_info_t kinfo;
        if (disk_load_kernel((uint64_t)cfg->kernel_lba, &kbuf, &ksz, &kinfo) < 0) return -1;
        /* Write to tempfile */
        snprintf(kernel_tmp, sizeof(kernel_tmp), "/tmp/hdgl_kernel_XXXXXX");
        int fd = mkstemp(kernel_tmp);
        if (fd<0) { free(kbuf); perror("mkstemp kernel"); return -1; }
        /* Write ONLY the protected-mode part (after setup code) */
                if (write(fd, kbuf, ksz) < 0) { /* write full image for kexec */ }
        close(fd); free(kbuf);
        kernel_path = kernel_tmp; cleanup_kernel = 1;
        fprintf(stderr,"[boot] kernel extracted to %s\n", kernel_tmp);
    }

    if (!initrd_path || !initrd_path[0]) {
        if (disk_is_open()) {
            uint8_t *ibuf=NULL; size_t isz=0;
            if (disk_load_initrd((uint64_t)cfg->initrd_lba, &ibuf, &isz) >= 0) {
                if (write_tmpfile("hdgl_initrd", ibuf, isz, initrd_tmp, sizeof(initrd_tmp)) == 0) {
                    initrd_path = initrd_tmp; cleanup_initrd = 1;
                    fprintf(stderr,"[boot] initrd extracted to %s\n", initrd_tmp);
                }
                free(ibuf);
            }
        }
    }

    if (!kernel_path || !kernel_path[0]) {
        fprintf(stderr,"[boot] ERROR: no kernel available\n");
        return -1;
    }

    /* Detect best boot method */
    boot_method_t method = cfg->force_method;
    if (method == BOOT_METHOD_AUTO)
        method = boot_detect_method();

    fprintf(stderr,"\n[boot] ─────────────────────────────────────────────────\n");
    fprintf(stderr,"[boot] Booting Alpine on analog substrate\n");
    fprintf(stderr,"[boot] Method:  %s\n", boot_method_name(method));
    fprintf(stderr,"[boot] Kernel:  %s\n", kernel_path);
    fprintf(stderr,"[boot] Initrd:  %s\n", initrd_path ? initrd_path : "(none)");
    fprintf(stderr,"[boot] Cmdline: %s\n", cmdline);
    fprintf(stderr,"[boot] ─────────────────────────────────────────────────\n\n");

    int rc = -1;
    switch (method) {
    case BOOT_METHOD_KEXEC_SYSCALL:
        rc = boot_kexec_syscall(kernel_path, initrd_path, cmdline);
        if (rc < 0) {
            fprintf(stderr,"[boot] kexec_file_load failed, trying kexec utility\n");
            method = BOOT_METHOD_KEXEC_UTIL;
            /* fall through */
        } else break;
        /* FALLTHROUGH */
    case BOOT_METHOD_KEXEC_UTIL:
        rc = boot_kexec_util(kernel_path, initrd_path, cmdline);
        if (rc < 0 && !cfg->no_fallback) {
            fprintf(stderr,"[boot] kexec utility failed, falling back to QEMU -kernel\n");
            method = BOOT_METHOD_QEMU_KERNEL;
            /* fall through */
        } else break;
        /* FALLTHROUGH */
    case BOOT_METHOD_QEMU_KERNEL:
        rc = boot_qemu_kernel(kernel_path, initrd_path, cmdline,
                              cfg->mem_mb > 0 ? cfg->mem_mb : 512);
        break;
    default:
        fprintf(stderr,"[boot] unknown method %d\n", (int)method);
    }

    if (cleanup_kernel && kernel_tmp[0]) unlink(kernel_tmp);
    if (cleanup_initrd && initrd_tmp[0]) unlink(initrd_tmp);
    return rc;
}

/* ── Print boot status ────────────────────────────────────────────────── */
void boot_print_status(void) {
    boot_method_t m = boot_detect_method();
    int is_root = (geteuid() == 0);

    printf("\n\033[1;36m Boot Engine Status\033[0m\n");
    printf("  Root:          %s\n", is_root ? "\033[1;32mYES\033[0m" : "\033[1;31mNO\033[0m (some methods require root)");
    printf("  Best method:   \033[1;33m%s\033[0m\n", boot_method_name(m));

    const char *kexec_paths[] = {"/sbin/kexec","/usr/sbin/kexec","/usr/bin/kexec",NULL};
    const char *kexec_path = NULL;
    for (int i=0; kexec_paths[i]; i++) {
        if (access(kexec_paths[i], X_OK)==0) { kexec_path=kexec_paths[i]; break; }
    }
    printf("  kexec utility: %s\n", kexec_path ? kexec_path : "\033[1;31mnot found\033[0m");

    const char *qemu = access("/usr/bin/qemu-system-x86_64",X_OK)==0
                     ? "/usr/bin/qemu-system-x86_64" : NULL;
    printf("  QEMU:          %s\n", qemu ? qemu : "\033[1;31mnot found\033[0m");

    if (!is_root && !kexec_path) {
        printf("\n  \033[1;33mRecommendation:\033[0m Run as root for kexec, or install kexec-tools:\n");
        printf("    sudo apt-get install kexec-tools  # Debian/Ubuntu\n");
        printf("    sudo apk add kexec-tools          # Alpine\n");
    }
    printf("\n");
}
