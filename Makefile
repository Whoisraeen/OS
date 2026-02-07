# Naming the output
OS_NAME = myos
KERNEL_BIN = kernel.elf
ISO_IMAGE = $(OS_NAME).iso

# Compiler and Linker
CC = gcc
LD = ld

# Flags
# -ffreestanding: Asserts that compilation targets a freestanding environment (no stdlibs).
# -mno-red-zone: Disables the "red zone" optimization (crucial for kernel interrupts).
# -m64: Targets x86_64.
CFLAGS = -O2 -g -Wall -Wextra -Wpedantic \
         -m64 -march=x86-64 -mno-80387 -mno-mmx -mno-sse -mno-sse2 -mno-red-zone \
         -mcmodel=kernel \
         -ffreestanding -fno-stack-protector -fno-stack-check -fno-lto -fno-PIE \
         -fno-pic -I.

# Linker flags
# We use a custom linker script or flags to set the base address.
# Limine's protocol usually handles relocations, but -z max-page-size=0x1000 is good for standard paging.
LDFLAGS = -m elf_x86_64 -nostdlib -static -z max-page-size=0x1000 -T linker.ld

# Source files
SRCS = kernel.c gdt.c idt.c pic.c keyboard.c pmm.c vmm.c heap.c serial.c console.c vfs.c initrd.c syscall.c user.c shell.c timer.c sched.c mouse.c desktop.c speaker.c compositor.c elf.c ipc.c security.c spinlock.c
OBJS = $(SRCS:.c=.o) interrupts.o

.PHONY: all clean run iso

all: $(ISO_IMAGE)

iso: $(ISO_IMAGE)

$(KERNEL_BIN): $(OBJS)
	$(LD) $(LDFLAGS) $(OBJS) -o $@

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

%.o: %.S
	nasm -felf64 $< -o $@

initrd.tar:
	tar -cvf initrd.tar -C initrd .

$(ISO_IMAGE): $(KERNEL_BIN) limine initrd.tar
	# Create a directory for the ISO content
	mkdir -p iso_root
	
	# Copy the kernel, config, and initrd
	cp $(KERNEL_BIN) limine.cfg initrd.tar limine/limine-bios.sys limine/limine-bios-cd.bin limine/limine-uefi-cd.bin iso_root/
	
	# Create directory for EFI boot
	mkdir -p iso_root/EFI/BOOT
	cp limine/BOOTX64.EFI iso_root/EFI/BOOT/
	cp limine/BOOTIA32.EFI iso_root/EFI/BOOT/

	# Create the ISO using xorriso
	xorriso -as mkisofs -b limine-bios-cd.bin \
		-no-emul-boot -boot-load-size 4 -boot-info-table \
		--efi-boot limine-uefi-cd.bin \
		-efi-boot-part --efi-boot-image --protective-msdos-label \
		iso_root -o $(ISO_IMAGE)
	
	# Install Limine to the ISO (BIOS boot support)
	./limine/limine bios-install $(ISO_IMAGE)

run: $(ISO_IMAGE)
	qemu-system-x86_64 -cdrom $(ISO_IMAGE) -M q35 -serial stdio

clean:
	rm -rf $(OBJS) $(KERNEL_BIN) $(ISO_IMAGE) iso_root
