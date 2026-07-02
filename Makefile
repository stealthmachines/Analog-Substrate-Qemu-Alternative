# ============================================================================
# Makefile — HDGL Runtime (Universal QEMU Alternative)
# ============================================================================
CC     = gcc
CFLAGS = -O3 -Wall -Wextra -Wno-unused-parameter -Wno-unused-result \
         -Wno-multichar -Wno-overflow -Wno-sequence-point \
         -Wno-misleading-indentation -Wno-unused-variable -Wno-empty-body -Wno-unused-function \
         -march=native -ffast-math -I.
LDFLAGS = -lpthread -lm

TARGET = hdgl_run
SRCS   = hdgl_run.c phi_substrate.c hdgl_shell.c \
         hdgl_disk.c hdgl_boot.c hdgl_term.c \
         hdgl_lattice.c hdgl_universe.c
OBJS   = $(SRCS:.c=.o)

.PHONY: all clean install test benchmark rootfs

all: $(TARGET)
	@echo ""
	@echo "  hdgl_run built — universal QEMU alternative"
	@echo "  $(shell ls -lh $(TARGET) | awk '{print $$5}') binary"
	@echo ""
	@echo "  Quick start:"
	@echo "    ./$(TARGET) --image bin/hdgl_router64.img"
	@echo "    ./$(TARGET) --benchmark"
	@echo "    ./$(TARGET) --image bin/hdgl_router64.img --boot"
	@echo ""
	@echo "  Fully functional Alpine:"
	@echo "    bash build_alpine_rootfs.sh"
	@echo "    ./$(TARGET) --image bin/hdgl_router64.img \\"
	@echo "                --rootfs bin/hdgl_alpine_rootfs.img --boot"
	@echo ""

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

# Dependency tracking
hdgl_run.o:      hdgl_run.c phi_substrate.h hdgl_shell.h hdgl_disk.h \
                 hdgl_boot.h hdgl_term.h hdgl_lattice.h hdgl_universe.h
phi_substrate.o: phi_substrate.c phi_substrate.h
hdgl_shell.o:    hdgl_shell.c hdgl_shell.h phi_substrate.h hdgl_boot.h \
                 hdgl_term.h hdgl_lattice.h
hdgl_disk.o:     hdgl_disk.c hdgl_disk.h
hdgl_boot.o:     hdgl_boot.c hdgl_boot.h phi_substrate.h hdgl_disk.h
hdgl_term.o:     hdgl_term.c hdgl_term.h
hdgl_lattice.o:  hdgl_lattice.c hdgl_lattice.h
hdgl_universe.o: hdgl_universe.c hdgl_universe.h hdgl_disk.h hdgl_lattice.h

benchmark: $(TARGET)
	./$(TARGET) --benchmark

rootfs:
	bash build_alpine_rootfs.sh

test: $(TARGET)
	@echo "=== Suite detection test ==="
	@mkdir -p /tmp/hdgl_test_slots /run/lattice
	timeout 4 ./$(TARGET) --substrate-only --genome DEADBEEF \
	    --settle 80 --slots /tmp/hdgl_test_slots --interval 100000 2>&1 || true
	@echo "Slots: $$(ls /tmp/hdgl_test_slots/ 2>/dev/null | wc -l)/4096"
	@echo "D1 = $$(cat /tmp/hdgl_test_slots/1 2>/dev/null || echo ?)"
	@echo "D32= $$(cat /tmp/hdgl_test_slots/32 2>/dev/null || echo ?)"
	@cat /run/lattice/state 2>/dev/null | grep -E "TYPE|CONSENSUS|KURA" || true

clean:
	rm -f $(OBJS) $(TARGET)

install: $(TARGET)
	install -m 755 $(TARGET) /usr/local/bin/
