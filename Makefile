# --- Makefile GregOS ---

CC  = gcc
CXX = g++
AS  = nasm
LD  = ld

CFLAGS   = -m32 -ffreestanding -O2 -Wall -Wextra -Iinclude
CXXFLAGS = -m32 -ffreestanding -O2 -Wall -Wextra -Iinclude \
           -fno-exceptions -fno-rtti -fno-threadsafe-statics \
           -std=c++11

LDFLAGS  = -m elf_i386 -T arch/i386/linker.ld

# keyboard.c is legacy (replaced by kernel/PS2Keyboard.cpp) and excluded
ALL_C       = $(wildcard kernel/*.c drivers/*.c)
C_SOURCES   = $(filter-out drivers/keyboard.c, $(ALL_C))
CPP_SOURCES = $(wildcard kernel/*.cpp kernel/Greg/*.cpp kernel/GUI/*.cpp drivers/*.cpp)

HEADERS = $(wildcard include/*.h include/*.hpp include/Greg/*.h include/Greg/*.hpp include/Kernel/*.h include/Kernel/*.hpp include/GUI/*.hpp include/Compositor/*.hpp)

OBJ = $(C_SOURCES:.c=.o) $(CPP_SOURCES:.cpp=.o) \
      arch/i386/boot.o arch/i386/isr.o arch/i386/switch_task.o

DISK_IMG = disk.img

# ── External ELF programs ───────────────────────────────────────────────
BLACKJACK_SRC = programs/blackjack/main.c
BLACKJACK_ELF = programs/blackjack/blackjack.elf
BLACKJACK_HDR = include/blackjack_elf_data.h

# Isolated Ring-3 userland demo — flat binary loaded at VA 0x40000000
USER_SRC = programs/userhello/user.asm
USER_BIN = programs/userhello/user.bin
USER_HDR = include/user_bin_data.h

# Isolated Ring-3 userland demo — a real C program compiled to a static ELF
USERAPP_SRC = programs/userapp/main.c
USERAPP_ELF = programs/userapp/userapp.elf
USERAPP_HDR = include/userapp_elf_data.h

all: $(BLACKJACK_HDR) $(USER_HDR) $(USERAPP_HDR) myos.iso

$(BLACKJACK_ELF): $(BLACKJACK_SRC)
	$(CC) -m32 -ffreestanding -nostdlib -fPIE -pie -e _start \
	      -Wl,--build-id=none -o $@ $<

$(BLACKJACK_HDR): $(BLACKJACK_ELF) tools/make_elf_header.py
	python3 tools/make_elf_header.py $< $@

$(USER_BIN): $(USER_SRC)
	$(AS) -f bin $< -o $@

$(USER_HDR): $(USER_BIN) tools/make_elf_header.py
	python3 tools/make_elf_header.py $< $@

# Static ET_EXEC at 0x40000000, one contiguous LOAD segment (-N), no libc.
$(USERAPP_ELF): $(USERAPP_SRC)
	$(CC) -m32 -ffreestanding -nostdlib -fno-pic -fno-pie -no-pie -O2 \
	      -Wl,-N -Wl,-Ttext=0x40000000 -Wl,-e_start -Wl,--build-id=none \
	      -o $@ $<

$(USERAPP_HDR): $(USERAPP_ELF) tools/make_elf_header.py
	python3 tools/make_elf_header.py $< $@

# elf.cpp includes blackjack_elf_data.h → rebuild when ELF changes
kernel/elf.o: $(BLACKJACK_HDR)

# kernel.c includes user_bin_data.h + userapp_elf_data.h → rebuild on change
kernel/kernel.o: $(USER_HDR) $(USERAPP_HDR)

myos.iso: kernel.bin
	mkdir -p iso/boot/grub
	cp kernel.bin iso/boot/myos.bin
	printf 'set timeout=0\nset default=0\nset gfxpayload=keep\nmenuentry "GregOS" { multiboot2 /boot/myos.bin }\n' > iso/boot/grub/grub.cfg
	grub-mkrescue -o myos.iso iso

kernel.bin: $(OBJ)
	$(LD) $(LDFLAGS) -o $@ $^

%.o: %.c $(HEADERS)
	$(CC) $(CFLAGS) -c $< -o $@

%.o: %.cpp $(HEADERS)
	$(CXX) $(CXXFLAGS) -c $< -o $@

%.o: %.asm
	$(AS) -felf32 $< -o $@

$(DISK_IMG):
	dd if=/dev/zero of=$@ bs=512 count=2048 2>/dev/null

# ── Networking (QEMU user/slirp + RTL8139) ─────────────────────────────
# Guest IP 10.0.2.15, gateway 10.0.2.2, DNS 10.0.2.3 — matches kernel/net.c.
NETDEV = -netdev user,id=n0 -device rtl8139,netdev=n0
# Uncomment to capture all traffic to /tmp/gregos-net.pcap (wireshark-readable):
# NETDEV += -object filter-dump,id=f0,netdev=n0,file=/tmp/gregos-net.pcap

# -cpu max exposes RDRAND, which the TLS CSPRNG uses for real entropy.
# QEMU's default i386 model (qemu32) has no RDRAND and the RNG would fall back
# to TSC/jiffies only — non-repeating but not cryptographically strong.
run: myos.iso $(DISK_IMG)
	qemu-system-i386 -cdrom myos.iso -vga std -m 256M -cpu max \
	  -audiodev pa,id=snd0 -machine pc,pcspk-audiodev=snd0 \
	  $(NETDEV) \
	  -drive file=$(DISK_IMG),format=raw,if=ide,index=0

run-kvm: myos.iso $(DISK_IMG)
	qemu-system-i386 -cdrom myos.iso -vga std -m 256M \
	  -enable-kvm -cpu host \
	  -audiodev pa,id=snd0 -machine pc,pcspk-audiodev=snd0 \
	  $(NETDEV) \
	  -drive file=$(DISK_IMG),format=raw,if=ide,index=0

run-debug: myos.iso $(DISK_IMG)
	qemu-system-i386 -cdrom myos.iso -vga std -m 256M -cpu max \
	  -audiodev pa,id=snd0 -machine pc,pcspk-audiodev=snd0 \
	  $(NETDEV) \
	  -no-reboot -no-shutdown \
	  -d cpu_reset -D /tmp/qemu-gregos.log \
	  -debugcon file:/tmp/gregos-debug.log \
	  -drive file=$(DISK_IMG),format=raw,if=ide,index=0

clean:
	rm -rf $(OBJ) kernel.bin myos.iso iso/ $(DISK_IMG) \
	       $(BLACKJACK_ELF) $(BLACKJACK_HDR) $(USER_BIN) $(USER_HDR) \
	       $(USERAPP_ELF) $(USERAPP_HDR)
