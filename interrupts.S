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
; Entry: 0x23 (User Code), 0x1B (User Data) - Wait, we swapped them!
; GDT[3] = 0x1B (User Data)
; GDT[4] = 0x23 (User Code)
jump_to_usermode:
    ; Data segments for user (RPL 3)
    mov ax, 0x1B        ; User Data (Index 3, RPL 3)
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    
    ; Stack frame for IRETQ
    ; [RSP+32] SS     (User Data)
    ; [RSP+24] RSP    (User Stack)
    ; [RSP+16] RFLAGS (Interrupts enabled)
    ; [RSP+8]  CS     (User Code)
    ; [RSP+0]  RIP    (User Entry)
    
    push 0x1B           ; SS = User Data
    push rsi            ; RSP = User Stack
    pushfq
    pop rax
    or rax, 0x200       ; Enable interrupts (IF)
    push rax            ; RFLAGS
    push 0x23           ; CS = User Code (Index 4, RPL 3)
    push rdi            ; RIP = User Entry
    
    iretq

; -----------------------------------------------------------------------------
; Syscall Entry Point (LSTAR points here)
; -----------------------------------------------------------------------------
; RCX = User RIP
; R11 = User RFLAGS
; RDI, RSI, RDX, R10, R8, R9 = Arguments
; -----------------------------------------------------------------------------
section .data
    ; Temporary storage for user stack pointer (until we have per-cpu GS)
    global syscall_scratch_rsp
    syscall_scratch_rsp: dq 0

section .text

syscall_entry:
    ; 1. Swap to kernel GS (if we had it, skipping for global var approach)
    ; swapgs 
    
    ; 2. Save User RSP and load Kernel RSP
    mov [syscall_scratch_rsp], rsp
    
    ; Load kernel stack from TSS (we need to find it)
    ; Ideally use GS:[offset], but for now let's use a fixed location or the TSS global
    ; Actually, let's just use the end of our static kernel stack?
    ; Better: Get it from the TSS struct.
    ; extern kernel_tss
    ; rsp = kernel_tss.rsp0
    mov rsp, [kernel_tss_rsp0_ptr]  ; Need to export this pointer or value!
    
    ; 3. Enable interrupts? No, typically syscalls run with interrupts disabled initially
    ; or enabled. FMASK disabled them. Let's keep them disabled for now.
    
    ; 4. Save registers (System V ABI: RDI, RSI, RDX, RCX, R8, R9 are args)
    ; But for syscall: R10 is 4th arg (instead of RCX, because RCX is RIP)
    ; The kernel function syscall_handler expects standard ABI: RDI, RSI, RDX, RCX, R8, R9.
    ; So we need to move R10 -> RCX.
    ; But syscall_handler(num, arg1, arg2, arg3)
    ;   num  = RDI (syscall number) -> actually user passes syscall num in RAX usually.
    ;   Let's check our ABI.
    ;   User: RAX=syscall#, RDI=arg1, RSI=arg2, RDX=arg3
    ;   Kernel Handler: syscall_handler(num, arg1, arg2, arg3)
    ;   So we need:
    ;     RDI (Handler Arg1) = RAX (User Syscall#)
    ;     RSI (Handler Arg2) = RDI (User Arg1)
    ;     RDX (Handler Arg3) = RSI (User Arg2)
    ;     RCX (Handler Arg4) = RDX (User Arg3)
    
    push r11    ; Save User RFLAGS
    push rcx    ; Save User RIP
    
    ; Preserve callee-saved registers if needed, but we can trust C compiler to save them
    ; except the ones we clobber.
    
    ; Remap arguments for C function
    mov rcx, rdx    ; 4th arg = User Arg3
    mov rdx, rsi    ; 3rd arg = User Arg2
    mov rsi, rdi    ; 2nd arg = User Arg1
    mov rdi, rax    ; 1st arg = Syscall Number
    
    ; Stack alignment (16 bytes)
    ; We pushed 2 * 8 bytes (R11, RCX). RSP is 16-byte aligned if it started aligned.
    
    call syscall_handler
    
    ; Return value in RAX.
    
    pop rcx     ; Restore User RIP
    pop r11     ; Restore User RFLAGS
    
    ; Restore User Stack
    mov rsp, [syscall_scratch_rsp]
    
    ; swapgs
    
    sysretq

; Pointer to TSS RSP0 (initialized in C)
section .data
global kernel_tss_rsp0_ptr
kernel_tss_rsp0_ptr: dq 0

; -----------------------------------------------------------------------------
; Interrupt Service Routines (ISRs)
; -----------------------------------------------------------------------------
; ... (Rest of ISR code same as before)
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

%assign i 0
%rep 256
    ISR_NOERRCODE i
    %assign i i+1
%endrep

isr_common_stub:
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

    ; Save segment registers
    mov ax, ds
    push rax
    mov ax, es
    push rax
    mov ax, fs
    push rax
    mov ax, gs
    push rax

    ; Load kernel data segment
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    ; Get interrupt number (it's at RSP + (15 GPRs + 4 segs) * 8 + 8 = RSP + 160)
    ; Stack: [GS FS ES DS R15..RAX] [error_code] [int_no] [RIP CS RFLAGS RSP SS]
    mov rax, [rsp + 152]    ; Get interrupt number (152 = 19 * 8)

    ; Check if this is timer interrupt (vector 32 = IRQ0)
    cmp rax, 32
    je .timer_irq

    ; For other interrupts, call normal handler
    mov rdi, rsp
    extern isr_handler
    call isr_handler
    jmp .restore

.timer_irq:
    ; Timer interrupt - do context switch

    ; First, increment tick counter
    extern timer_tick
    call timer_tick

    ; Now do scheduler context switch
    ; Pass current RSP to scheduler_switch
    mov rdi, rsp
    extern scheduler_switch
    call scheduler_switch
    ; RAX now contains new RSP

    ; Switch to new task's stack
    mov rsp, rax

    ; Send EOI to PIC (acknowledge interrupt)
    mov al, 0x20
    out 0x20, al

.restore:
    ; Restore segment registers
    pop rax
    mov gs, ax
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

    ; Remove error code and interrupt number
    add rsp, 16

    ; Return from interrupt
    iretq

global isr_stub_table
isr_stub_table:
    %assign i 0
    %rep 256
        dq isr%+i
        %assign i i+1
    %endrep
