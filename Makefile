CC = gcc
AS = nasm
LD = ld

CFLAGS = -ffreestanding -O3 -Wall -Wextra -mno-red-zone -fno-pic -fno-pie -nostdlib -m64 -fno-stack-protector -mavx2 -mfma
ASFLAGS = -f elf64
LDFLAGS = -n -T linker.ld

OBJS = boot.o kernel.o serial.o idt.o keyboard.o malloc.o tar.o sysinfo.o gguf.o shell.o igpu.o

all: kernel.bin initrd.tar bootloader.efi

boot.o: boot.asm ap_trampoline.asm
	$(AS) $(ASFLAGS) -o $@ boot.asm

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

kernel.bin: $(OBJS) linker.ld
	$(LD) $(LDFLAGS) -o $@ $(OBJS)

initrd.tar: tiny-lm-8M.Q8_0.gguf tiny-lm-8M.Q8_0.instruct
	tar -cf initrd.tar --format=ustar tiny-lm-8M.Q8_0.gguf tiny-lm-8M.Q8_0.instruct

efi_stub.o: efi_stub.asm
	$(AS) -f win64 -o $@ efi_stub.asm

bootloader.o: bootloader.c efi.h
	$(CC) -ffreestanding -mabi=ms -fshort-wchar -mno-red-zone -fno-stack-protector -fPIE -nostdlib -O2 -c -o $@ bootloader.c

bootloader.efi: bootloader.o efi_stub.o
	$(LD) -m i386pep --subsystem 10 -shared -Bsymbolic -e efi_main -o $@ bootloader.o efi_stub.o

clean:
	rm -f *.o kernel.bin initrd.tar bootloader.efi

run: kernel.bin initrd.tar
	qemu-system-x86_64 -kernel kernel.bin -initrd initrd.tar -smp 4 -serial stdio -display none

run-gui: kernel.bin initrd.tar
	qemu-system-x86_64 -kernel kernel.bin -initrd initrd.tar -smp 4 -serial stdio
