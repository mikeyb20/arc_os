; arc_os — STAC / CLAC only on CPUs that have SMAP (M1-19, protect.h)
;
; Both instructions raise #UD on a CPU without SMAP (the x86-64-v3 floor,
; Haswell, has none), so each use tests protect_smap_on first: one load and a
; predictable branch. They change only RFLAGS (the arithmetic flags too: the
; CMP), never a general register.

extern protect_smap_on

; Allow kernel access to user pages (RFLAGS.AC = 1): the user-copy routine only
%macro SMAP_STAC 0
    cmp byte [rel protect_smap_on], 0
    je %%done
    stac
%%done:
%endmacro

; Forbid it again (RFLAGS.AC = 0): after the copy, and on every kernel entry,
; since an interrupt or exception keeps the interrupted code's AC
%macro SMAP_CLAC 0
    cmp byte [rel protect_smap_on], 0
    je %%done
    clac
%%done:
%endmacro
