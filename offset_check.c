#include "cpu.h"

_Static_assert(__builtin_offsetof(cpu_t, self) == 0, "OFFSET_0_FAIL");
_Static_assert(__builtin_offsetof(cpu_t, syscall_scratch) == 8, "OFFSET_8_FAIL");
_Static_assert(__builtin_offsetof(cpu_t, gdt) == 24, "OFFSET_24_FAIL");
_Static_assert(__builtin_offsetof(cpu_t, tss) == 80, "OFFSET_80_FAIL");
// TSS.rsp0 is at offset 4 within TSS
_Static_assert(__builtin_offsetof(struct tss, rsp0) == 4, "TSS_RSP0_FAIL");
// So cpu.tss.rsp0 is at 80+4 = 84
