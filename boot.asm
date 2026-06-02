section .multiboot
align 8
multiboot1_header:
    dd 0x1BADB002       ; Magic (MB1)
    dd 0x00000007       ; Flags: ALIGN + MEMINFO + VIDEO
    dd -(0x1BADB002 + 0x00000007) ; Checksum
    
    ; Address fields (only if flags[16] is set, but we must pad to offset 32)
    dd 0
    dd 0
    dd 0
    dd 0
    dd 0
    
    ; Graphics fields (present since flags[2] is set)
    dd 0                ; mode_type (0 = linear graphics)
    dd 1024             ; width
    dd 768              ; height
    dd 32               ; depth

align 8
multiboot2_header:
    dd 0xE85250D6       ; Magic (MB2)
    dd 0                ; Architecture (0 = protected mode i386)
    dd (multiboot2_header_end - multiboot2_header) ; Header length
    dd -(0xE85250D6 + 0 + (multiboot2_header_end - multiboot2_header)) ; Checksum

    ; Tags
    ; Framebuffer tag (requesting linear graphics 1024x768x32)
    align 8
    dw 5                ; Type: Framebuffer tag
    dw 0                ; Flags: 0
    dd 20               ; Size: 20
    dd 1024             ; Width
    dd 768              ; Height
    dd 32               ; Depth
    
    ; End tag
    align 8
    dw 0                ; Type: 0
    dw 0                ; Flags: 0
    dd 8                ; Size: 8
multiboot2_header_end:

section .data
align 4096

; Page tables - identity map first 64GB using 2MB Huge Pages
pml4:
    dq pdpt + 3
    times 511 dq 0

pdpt:
%assign i 0
%rep 64
    dq pdts + (i * 4096) + 3
%assign i i+1
%endrep
times 448 dq 0

pdts:
%assign i 0
%rep 32768    ; 64 PDTs * 512 entries = 32768
    dq (i << 21) + 0x83
%assign i i+1
%endrep

; GDT
gdt:
    dq 0
gdt_code: equ $ - gdt
    dq (1<<43)|(1<<44)|(1<<47)|(1<<53)|(1<<41)
gdt_data: equ $ - gdt
    dq (1<<44)|(1<<47)|(1<<41)
gdt_end:
gdt_desc:
    dw gdt_end - gdt - 1
    dd gdt

; APIC base address (read from MSR)
apic_base: dd 0

; Synchronization
cores_ready: db 0
align 4
ap_stack_index: dd 1

section .bss
align 16
bsp_stack: resb 16384
stack_top: resq 1
multiboot_info_ptr: resd 1
multiboot_magic: resd 1

; Stacks for up to 32 APs (4KB each)
alignb 4096
ap_stacks: resb 131072

; Space for AP code copied at runtime
trampoline_buf: resb 4096

section .text
bits 32

global _start
global cores_ready
global pml4
global trampoline_start
global trampoline_end
global apic_base
global gdt_desc
_start:
    cli
    mov esp, bsp_stack + 16384
    mov [multiboot_info_ptr], ebx
    mov [multiboot_magic], eax
    
    ; Print checkpoint 0: Entered 32-bit mode
    mov edi, 0xB8000
    mov esi, boot_msg0
    call print_32

    ; Get LAPIC base from MSR
    mov ecx, 0x1B
    rdmsr
    and eax, 0xFFFFF000
    mov [apic_base], eax

    ; Print checkpoint 1: APIC base read successfully
    mov edi, 0xB8000 + 160
    mov esi, boot_msg1
    call print_32

    ; BSP is ready, set cores_ready to 1
    mov byte [cores_ready], 1
    
.done:
    ; Load our GDT (to override Multiboot's 32-bit GDT)
    lgdt [gdt_desc]

    ; Print checkpoint 2: GDT loaded
    mov edi, 0xB8000 + 160 * 2
    mov esi, boot_msg2
    call print_32

    ; Enable PAE (Physical Address Extension) and SSE in CR4
    mov eax, cr4
    or eax, 1 << 5 | 1 << 9 | 1 << 10
    mov cr4, eax

    ; Print checkpoint 3: CR4 set
    mov edi, 0xB8000 + 160 * 3
    mov esi, boot_msg3
    call print_32
    
    ; Load PML4 address into CR3
    mov eax, pml4
    mov cr3, eax

    ; Print checkpoint 4: CR3 set
    mov edi, 0xB8000 + 160 * 4
    mov esi, boot_msg4
    call print_32
    
    ; Enable Long Mode (LME) in EFER MSR
    mov ecx, 0xC0000080
    rdmsr
    or eax, 1 << 8
    wrmsr

    ; Print checkpoint 5: EFER LME enabled
    mov edi, 0xB8000 + 160 * 5
    mov esi, boot_msg5
    call print_32
    
    ; Enable Paging, set NE, set MP, clear EM in CR0
    mov eax, cr0
    and eax, 0xFFFFFFFB ; Clear EM (bit 2)
    or eax, 1 << 31 | 1 << 5 | 1 << 1 ; Paging (31), NE (5), and MP (1)
    mov cr0, eax

    ; Print checkpoint 6: CR0 paging enabled
    mov edi, 0xB8000 + 160 * 6
    mov esi, boot_msg6
    call print_32
    
    ; Far jump to 64-bit code segment
    jmp gdt_code:long_mode_start

bits 64
extern kmain
long_mode_start:
    ; Set up segment registers for 64-bit data
    mov ax, gdt_data
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax

    ; Enable OSXSAVE (bit 18) in CR4
    mov rax, cr4
    or rax, 1 << 18
    mov cr4, rax

    ; Enable AVX and SSE in XCR0
    xor ecx, ecx
    xgetbv
    or rax, 7           ; AVX (bit 2) | SSE (bit 1) | x87 (bit 0)
    xsetbv

    ; Print checkpoint 7: Entered 64-bit mode
    mov rdi, 0xB8000 + 160 * 7
    mov rsi, boot_msg7
    call print_64
    
    ; Initialize MXCSR to mask all FP exceptions and enable FTZ/DAZ
    push rax
    mov eax, 0x9FC0
    push rax
    ldmxcsr [rsp]
    pop rax
    pop rax

    ; Print checkpoint 8: Segment registers and MXCSR loaded
    mov rdi, 0xB8000 + 160 * 8
    mov rsi, boot_msg8
    call print_64
    
    ; Print checkpoint 9: Invoking C kmain
    mov rdi, 0xB8000 + 160 * 9
    mov rsi, boot_msg9
    call print_64
    
    ; Pass arguments to kmain
    mov edi, [multiboot_magic]
    mov esi, [multiboot_info_ptr]
    
    ; Call C kernel
    call kmain
    
    cli
.halt_loop:
    hlt
    jmp .halt_loop

extern ap_main
global ap_long_mode_start
ap_long_mode_start:
    ; 1. SET UP STACK FIRST before any push/pop operations
    mov eax, 1
    lock xadd [ap_stack_index], eax
    
    ; Pass core index as first argument (original index)
    mov rdi, rax
    
    ; Allocate stack: ap_stacks + (index * 4096)
    imul eax, eax, 4096
    mov rsp, ap_stacks
    add rsp, rax
    add rsp, 4096 ; top of stack

    ; 2. NOW set up segment registers for 64-bit data
    mov ax, gdt_data
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax

    ; Enable OSXSAVE (bit 18) in CR4
    mov rax, cr4
    or rax, 1 << 18
    mov cr4, rax

    ; Enable AVX and SSE in XCR0
    xor ecx, ecx
    xgetbv
    or rax, 7           ; AVX (bit 2) | SSE (bit 1) | x87 (bit 0)
    xsetbv
    
    ; 3. Initialize MXCSR (Safe to use push/pop now)
    push rax
    mov eax, 0x9FC0
    push rax
    ldmxcsr [rsp]
    pop rax
    pop rax
    
    ; Call AP main
    call ap_main
    
    cli
.ap_halt:
    hlt
    jmp .ap_halt

bits 32
print_32:
    mov ah, 0x0F
.loop:
    lodsb
    test al, al
    jz .print_done
    stosw
    jmp .loop
.print_done:
    ret

bits 64
print_64:
    mov ah, 0x0F
.loop:
    lodsb
    test al, al
    jz .print_done
    stosw
    jmp .loop
.print_done:
    ret

extern keyboard_handler
global isr33
isr33:
    push rax
    push rcx
    push rdx
    push rbx
    push rbp
    push rsi
    push rdi
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15
    call keyboard_handler
    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rdi
    pop rsi
    pop rbp
    pop rbx
    pop rdx
    pop rcx
    pop rax
    iretq

extern gpf_handler
global isr13
isr13:
    push rax
    push rcx
    push rdx
    push rbx
    push rbp
    push rsi
    push rdi
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15
    
    sub rsp, 8           ; ALIGN STACK TO 16 BYTES FOR C ABI
    mov rdi, [rsp + 128] ; error code (offset shifted by 8)
    mov rsi, rsp         ; stack frame / regs
    call gpf_handler
    add rsp, 8           ; RESTORE STACK
    
    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rdi
    pop rsi
    pop rbp
    pop rbx
    pop rdx
    pop rcx
    pop rax
    add rsp, 8           ; remove error code
    iretq

extern pf_handler
global isr14
isr14:
    push rax
    push rcx
    push rdx
    push rbx
    push rbp
    push rsi
    push rdi
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15
    
    sub rsp, 8           ; ALIGN STACK TO 16 BYTES FOR C ABI
    mov rdi, [rsp + 128] ; error code (offset shifted by 8)
    mov rsi, rsp         ; stack frame
    call pf_handler
    add rsp, 8           ; RESTORE STACK
    
    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rdi
    pop rsi
    pop rbp
    pop rbx
    pop rdx
    pop rcx
    pop rax
    add rsp, 8           ; remove error code
    iretq

section .rodata
bsp_msg: db "BSP: Waking CPUs...", 0
done_msg: db "Done! CPUs awake: ", 0
boot_msg0: db "aiOS boot: Entered 32-bit _start", 0
boot_msg1: db "aiOS boot: APIC base MSR read successfully", 0
boot_msg2: db "aiOS boot: GDT descriptor loaded via lgdt", 0
boot_msg3: db "aiOS boot: CR4 registers configured (PAE/SSE)", 0
boot_msg4: db "aiOS boot: CR3 set to PML4 address", 0
boot_msg5: db "aiOS boot: EFER Long Mode (LME) bit enabled", 0
boot_msg6: db "aiOS boot: CR0 Paging enabled successfully", 0
boot_msg7: db "aiOS boot: Jumped to 64-bit long_mode_start", 0
boot_msg8: db "aiOS boot: Segment registers loaded. MXCSR set", 0
boot_msg9: db "aiOS boot: Invoking kmain...", 0

; Trampoline code definition (copied to 0x8000)
align 16
trampoline_start:
%include "ap_trampoline.asm"
trampoline_end:
