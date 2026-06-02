section .text
bits 64
global efi_jmp_to_kernel
efi_jmp_to_kernel:
    ; rcx = entry point
    ; rdx = magic
    ; r8  = boot_info pointer
    
    ; Disable interrupts
    cli
    
    ; Move parameters to standard 32-bit registers before leaving 64-bit mode
    mov esi, ecx  ; esi = entry point
    mov edi, edx  ; edi = magic
    mov ebx, r8d  ; ebx = boot_info pointer
    
    ; Load 32-bit compatibility GDT
    lgdt [rel gdt_desc_32]
    
    ; Far jump to 32-bit compatibility mode
    push 0x08
    lea rax, [rel .compat_32]
    push rax
    retfq

bits 32
.compat_32:
    ; Set 32-bit data segments
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    
    ; Disable paging (clear PG in CR0)
    mov eax, cr0
    and eax, 0x7FFFFFFF
    mov cr0, eax
    
    ; Disable Long Mode (clear LME in EFER MSR)
    mov ecx, 0xC0000080
    rdmsr
    and eax, ~0x00000100 ; clear LME (bit 8)
    wrmsr
    
    ; Disable PAE in CR4 (bit 5)
    mov eax, cr4
    and eax, ~0x00000020
    mov cr4, eax
    
    ; Prepare arguments for kernel entry (magic in eax, boot info in ebx)
    mov eax, edi
    ; ebx already has boot_info pointer
    
    ; Jump to the kernel's entry point
    jmp esi

section .data
align 8
gdt_desc_32:
    dw gdt_table_32_end - gdt_table_32 - 1
    dq gdt_table_32

align 8
gdt_table_32:
    dq 0 ; null descriptor
    dq 0x00cf9a000000ffff ; 32-bit code selector (0x08)
    dq 0x00cf92000000ffff ; 32-bit data selector (0x10)
gdt_table_32_end:
