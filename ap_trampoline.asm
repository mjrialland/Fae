; This is the AP trampoline - will be copied to 0x8000
; APs start in 16-bit real mode

bits 16
ap_start:
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7C00
    
    lgdt [cs:trampoline_gdtr - trampoline_start]
    
    mov eax, cr0
    or eax, 1
    mov cr0, eax
    
    jmp 0x08:(0x8000 + protected_mode - trampoline_start)

align 16
trampoline_gdtr:
    dw (trampoline_gdt_end - trampoline_gdt - 1)
    dd 0x8000 + (trampoline_gdt - trampoline_start)

align 16
trampoline_gdt:
    dq 0             ; Null
    dq 0x00CF9A000000FFFF  ; Code: 4GB flat, 32-bit, exec/read
    dq 0x00CF92000000FFFF  ; Data: 4GB flat, 32-bit, read/write
trampoline_gdt_end:

bits 32
protected_mode:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov ss, ax
    ; Increment cores_ready 
    lock inc byte [cores_ready]
    
    ; Read LAPIC ID (no paging enabled yet)
    mov ecx, 0x1B
    rdmsr
    and eax, 0xFFFFF000
    mov edi, [eax + 0x20]
    shr edi, 24           ; EDI now holds the APIC ID
    
    ; Enable PAE and SSE
    mov eax, cr4
    or eax, 1 << 5 | 1 << 9 | 1 << 10
    mov cr4, eax
    
    ; Load PML4
    mov eax, pml4
    mov cr3, eax
    
    ; Enable Long Mode
    mov ecx, 0xC0000080
    rdmsr
    or eax, 1 << 8
    wrmsr
    
    ; Enable Paging, set NE, set MP, clear EM
    mov eax, cr0
    and eax, 0xFFFFFFFB
    or eax, 1 << 31 | 1 << 5 | 1 << 1
    mov cr0, eax
    
    ; Load 64-bit GDT
    lgdt [gdt_desc]
    
    ; Far jump to 64-bit mode
    jmp gdt_code:ap_long_mode_start
