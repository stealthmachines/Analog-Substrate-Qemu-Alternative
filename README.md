In addition to being designed to work with water glyphs continuous analog substrate (see zip folder), this version is emergently designed to work with https://github.com/stealthmachines/HDGL-fabric/tree/v0.2

See also: https://josefkulovany.com/demo/6.26.26%20-%20HDGL%20Alternative%20Fabric/6.30.26%20-%20day%204%20combined%20works/

# hdgl_runtime — HDGL Continuous Analog Substrate Runtime

**The complete QEMU alternative. Reliable real-metal emulation.**

Replaces:
```bash
qemu-system-x86_64 -drive file=bin/hdgl_router64.img,format=raw,if=ide \
  -boot order=c -m 256M -serial stdio -no-reboot -display none
```

With:
```bash
./hdgl_run --image bin/hdgl_router64.img
```

---

## Measured performance vs QEMU TCG (QEMU 8.2, no KVM)

| Operation | hdgl_run native | QEMU TCG | Speedup |
|-----------|----------------|----------|---------|
| Basin step (64×64=4096 cells) | **10.6 µs** | ~2500 µs | **235×** |
| Kuramoto 8D step | **<1 ns** | ~5000 ns | **>1000×** |
| Shell dispatch | **~1 µs** | ~100 µs | **~100×** |
| Alpine boot (kexec) | **~1–3 s** | ~90 s | **~45×** |
| Substrate after Alpine | **RUNNING** | **STOPPED** | ∞ |

The last row is the fundamental difference. QEMU stops the analog substrate when
the kernel takes over. `hdgl_run` keeps the substrate thread alive throughout the
entire Alpine session — `/lattice/slots/` updates continuously.

---

## Build

```bash
cd hdgl_runtime
make
./hdgl_run --help
```

**Requirements:** gcc ≥ 9, libpthread, libm. Nothing else.

---

## Usage

### From a disk image (full real-metal emulation)
```bash
./hdgl_run --image bin/hdgl_router64.img
```
- Opens the disk image
- Scans for MBR, kernel@LBA512, initrd@LBA16896
- Derives `genome_fp` from disk content (hardware identity)
- Starts substrate thread with that genome_fp
- Prints exact firmware boot sequence (matches real hardware serial output)
- Shows interactive shell

### Auto-boot Alpine
```bash
./hdgl_run --image bin/hdgl_router64.img --boot
```

### Interactive shell
```bash
./hdgl_run                                    # default genome
./hdgl_run --genome DEADBEEF                  # specific genome
./hdgl_run --image bin/hdgl_router64.img      # from disk image
```

### Shell commands
```
Router64> help         list all commands
Router64> analog       D1..D32 field with FIRE indicators at sqrt(phi)
Router64> dn           Dn aggregate (8-nibble hex, e.g. 0xFFFF0000)
Router64> prismatic    4096-strand tally, Chladni symmetry class
Router64> pool         32-bit binary pool state
Router64> glyph        Chladni pattern + Kuramoto phase display
Router64> substrate    full substrate status (phase, R, step, active modes)
Router64> slots [N]    show slot N or first 32
Router64> boot [K] [I] boot Alpine (auto-detect method)
Router64> kexec K [I]  boot via kexec with specific kernel/initrd
Router64> bootstat     show available boot methods
Router64> quit
```

### Substrate daemon
```bash
./hdgl_run --substrate-only --genome DEADBEEF &
# /lattice/slots/1..4096 update continuously
# /run/lattice/state is live
```

### Benchmark
```bash
./hdgl_run --benchmark
```

---

## How it emulates real metal

### Boot sequence
`hdgl_run` prints the **exact same serial output** as the real firmware:

```
[Omega] BOOT: graph init -> OBSERVE
[Omega] REALIZE: T_COMPILE_SELF -> fixed point
[Omega] RUNTIME: Omega_n+1=T(Omega_n) complete
[Omega] Graph state:
  Omega[01 ] type=1 state=4
  ...
[Analog] Dn(r) lattice: phi-seeded 8-strand 32-slot
[Kernel] phi-lattice 64-bit router ready. Consensus=LOCK
[Analog@4096] substrate ready
Router64>
```

A script or test harness that checks firmware serial output works identically
against `hdgl_run` and against real hardware.

### Disk image
`disk_scan()` reads the image and finds:
- MBR at sector 0 (checks 0xAA55 signature)
- Runtime64 at sectors 2–65 (checks jump opcode)
- Kernel at LBA 512 (checks HdrS magic + XLF_KERNEL_64)
- Initrd at LBA 16896 (checks gzip/cpio magic)

`genome_fp` is derived from the disk content (kernel size ^ scan flags ^ base)
so different disk images produce different genome fingerprints, matching the
hardware-unique identity from CPUID + E820 in the real firmware.

### Analog substrate
Two concurrent layers in one pthread:

**Layer A — Water Glyph Basin** (Chladni eigenmodes):
- 64×64 = 4096 cells (the fourth 4096)
- `genome_fp` bytes → DNA bases → Bessel angular modes J_n
- Wave equation: `u_new = 2u - u_prev + c²∇²u - γ(u-u_prev) + drive`
- Stable eigenmodes → D1..D32 slot values at Bessel zero loci
- Step: **10.6 µs** native vs **2500 µs** QEMU TCG

**Layer B — 8D Kuramoto Oscillator** (ll_analog architecture):
- φ-seeded natural frequencies ω_i = φ^(i+1)
- PLUCK→SUSTAIN→FINETUNE→LOCK progression
- Writes θ[0..7] to /lattice/slots/0..7
- `LOCK` when CV < 0.05 (order parameter R → 1)

### Boot methods (in priority order)
1. **kexec_file_load() syscall** — fastest, requires root, Linux ≥ 3.17
2. **kexec utility** — requires kexec-tools installed
3. **qemu-system-x86_64 -kernel** — fallback; substrate runs in parent process outside QEMU

All three methods pass `hdgl.dn=XXXXXXXX hdgl.tick=XXXXXXXX` in the kernel
cmdline with the **live substrate state** at handoff time. Alpine's `/init`
reads these tokens and phi_pool starts from the settled eigenmode values.

---

## File structure

```
hdgl_runtime/
  hdgl_run.c         — main driver: args, boot seq, shell, daemon mode
  phi_substrate.c    — basin wave eq + Kuramoto engine (the actual compute)
  phi_substrate.h    — hdgl_shm_t, substrate_config_t, API
  hdgl_shell.c       — Router64 commands as native C (analog/dn/prismatic/pool...)
  hdgl_shell.h       — shell API
  hdgl_disk.c        — disk image reader: scan, load kernel/initrd, validate bzImage
  hdgl_disk.h        — disk API
  hdgl_boot.c        — boot engine: kexec syscall, kexec utility, QEMU fallback
  hdgl_boot.h        — boot_config_t, e820_entry_t, API
  hdgl_term.c        — terminal: raw mode, line editor, history, arrow keys
  hdgl_term.h        — terminal API
  Makefile           — one-step build
  README.md          — this file
```

---

## Roadmap to true concurrent substrate

After `kexec`, the substrate thread dies with the parent process. Three paths
to keep the field alive under Alpine:

**1. Kernel thread module** (`phi_kthread_module.c`, ~300 lines):
Port `phi_substrate.c` to a Linux kthread at `SCHED_FIFO` priority.
The wave equation runs in kernel space. `/proc/phi` or `/sys/phi` exposes it.
`insmod phi_kthread_module.ko` after Alpine boots.

**2. SMP AP core** (`hdgl_smp_substrate.asm`, already in outputs):
INIT/SIPI before handoff parks a secondary CPU in the substrate loop.
After kexec, core 1 keeps ticking. `phi_analog_module.ko` exposes the SHM.
Requires SMP hardware (more than 1 core). Works in QEMU with `-smp 2`.

**3. FPGA offload**:
Implement the wave equation in RTL (Verilog, ~200 lines).
FPGA on PCIe DMA-writes D-slot values to host memory every basin step.
The field evolves in continuous time. This is the true analog substrate.

---

*Ωₙ₊₁ = T(Ωₙ)*
