; Import address table entries (and direct-call thunks) for winxp_crt.c's stand-ins, which the static CRT links to
; instead of kernel32.lib's. In assembly because C can't spell the decorated names (__imp__Name@argbytes).
.686
.model flat
option casemap:none

stand_in macro name, argbytes
    EXTERN _xp_&name&@&argbytes&:PROC
    .const
    PUBLIC __imp__&name&@&argbytes&
    __imp__&name&@&argbytes& DD _xp_&name&@&argbytes&
    .code
    PUBLIC _&name&@&argbytes&
    _&name&@&argbytes&::
    jmp DWORD PTR [__imp__&name&@&argbytes&]
endm

stand_in FlsAlloc, 4
stand_in FlsFree, 4
stand_in FlsGetValue, 4
stand_in FlsSetValue, 8
stand_in InitializeCriticalSectionEx, 12
stand_in AcquireSRWLockExclusive, 4
stand_in ReleaseSRWLockExclusive, 4
stand_in SleepConditionVariableSRW, 16
stand_in WakeAllConditionVariable, 4

end
