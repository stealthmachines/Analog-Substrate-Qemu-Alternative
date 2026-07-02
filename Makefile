# ============================================================================
# Makefile — HDGL Runtime
# ============================================================================

CC      = gcc
CFLAGS  = -O3 -Wall -Wextra -Wno-unused-parameter \
           -Wno-unused-result -Wno-multichar -Wno-overflow \
           -Wno-sequence-point -Wno-misleading-indentation -Wno-unused-variable -march=native -ffast-math -I.
LDFLAGS = -lpthread -lm

TARGET = hdgl_run
SRCS   = hdgl_run.c phi_substrate.c hdgl_shell.c \
         hdgl_disk.c hdgl_boot.c hdgl_term.c hdgl_lattice.c
OBJS   = $(SRCS:.c=.o)

.PHONY: all clean install test benchmark

all: $(TARGET)
	@printf "\n  %-20s %s\n" "Binary:" "./$(TARGET)"
	@printf "  %-20s %s\n" "Quick test:" "./$(TARGET) --benchmark"
	@printf "  %-20s %s\n" "Interactive:" "./$(TARGET)"
	@printf "  %-20s %s\n" "From image:" "./$(TARGET) --image bin/hdgl_router64.img\n"

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

# Dependencies
hdgl_run.o:      hdgl_run.c phi_substrate.h hdgl_shell.h hdgl_disk.h hdgl_boot.h hdgl_term.h
phi_substrate.o: phi_substrate.c phi_substrate.h
hdgl_shell.o:    hdgl_shell.c hdgl_shell.h phi_substrate.h hdgl_boot.h hdgl_term.h
hdgl_disk.o:     hdgl_disk.c hdgl_disk.h
hdgl_boot.o:     hdgl_boot.c hdgl_boot.h phi_substrate.h hdgl_disk.h
hdgl_term.o:     hdgl_term.c hdgl_term.h
hdgl_lattice.o:  hdgl_lattice.c hdgl_lattice.h

benchmark: $(TARGET)
	./$(TARGET) --benchmark

test: $(TARGET)
	@echo "=== substrate 5s ===" && mkdir -p /tmp/hr_slots /run/lattice
	timeout 5 ./$(TARGET) --substrate-only --genome DEADBEEF \
	    --settle 100 --slots /tmp/hr_slots --interval 100000 2>&1 || true
	@echo "slots: $$(ls /tmp/hr_slots/ | wc -l)"
	@cat /tmp/hr_slots/1 2>/dev/null | xargs printf "D1  = %s\n"
	@cat /tmp/hr_slots/32 2>/dev/null | xargs printf "D32 = %s\n"
	@grep -E "D_BITS|GLYPH|KURA" /run/lattice/state 2>/dev/null || true

install: $(TARGET)
	install -m 755 $(TARGET) /usr/local/bin/

clean:
	rm -f $(OBJS) $(TARGET)
