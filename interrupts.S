[BITS 64]

global global_gdt_flush
global tss_flush
global jump_to_usermode
global syscall_entry

; Import C handlers
extern isr_handler
extern syscall_handler

; GDT Flush (reloads segment registers)
; void global_gdt_flush(uint64_t gdt_ptr);
global_gdt_flush:
    lgdt [rdi]
    mov ax, 0x10    ; Kernel Data Segment
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    push 0x08       ; Kernel Code Segment
    push .reload_cs
    retfq
.reload_cs:
    ret

; Load TSS segment (selector 0x28)
tss_flush:
    mov ax, 0x28
    ltr ax
    ret

; Jump to Ring 3 usermode
; rdi = user entry point
; rsi = user stack pointer
; Entry: 0x23 (User Code), 0x1B (User Data)
jump_to_usermode:
    ; Data segments for user (RPL 3)
    mov ax, 0x1B        ; User Data (Index 3, RPL 3)
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    ; Stack frame for IRETQ
    push 0x1B           ; SS = User Data
    push rsi            ; RSP = User Stack
    pushfq
    pop rax
    or rax, 0x200       ; Enable interrupts (IF)
    push rax            ; RFLAGS
    push 0x23           ; CS = User Code (Index 4, RPL 3)
    push rdi            ; RIP = User Entry

    swapgs              ; Swap GS to User GS
    iretq

; -----------------------------------------------------------------------------
; Syscall Entry Point (LSTAR points here)
; -----------------------------------------------------------------------------
; cpu_t layout (must match cpu.h):
;   offset 0:  self              (8 bytes) — get_cpu() reads this
;   offset 8:  syscall_scratch   (8 bytes) — user RSP saved here
;   offset 16: lapic_id          (4 bytes)
;   offset 20: cpu_id            (4 bytes)
;   offset 24: gdt[7]            (56 bytes)
;   offset 80: tss               (104 bytes, packed)
;   offset 84: tss.rsp0          (8 bytes) — kernel stack pointer
; -----------------------------------------------------------------------------
section .text

syscall_entry:
    swapgs              ; Swap GS to Kernel GS (cpu_t)

    ; Save User RSP to syscall_scratch (offset 8 in cpu_t)
    mov gs:[8], rsp

    ; Load Kernel RSP from TSS RSP0 (offset 84 in cpu_t)
    mov rsp, gs:[84]

    ; Save User RFLAGS and RIP (RCX/R11 are clobbered by sysret, but we need to preserve them)
    push r11    ; Save User RFLAGS
    push rcx    ; Save User RIP

    ; Switch to Kernel Segments
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    ; SS is already Kernel (handled by CPU)

    ; Remap arguments for C function
    mov rcx, rdx    ; 4th arg = User Arg3
    mov rdx, rsi    ; 3rd arg = User Arg2
    mov rsi, rdi    ; 2nd arg = User Arg1
    mov rdi, rax    ; 1st arg = Syscall Number
    xor r8d, r8d    ; 5th arg = NULL (no register frame for SYSCALL path)

    call syscall_handler

    ; Restore User Segments
    mov dx, 0x1B
    mov ds, dx
    mov es, dx

    pop rcx     ; Restore User RIP
    pop r11     ; Restore User RFLAGS

    ; Restore User Stack from syscall_scratch (offset 8 in cpu_t)
    mov rsp, gs:[8]

    swapgs      ; Swap back to User GS

    sysretq

; -----------------------------------------------------------------------------
; Interrupt Service Routines (ISRs)
; -----------------------------------------------------------------------------
%macro ISR_NOERRCODE 1
    global isr%1
    isr%1:
        cli
        push 0
        push %1
        jmp isr_common_stub
%endmacro

%macro ISR_ERRCODE 1
    global isr%1
    isr%1:
        cli
        push %1
        jmp isr_common_stub
%endmacro

; Define ISRs manually to handle error codes correctly
ISR_NOERRCODE 0
ISR_NOERRCODE 1
ISR_NOERRCODE 2
ISR_NOERRCODE 3
ISR_NOERRCODE 4
ISR_NOERRCODE 5
ISR_NOERRCODE 6
ISR_NOERRCODE 7
ISR_ERRCODE   8  ; Double Fault
ISR_NOERRCODE 9
ISR_ERRCODE   10 ; Invalid TSS
ISR_ERRCODE   11 ; Segment Not Present
ISR_ERRCODE   12 ; Stack-Segment Fault
ISR_ERRCODE   13 ; General Protection Fault
ISR_ERRCODE   14 ; Page Fault
ISR_NOERRCODE 15
ISR_NOERRCODE 16
ISR_ERRCODE   17 ; Alignment Check
ISR_NOERRCODE 18
ISR_NOERRCODE 19
ISR_NOERRCODE 20
ISR_NOERRCODE 21
ISR_NOERRCODE 22
ISR_NOERRCODE 23
ISR_NOERRCODE 24
ISR_NOERRCODE 25
ISR_NOERRCODE 26
ISR_NOERRCODE 27
ISR_NOERRCODE 28
ISR_NOERRCODE 29
ISR_ERRCODE   30 ; Security Exception
ISR_NOERRCODE 31

; Remainder (32-255) are IRQs and Syscalls, no error code
%assign i 32
%rep 224
    ISR_NOERRCODE i
    %assign i i+1
%endrep

isr_common_stub:
    ; Stack layout at this point:
    ;   [rsp+0]  = int_no  (pushed by ISR macro)
    ;   [rsp+8]  = err_code (pushed by ISR macro or CPU)
    ;   [rsp+16] = RIP     (pushed by CPU)
    ;   [rsp+24] = CS      (pushed by CPU)
    ;   [rsp+32] = RFLAGS  (pushed by CPU)
    ;   [rsp+40] = RSP     (pushed by CPU, if privilege change)
    ;   [rsp+48] = SS      (pushed by CPU, if privilege change)

    ; Check if we came from User Mode (CS & 3 == 3)
    cmp word [rsp + 24], 0x08   ; Check CS == Kernel Code
    je .entry_kernel
    swapgs                      ; Came from User, swap GS
.entry_kernel:

    ; Save all general purpose registers
    push rax
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi
    push rbp
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15

    ; Save segment registers (except GS)
    mov ax, ds
    push rax
    mov ax, es
    push rax
    mov ax, fs
    push rax

    ; Load kernel data segment
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    ; GS points to cpu_t via MSR (because we swapgs'd if needed)

    ; Call C handler — returns new RSP (for context switch) or same RSP
    mov rdi, rsp
    call isr_handler

    ; isr_handler returns new RSP in RAX.
    ; For non-switching interrupts, RAX == old RSP (no-op).
    ; For timer/yield, RAX may point to a different task's saved registers.
    mov rsp, rax

    ; Restore segment registers (from potentially new task's stack)
    pop rax
    mov fs, ax
    pop rax
    mov es, ax
    pop rax
    mov ds, ax

    ; Restore general purpose registers
    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rbp
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    pop rax

    ; Check if returning to User Mode (may be a DIFFERENT task than entry!)
    ; Stack: [int_no] [err_code] [RIP] [CS] ...
    cmp word [rsp + 24], 0x08
    je .exit_kernel
    swapgs                      ; Returning to User, swap GS
.exit_kernel:

    ; Remove error code and interrupt number
    add rsp, 16

    iretq

global isr_stub_table
isr_stub_table:
    %assign i 0
    %rep 256
        dq isr%+i
        %assign i i+1
    %endrep
