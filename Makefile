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
         -mcmodel=large \
         -ffreestanding -fno-stack-protector -fno-stack-check -fno-lto -fno-PIE \
         -fno-pic -I. -Inet/include

# Linker flags
# We use a custom linker script or flags to set the base address.
# Limine's protocol usually handles relocations, but -z max-page-size=0x1000 is good for standard paging.
LDFLAGS = -m elf_x86_64 -nostdlib -static -z max-page-size=0x1000 -T linker.ld

# Source files
SRCS = kernel.c gdt.c idt.c pic.c keyboard.c pmm.c vmm.c heap.c serial.c \
       console.c vfs.c initrd.c syscall.c user.c shell.c timer.c sched.c \
       mouse.c desktop.c speaker.c compositor.c elf.c ipc.c security.c \
       spinlock.c cpu.c lapic.c mutex.c semaphore.c fd.c pipe.c signal.c \
       futex.c vm_area.c acpi.c ioapic.c rtc.c driver.c pci.c dma.c \
       devfs.c ahci.c bga.c block.c partition.c bcache.c ext2.c klog.c ksyms.c \
       aio.c drivers/e1000.c drivers/hda.c drivers/usb/xhci.c drivers/nvme.c net/sys_arch.c net/core/pbuf.c \
       net/core/netif.c net/core/ip.c net/core/tcp.c net/core/arp.c \
       compat/linux/linux_syscall.c

OBJS = $(SRCS:.c=.o) interrupts.o

.PHONY: all clean run iso

all: $(ISO_IMAGE)

clean:
	rm -f $(OBJS) $(KERNEL_BIN) $(ISO_IMAGE) iso_root/kernel.elf
	rm -rf initrd iso_root

iso: $(ISO_IMAGE)

$(KERNEL_BIN): $(OBJS)
	$(LD) $(LDFLAGS) $(OBJS) -o $@

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

%.o: %.S
	nasm -felf64 $< -o $@

initrd/init.elf initrd/compositor.elf initrd/service_manager.elf initrd/keyboard_driver.elf initrd/mouse_driver.elf initrd/terminal.elf initrd/panel.elf initrd/audio_server.elf:
	$(MAKE) -f userspace/Makefile.musl $@

initrd/gemini.elf: userspace/gemini.c userspace/lib/tls.c userspace/lib/tlse/tlse.c userspace/linker.ld
	mkdir -p initrd
	$(CC) -O2 -g -Wall -Wextra -m64 -march=x86-64 -ffreestanding -fno-stack-protector -fno-PIE -no-pie -fno-pic -nostdlib \
		-Iuserspace/include -Iuserspace -DTLS_AMALGAMATION -DSSL_COMPATIBLE_INTERFACE -DLTC_NO_FILE \
		-T userspace/linker.ld userspace/gemini.c userspace/lib/tls.c userspace/lib/tlse/tlse.c -o initrd/gemini.elf

initrd/test_driver.o: drivers/test_driver.c
	mkdir -p initrd
	$(CC) $(CFLAGS) -mcmodel=large -r -c drivers/test_driver.c -o initrd/test_driver.o


userspace/libc/syscalls.o: userspace/libc/syscalls.c
	$(CC) -O2 -g -Wall -Wextra -m64 -march=x86-64 -ffreestanding -fno-stack-protector -fno-PIE -no-pie -fno-pic -nostdlib -Iuserspace -Iuserspace/libc -c userspace/libc/syscalls.c -o userspace/libc/syscalls.o



initrd/doom.elf:
	$(MAKE) -C ports/doomgeneric/doomgeneric -f Makefile.raeenos

initrd/FreeMono.ttf: assets/FreeMono.ttf
	mkdir -p initrd
	cp assets/FreeMono.ttf initrd/FreeMono.ttf

initrd/doom1.wad: assets/doom1.wad
	mkdir -p initrd
	cp assets/doom1.wad initrd/doom1.wad

initrd.tar: initrd/init.elf initrd/compositor.elf initrd/service_manager.elf initrd/keyboard_driver.elf initrd/mouse_driver.elf initrd/terminal.elf initrd/panel.elf initrd/audio_server.elf initrd/test_driver.o initrd/doom.elf initrd/FreeMono.ttf initrd/doom1.wad
	tar -cvf initrd.tar -C initrd .

$(ISO_IMAGE): $(KERNEL_BIN) limine initrd.tar
	# Create a directory for the ISO content
	mkdir -p iso_root
	
	# Copy the kernel, config, and initrd
	cp -f $(KERNEL_BIN) limine.cfg initrd.tar limine/limine-bios.sys limine/limine-bios-cd.bin limine/limine-uefi-cd.bin iso_root/

	# Create directory for EFI boot
	mkdir -p iso_root/EFI/BOOT
	cp -f limine/BOOTX64.EFI iso_root/EFI/BOOT/
	cp -f limine/BOOTIA32.EFI iso_root/EFI/BOOT/

	# Create the ISO using xorriso
	xorriso -as mkisofs -b limine-bios-cd.bin \
		-no-emul-boot -boot-load-size 4 -boot-info-table \
		--efi-boot limine-uefi-cd.bin \
		-efi-boot-part --efi-boot-image --protective-msdos-label \
		iso_root -o $(ISO_IMAGE)
	
	# Install Limine to the ISO (BIOS boot support)
	./limine/limine bios-install $(ISO_IMAGE)

run: $(ISO_IMAGE) disk.img
	qemu-system-x86_64 -cdrom $(ISO_IMAGE) -M q35 -serial file:serial.log \
	-drive id=disk,file=disk.img,if=none,format=raw \
	-device ahci,id=ahci \
	-device ide-hd,drive=disk,bus=ahci.0

disk.img:
	dd if=/dev/zero of=disk.img bs=1M count=64


