[BITS 64]

global smp_ap_trampoline
extern smp_ap_entry

; AP Startup Trampoline
; Limine jumps here with RDI = struct limine_smp_info *
smp_ap_trampoline:
    ; Load cpu_t * from extra_argument (offset 24)
    mov rsi, [rdi + 24]
    
    ; Load RSP from cpu_t->startup_stack_top (offset 8)
    mov rsp, [rsi + 8]
    
    ; Clear RBP for stack trace termination
    xor rbp, rbp
    
    ; Jump to C entry point (RDI is still info)
    call smp_ap_entry
    
    ; Should never return
.halt:
    hlt
    jmp .halt

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
;   offset 8:  startup_stack_top (8 bytes)
;   offset 16: syscall_scratch   (8 bytes) — user RSP saved here
;   offset 24: lapic_id          (4 bytes)
;   offset 28: cpu_id            (4 bytes)
;   offset 32: gdt[7]            (56 bytes)
;   offset 88: tss               (104 bytes, packed)
;   offset 92: tss.rsp0          (8 bytes) — kernel stack pointer
; -----------------------------------------------------------------------------
section .text

syscall_entry:
    swapgs              ; Swap GS to Kernel GS (cpu_t)

    ; Save User RSP to syscall_scratch (offset 16 in cpu_t)
    mov gs:[16], rsp

    ; Load Kernel RSP from TSS RSP0 (offset 92 in cpu_t)
    mov rsp, gs:[92]

    ; Synthesize struct interrupt_frame
    push 0x1B           ; SS
    push qword gs:[16]  ; RSP
    push r11            ; RFLAGS
    push 0x23           ; CS
    push rcx            ; RIP
    push 0              ; err
    push 0              ; int
    push rax            ; rax
    push rbx            ; rbx
    push rcx            ; rcx
    push rdx            ; rdx
    push rsi            ; rsi
    push rdi            ; rdi
    push rbp            ; rbp
    push r8             ; r8
    push r9             ; r9
    push r10            ; r10
    push r11            ; r11
    push r12            ; r12
    push r13            ; r13
    push r14            ; r14
    push r15            ; r15
    mov ax, ds
    push rax
    mov ax, es
    push rax
    mov ax, fs
    push rax

    ; Switch to Kernel Segments
    mov rdi, [rsp + 136] ; Load original RAX (syscall num) from stack
    mov ax, 0x10         ; Kernel Data Segment
    mov ds, ax
    mov es, ax
    mov fs, ax

    ; Call C handler: syscall_handler(num, regs)
    mov rsi, rsp    ; 2nd arg: regs
    call syscall_handler

    ; RAX has return value. Keep it.

    ; Restore segment registers
    pop rbx
    mov fs, bx
    pop rbx
    mov es, bx
    pop rbx
    mov ds, bx

    ; Restore GPRs
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
    add rsp, 8      ; Skip rax

    add rsp, 16     ; Skip int, err
    ; Stack is now: SS, RSP, RFLAGS, CS, RIP
    swapgs
    iretq

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
