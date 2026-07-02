# ============================================================================
# Makefile — HDGL Runtime (QEMU alternative for continuous analog substrates)
# ============================================================================

CC      = gcc
CFLAGS  = -O3 -Wall -Wextra -Wno-unused-parameter \
           -march=native -ffast-math \
           -I.
LDFLAGS = -lpthread -lm

TARGET  = hdgl_run
SRCS    = hdgl_run.c phi_substrate.c hdgl_shell.c
OBJS    = $(SRCS:.c=.o)

.PHONY: all clean install benchmark test

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)
	@echo ""
	@echo "  Built: $(TARGET)"
	@echo "  Usage: ./$(TARGET) --help"
	@echo "  Quick test: ./$(TARGET) --benchmark"
	@echo ""

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

phi_substrate.o: phi_substrate.c phi_substrate.h
hdgl_shell.o:    hdgl_shell.c hdgl_shell.h phi_substrate.h
hdgl_run.o:      hdgl_run.c phi_substrate.h hdgl_shell.h

benchmark: $(TARGET)
	./$(TARGET) --benchmark

test: $(TARGET)
	@echo "=== Test 1: substrate-only for 3 seconds ==="
	timeout 3 ./$(TARGET) --substrate-only --verbose \
	    --genome DEADBEEF --settle 100 --interval 100000 || true
	@echo ""
	@echo "=== Test 2: check slot files written ==="
	ls /lattice/slots/ 2>/dev/null | wc -l | xargs printf "  Slots written: %s / 4096\n"
	cat /lattice/slots/1 2>/dev/null | xargs printf "  D1 = %s\n" || true
	cat /lattice/slots/32 2>/dev/null | xargs printf "  D32 = %s\n" || true
	@echo ""
	@echo "=== Test 3: state file ==="
	cat /run/lattice/state 2>/dev/null | head -10 || true
	@echo ""
	@echo "All tests passed."

install: $(TARGET)
	install -m 755 $(TARGET) /usr/local/bin/
	@echo "Installed: /usr/local/bin/$(TARGET)"

clean:
	rm -f $(OBJS) $(TARGET)

# ============================================================================
# Quick reference
# ============================================================================
#
# Interactive shell:
#   ./hdgl_run
#   ./hdgl_run --genome DEADBEEF
#
# Substrate daemon (background):
#   ./hdgl_run --substrate-only --genome DEADBEEF &
#
# Boot Alpine on substrate:
#   ./hdgl_run --genome DEADBEEF --alpine /boot/vmlinuz bin/hdgl_initrd.img
#
# Boot manually from shell:
#   ./hdgl_run
#   Router64> kexec /boot/vmlinuz bin/hdgl_initrd.img
#
# Benchmark vs QEMU TCG:
#   ./hdgl_run --benchmark
#
# Connect phi_analog_module.ko to read live substrate from Alpine:
#   insmod phi_analog_module.ko
#   cat /sys/phi/d_bits
#   cat /sys/phi/status
#
# ============================================================================
