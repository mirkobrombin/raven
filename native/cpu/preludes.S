/* The routines the runtime puts into a guest to answer a call there, from what it publishes in the thread's served block, instead of
 * stopping the guest for it (see the prelude functions of native/nt/providers.cpp for what each answers and when it gives up). The
 * runtime copies these bytes into the guest; it never runs them itself.
 *
 * Those that read the served block load its pointer first, with a rip-relative load whose displacement, at byte 3, the runtime fills
 * in; those that need no block take its address with a lea in the same place. Each ends at `slow`, a jmp whose rel32 the runtime points
 * at the export's breakpoint (or at the wrapper an answer taken while other threads run gives up to), so whatever the routine does not
 * answer goes to the runtime with the registers the caller left. */

	.intel_syntax noprefix
	.section .rodata, "a"

/* GetTickCount and GetTickCount64 as Windows answers them: the tick count of KUSER_SHARED_DATA, which the host keeps in milliseconds
 * (its multiplier is one), read as a KSYSTEM_TIME: the first high word, the low word, and again while the second high word differs.
 * The lea only keeps the layout of the preludes that load the block pointer; the jump is never taken. */
	.global raven_prelude_tick, raven_prelude_tick_end
raven_prelude_tick:
	lea r10, [rip + 0]
1:	mov edx, dword ptr ds:0x7ffe0324
	mov eax, dword ptr ds:0x7ffe0320
	cmp edx, dword ptr ds:0x7ffe0328
	jne 1b
	shl rdx, 0x20
	or rax, rdx
	ret
.Ltick_slow:
	.byte 0xe9
	.long 0
raven_prelude_tick_end:

/* mov r10,[pointer]; test r10,r10; jz slow; test rcx,rcx; jnz slow; xor eax,eax; mov r11d,1; lock cmpxchg [r10+state],r11d; jne slow;
 * mov rax,[r10+context]; test rax,rax; jz undo; ret; undo: mov dword [r10+state],0; slow: jmp breakpoint */
	.global raven_prelude_screen_get_dc, raven_prelude_screen_get_dc_end
raven_prelude_screen_get_dc:
	mov r10, qword ptr [rip + 0]
	test r10, r10
	je .Lscreen_get_dc_slow
	test rcx, rcx
	jne .Lscreen_get_dc_slow
	xor eax, eax
	mov r11d, 0x1
	lock cmpxchg dword ptr [r10 + 0x608], r11d
	jne .Lscreen_get_dc_slow
	mov rax, qword ptr [r10 + 0x600]
	test rax, rax
	je .Lscreen_get_dc_31
	ret
.Lscreen_get_dc_31:
	mov dword ptr [r10 + 0x608], 0x0
.Lscreen_get_dc_slow:
	.byte 0xe9
	.long 0
raven_prelude_screen_get_dc_end:

/* mov r10,[pointer]; test r10,r10; jz slow; test rcx,rcx; jnz slow; test rdx,rdx; jz slow; cmp rdx,[r10+context]; jne slow; mov eax,1;
 * xor r11d,r11d; lock cmpxchg [r10+state],r11d; jne slow; ret; slow: jmp breakpoint */
	.global raven_prelude_screen_release_dc, raven_prelude_screen_release_dc_end
raven_prelude_screen_release_dc:
	mov r10, qword ptr [rip + 0]
	test r10, r10
	je .Lscreen_release_dc_slow
	test rcx, rcx
	jne .Lscreen_release_dc_slow
	test rdx, rdx
	je .Lscreen_release_dc_slow
	cmp rdx, qword ptr [r10 + 0x600]
	jne .Lscreen_release_dc_slow
	mov eax, 0x1
	xor r11d, r11d
	lock cmpxchg dword ptr [r10 + 0x608], r11d
	jne .Lscreen_release_dc_slow
	ret
.Lscreen_release_dc_slow:
	.byte 0xe9
	.long 0
raven_prelude_screen_release_dc_end:

/* mov r10,[pointer]; test r10,r10; jz slow; test rcx,rcx; jz slow; cmp edx,127; ja slow; the screen's context answers from the caps
 * table; another context answers when the attribute table holds its slot (see dc_text_color_prelude) and the index does not depend on
 * its bitmap: TECHNOLOGY 1 (DT_RASDISPLAY, a memory context's too), LOGPIXELSX and LOGPIXELSY 96, PLANES 1, SHADEBLENDCAPS 0, as
 * device_caps answers them; anything else: jmp breakpoint. GDI+ asks these thousands of times while it paints. The slot's window flag
 * that TECHNOLOGY once read is left unreached (mov eax,[rax+0x18]; ret). */
	.global raven_prelude_screen_device_caps, raven_prelude_screen_device_caps_end
raven_prelude_screen_device_caps:
	mov r10, qword ptr [rip + 0]
	test r10, r10
	je .Lscreen_device_caps_slow
	test rcx, rcx
	je .Lscreen_device_caps_slow
	cmp edx, 0x7f
	ja .Lscreen_device_caps_slow
	cmp rcx, qword ptr [r10 + 0x600]
	je .Lscreen_device_caps_70
	mov r11, qword ptr [r10 + 0x748]
	test r11, r11
	je .Lscreen_device_caps_slow
	mov eax, ecx
	and eax, 0x3ff
	shl eax, 0x5
	lea rax, [r11 + rax * 1 + 0x1000]
	cmp rcx, qword ptr [rax]
	jne .Lscreen_device_caps_slow
	cmp edx, 0x2
	je .Lscreen_device_caps_67
	cmp edx, 0x58
	je .Lscreen_device_caps_61
	cmp edx, 0x5a
	je .Lscreen_device_caps_61
	cmp edx, 0xe
	je .Lscreen_device_caps_67
	cmp edx, 0x78
	je .Lscreen_device_caps_6d
	jmp .Lscreen_device_caps_slow
	mov eax, dword ptr [rax + 0x18]
	ret
.Lscreen_device_caps_61:
	mov eax, 0x60
	ret
.Lscreen_device_caps_67:
	mov eax, 0x1
	ret
.Lscreen_device_caps_6d:
	xor eax, eax
	ret
.Lscreen_device_caps_70:
	mov eax, edx
	mov eax, dword ptr [r10 + rax * 4 + 0x400]
	ret
.Lscreen_device_caps_slow:
	.byte 0xe9
	.long 0
raven_prelude_screen_device_caps_end:

	.global raven_prelude_window_long_ptr, raven_prelude_window_long_ptr_end
raven_prelude_window_long_ptr:
	mov r10, qword ptr [rip + 0]
	test r10, r10
	je .Lwindow_long_ptr_slow
	mov r11, qword ptr [r10 + 0x700]
	test r11, r11
	je .Lwindow_long_ptr_slow
	test rcx, rcx
	je .Lwindow_long_ptr_slow
	mov eax, ecx
	and eax, 0x1ff
	shl eax, 0x8
	add r11, rax
	cmp rcx, qword ptr [r11]
	jne .Lwindow_long_ptr_slow
	test edx, edx
	js .Lwindow_long_ptr_4c
	mov eax, edx
	lea r10, [rax + 0x8]
	cmp r10d, dword ptr [r11 + 0x50]
	ja .Lwindow_long_ptr_slow
	mov rax, qword ptr [r11 + rax * 1 + 0xa0]
	ret
.Lwindow_long_ptr_4c:
	cmp edx, 0xffffffeb
	jne .Lwindow_long_ptr_56
	mov rax, qword ptr [r11 + 0x8]
	ret
.Lwindow_long_ptr_56:
	cmp edx, 0xfffffffc
	jne .Lwindow_long_ptr_60
	mov rax, qword ptr [r11 + 0x10]
	ret
.Lwindow_long_ptr_60:
	cmp edx, 0xfffffffa
	jne .Lwindow_long_ptr_6a
	mov rax, qword ptr [r11 + 0x18]
	ret
.Lwindow_long_ptr_6a:
	cmp edx, 0xfffffff8
	jne .Lwindow_long_ptr_74
	mov rax, qword ptr [r11 + 0x20]
	ret
.Lwindow_long_ptr_74:
	cmp edx, 0xfffffff4
	jne .Lwindow_long_ptr_7e
	mov rax, qword ptr [r11 + 0x28]
	ret
.Lwindow_long_ptr_7e:
	cmp edx, 0xfffffff0
	jne .Lwindow_long_ptr_88
	mov eax, dword ptr [r11 + 0x30]
	ret
.Lwindow_long_ptr_88:
	cmp edx, 0xffffffec
	jne .Lwindow_long_ptr_slow
	mov eax, dword ptr [r11 + 0x34]
	ret
.Lwindow_long_ptr_slow:
	.byte 0xe9
	.long 0
raven_prelude_window_long_ptr_end:

	.global raven_prelude_window_long, raven_prelude_window_long_end
raven_prelude_window_long:
	mov r10, qword ptr [rip + 0]
	test r10, r10
	je .Lwindow_long_slow
	mov r11, qword ptr [r10 + 0x700]
	test r11, r11
	je .Lwindow_long_slow
	test rcx, rcx
	je .Lwindow_long_slow
	mov eax, ecx
	and eax, 0x1ff
	shl eax, 0x8
	add r11, rax
	cmp rcx, qword ptr [r11]
	jne .Lwindow_long_slow
	test edx, edx
	js .Lwindow_long_48
	mov eax, edx
	lea r10, [rax + 0x4]
	cmp r10d, dword ptr [r11 + 0x50]
	ja .Lwindow_long_slow
	mov eax, dword ptr [r11 + rax * 1 + 0xa0]
	ret
.Lwindow_long_48:
	cmp edx, 0xffffffeb
	jne .Lwindow_long_52
	mov eax, dword ptr [r11 + 0x8]
	ret
.Lwindow_long_52:
	cmp edx, 0xfffffff4
	jne .Lwindow_long_5c
	mov eax, dword ptr [r11 + 0x28]
	ret
.Lwindow_long_5c:
	cmp edx, 0xfffffff0
	jne .Lwindow_long_66
	mov eax, dword ptr [r11 + 0x30]
	ret
.Lwindow_long_66:
	cmp edx, 0xffffffec
	jne .Lwindow_long_slow
	mov eax, dword ptr [r11 + 0x34]
	ret
.Lwindow_long_slow:
	.byte 0xe9
	.long 0
raven_prelude_window_long_end:

	.global raven_prelude_window_property, raven_prelude_window_property_end
raven_prelude_window_property:
	mov r10, qword ptr [rip + 0]
	test r10, r10
	je .Lwindow_property_slow
	mov r11, qword ptr [r10 + 0x700]
	test r11, r11
	je .Lwindow_property_slow
	test rcx, rcx
	je .Lwindow_property_slow
	mov eax, ecx
	and eax, 0x1ff
	shl eax, 0x8
	add r11, rax
	cmp rcx, qword ptr [r11]
	jne .Lwindow_property_slow
	cmp rdx, 0xffff
	ja .Lwindow_property_slow
	mov eax, dword ptr [r11 + 0x54]
	cmp eax, 0x4
	ja .Lwindow_property_slow
	lea r10, [r11 + 0x60]
.Lwindow_property_45:
	test eax, eax
	je .Lwindow_property_56
	cmp rdx, qword ptr [r10]
	je .Lwindow_property_59
	add r10, 0x10
	dec eax
	jmp .Lwindow_property_45
.Lwindow_property_56:
	xor eax, eax
	ret
.Lwindow_property_59:
	mov rax, qword ptr [r10 + 0x8]
	ret
.Lwindow_property_slow:
	.byte 0xe9
	.long 0
raven_prelude_window_property_end:

	.global raven_prelude_window_exists, raven_prelude_window_exists_end
raven_prelude_window_exists:
	mov r10, qword ptr [rip + 0]
	test r10, r10
	je .Lwindow_exists_slow
	mov r11, qword ptr [r10 + 0x700]
	test r11, r11
	je .Lwindow_exists_slow
	test rcx, rcx
	je .Lwindow_exists_slow
	mov eax, ecx
	and eax, 0x1ff
	shl eax, 0x8
	add r11, rax
	cmp rcx, qword ptr [r11]
	jne .Lwindow_exists_slow
	mov eax, 0x1
	ret
.Lwindow_exists_slow:
	.byte 0xe9
	.long 0
raven_prelude_window_exists_end:

	.global raven_prelude_window_thread, raven_prelude_window_thread_end
raven_prelude_window_thread:
	mov r10, qword ptr [rip + 0]
	test r10, r10
	je .Lwindow_thread_slow
	mov r11, qword ptr [r10 + 0x700]
	test r11, r11
	je .Lwindow_thread_slow
	test rcx, rcx
	je .Lwindow_thread_slow
	mov eax, ecx
	and eax, 0x1ff
	shl eax, 0x8
	add r11, rax
	cmp rcx, qword ptr [r11]
	jne .Lwindow_thread_slow
	mov eax, dword ptr [r11 + 0xf0]
	test eax, eax
	je .Lwindow_thread_slow
	test rdx, rdx
	je .Lwindow_thread_49
	mov r10d, dword ptr [r11 + 0xf4]
	mov dword ptr [rdx], r10d
.Lwindow_thread_49:
	ret
.Lwindow_thread_slow:
	.byte 0xe9
	.long 0
raven_prelude_window_thread_end:

	.global raven_prelude_window_client, raven_prelude_window_client_end
raven_prelude_window_client:
	mov r10, qword ptr [rip + 0]
	test r10, r10
	je .Lwindow_client_slow
	mov r11, qword ptr [r10 + 0x700]
	test r11, r11
	je .Lwindow_client_slow
	test rcx, rcx
	je .Lwindow_client_slow
	mov eax, ecx
	and eax, 0x1ff
	shl eax, 0x8
	add r11, rax
	cmp rcx, qword ptr [r11]
	jne .Lwindow_client_slow
	test rdx, rdx
	je .Lwindow_client_slow
	mov qword ptr [rdx], 0x0
	mov rax, qword ptr [r11 + 0x38]
	mov qword ptr [rdx + 0x8], rax
	mov eax, 0x1
	ret
.Lwindow_client_slow:
	.byte 0xe9
	.long 0
raven_prelude_window_client_end:

	.global raven_prelude_window_outer, raven_prelude_window_outer_end
raven_prelude_window_outer:
	mov r10, qword ptr [rip + 0]
	test r10, r10
	je .Lwindow_outer_slow
	mov r11, qword ptr [r10 + 0x700]
	test r11, r11
	je .Lwindow_outer_slow
	test rcx, rcx
	je .Lwindow_outer_slow
	mov eax, ecx
	and eax, 0x1ff
	shl eax, 0x8
	add r11, rax
	cmp rcx, qword ptr [r11]
	jne .Lwindow_outer_slow
	test rdx, rdx
	je .Lwindow_outer_slow
	mov rax, qword ptr [r11 + 0x40]
	mov qword ptr [rdx], rax
	mov rax, qword ptr [r11 + 0x48]
	mov qword ptr [rdx + 0x8], rax
	mov eax, 0x1
	ret
.Lwindow_outer_slow:
	.byte 0xe9
	.long 0
raven_prelude_window_outer_end:

	.global raven_prelude_locale, raven_prelude_locale_end
raven_prelude_locale:
	mov r10, qword ptr [rip + 0]
	test r10, r10
	je .Llocale_slow
	mov r11, qword ptr [r10 + 0x708]
	test r11, r11
	je .Llocale_slow
	mov eax, edx
	and eax, 0x5fffffff
	cmp eax, 0x80
	jb .Llocale_3d
	sub eax, 0x1000
	cmp eax, 0x10
	jae .Llocale_slow
	add eax, 0x80
.Llocale_3d:
	movzx r10d, word ptr [r11 + rax * 8]
	cmp r10d, 0xffff
	je .Llocale_slow
	test edx, 0x20000000
	jne .Llocale_8b
	movzx eax, word ptr [r11 + rax * 8 + 0x2]
	inc eax
	test r9d, r9d
	je .Llocale_8a
	js .Llocale_slow
	cmp r9d, eax
	jb .Llocale_slow
	test r8, r8
	je .Llocale_slow
	lea r10, [r11 + r10 * 2 + 0x480]
	mov ecx, eax
.Llocale_76:
	mov dx, word ptr [r10]
	mov word ptr [r8], dx
	add r10, 0x2
	add r8, 0x2
	dec ecx
	jne .Llocale_76
.Llocale_8a:
	ret
.Llocale_8b:
	cmp r9d, 0x2
	jl .Llocale_slow
	test r8, r8
	je .Llocale_slow
	mov eax, dword ptr [r11 + rax * 8 + 0x4]
	mov dword ptr [r8], eax
	mov eax, 0x2
	ret
.Llocale_slow:
	.byte 0xe9
	.long 0
raven_prelude_locale_end:

	.global raven_prelude_nt_commit, raven_prelude_nt_commit_end
raven_prelude_nt_commit:
	mov r10, qword ptr [rip + 0]
	test r10, r10
	je .Lnt_commit_slow
	cmp rcx, 0xffffffffffffffff
	jne .Lnt_commit_slow
	test r8, r8
	jne .Lnt_commit_slow
	test rdx, rdx
	je .Lnt_commit_slow
	test r9, r9
	je .Lnt_commit_slow
	cmp dword ptr [rsp + 0x28], 0x1000
	jne .Lnt_commit_slow
	cmp dword ptr [rsp + 0x30], 0x4
	jne .Lnt_commit_slow
	mov rax, qword ptr [rdx]
	test rax, rax
	je .Lnt_commit_slow
	mov r11, qword ptr [r9]
	test r11, r11
	je .Lnt_commit_slow
	lea r11, [rax + r11 * 1 + 0xfff]
	and r11, 0xfffffffffffff000
	and rax, 0xfffffffffffff000
	cmp r11, rax
	jbe .Lnt_commit_slow
	mov ecx, dword ptr [r10 + 0xa00]
	lea r8, [r10 + 0xa08]
.Lnt_commit_8a:
	test ecx, ecx
	je .Lnt_commit_d4
	cmp rax, qword ptr [r8]
	jb .Lnt_commit_cc
	cmp r11, qword ptr [r8 + 0x8]
	ja .Lnt_commit_cc
	mov ecx, 0x1
	lock xadd dword ptr [r10 + 0xd10], ecx
	cmp ecx, 0x20
	jae .Lnt_commit_d4
	shl rcx, 0x4
	mov qword ptr [r10 + rcx * 1 + 0xd18], rax
	mov qword ptr [r10 + rcx * 1 + 0xd20], r11
	mov qword ptr [rdx], rax
	sub r11, rax
	mov qword ptr [r9], r11
	xor eax, eax
	ret
.Lnt_commit_cc:
	add r8, 0x10
	dec ecx
	jmp .Lnt_commit_8a
.Lnt_commit_d4:
	mov rcx, 0xffffffffffffffff
	xor r8d, r8d
.Lnt_commit_slow:
	.byte 0xe9
	.long 0
raven_prelude_nt_commit_end:

	.global raven_prelude_nt_decommit, raven_prelude_nt_decommit_end
raven_prelude_nt_decommit:
	mov r10, qword ptr [rip + 0]
	test r10, r10
	je .Lnt_decommit_slow
	cmp rcx, 0xffffffffffffffff
	jne .Lnt_decommit_slow
	test rdx, rdx
	je .Lnt_decommit_slow
	test r8, r8
	je .Lnt_decommit_slow
	cmp r9d, 0x4000
	jne .Lnt_decommit_slow
	mov rax, qword ptr [rdx]
	test rax, rax
	je .Lnt_decommit_slow
	mov r11, qword ptr [r8]
	test r11, r11
	je .Lnt_decommit_slow
	cmp r11, 0x40000
	ja .Lnt_decommit_slow
	lea r11, [rax + r11 * 1 + 0xfff]
	and r11, 0xfffffffffffff000
	and rax, 0xfffffffffffff000
	cmp r11, rax
	jbe .Lnt_decommit_slow
	mov ecx, dword ptr [r10 + 0xa00]
	lea r9, [r10 + 0xa08]
.Lnt_decommit_86:
	test ecx, ecx
	je .Lnt_decommit_e8
	cmp rax, qword ptr [r9]
	jb .Lnt_decommit_e0
	cmp r11, qword ptr [r9 + 0x8]
	ja .Lnt_decommit_e0
	mov ecx, 0x1
	lock xadd dword ptr [r10 + 0xd10], ecx
	cmp ecx, 0x20
	jae .Lnt_decommit_e8
	shl rcx, 0x4
	mov r9, rcx
	push rdi
	push rax
	mov rdi, rax
	mov rcx, r11
	sub rcx, rax
	xor eax, eax
	rep stos byte ptr [rdi], al
	pop rax
	pop rdi
	lea rcx, [rax + 0x1]
	mov qword ptr [r10 + r9 * 1 + 0xd18], rcx
	mov qword ptr [r10 + r9 * 1 + 0xd20], r11
	mov qword ptr [rdx], rax
	sub r11, rax
	mov qword ptr [r8], r11
	xor eax, eax
	ret
.Lnt_decommit_e0:
	add r9, 0x10
	dec ecx
	jmp .Lnt_decommit_86
.Lnt_decommit_e8:
	mov rcx, 0xffffffffffffffff
	mov r9d, 0x4000
.Lnt_decommit_slow:
	.byte 0xe9
	.long 0
raven_prelude_nt_decommit_end:

	.global raven_prelude_window_child, raven_prelude_window_child_end
raven_prelude_window_child:
	mov r10, qword ptr [rip + 0]
	test r10, r10
	je .Lwindow_child_slow
	mov r11, qword ptr [r10 + 0x700]
	test r11, r11
	je .Lwindow_child_slow
	xor eax, eax
	test rcx, rcx
	je .Lwindow_child_6a
	test rdx, rdx
	je .Lwindow_child_6a
	cmp rcx, rdx
	je .Lwindow_child_6a
	mov r8d, 0x40
	mov r9, rdx
.Lwindow_child_32:
	mov eax, r9d
	and eax, 0x1ff
	shl eax, 0x8
	add rax, r11
	cmp r9, qword ptr [rax]
	jne .Lwindow_child_5a
	mov r9, qword ptr [rax + 0x58]
	xor eax, eax
	test r9, r9
	je .Lwindow_child_6a
	cmp r9, rcx
	je .Lwindow_child_65
	dec r8d
	jne .Lwindow_child_32
.Lwindow_child_5a:
	mov r8d, 0x0
	xor r9d, r9d
	jmp .Lwindow_child_slow
.Lwindow_child_65:
	mov eax, 0x1
.Lwindow_child_6a:
	ret
.Lwindow_child_slow:
	.byte 0xe9
	.long 0
raven_prelude_window_child_end:

	.global raven_prelude_window_dpi, raven_prelude_window_dpi_end
raven_prelude_window_dpi:
	mov r10, qword ptr [rip + 0]
	mov eax, 0x60
	ret
	.byte 0xe9
	.long 0
raven_prelude_window_dpi_end:

	.global raven_prelude_focus, raven_prelude_focus_end
raven_prelude_focus:
	mov r10, qword ptr [rip + 0]
	test r10, r10
	je .Lfocus_slow
	cmp qword ptr [r10 + 0x710], 0x0
	je .Lfocus_slow
	mov rax, qword ptr [r10 + 0x718]
	ret
.Lfocus_slow:
	.byte 0xe9
	.long 0
raven_prelude_focus_end:

	.global raven_prelude_foreground, raven_prelude_foreground_end
raven_prelude_foreground:
	mov r10, qword ptr [rip + 0]
	test r10, r10
	je .Lforeground_slow
	cmp qword ptr [r10 + 0x710], 0x0
	je .Lforeground_slow
	mov rax, qword ptr [r10 + 0x798]
	ret
.Lforeground_slow:
	.byte 0xe9
	.long 0
raven_prelude_foreground_end:

	.global raven_prelude_default_procedure, raven_prelude_default_procedure_end
raven_prelude_default_procedure:
	mov r10, qword ptr [rip + 0]
	test r10, r10
	je .Ldefault_procedure_slow
	mov r11, qword ptr [r10 + 0x700]
	test r11, r11
	je .Ldefault_procedure_slow
	mov eax, ecx
	and eax, 0x1ff
	shl eax, 0x8
	cmp rcx, qword ptr [r11 + rax * 1]
	jne .Ldefault_procedure_slow
	cmp edx, 0xc000
	jb .Ldefault_procedure_3b
	cmp edx, 0xffff
	ja .Ldefault_procedure_slow
	xor eax, eax
	ret
.Ldefault_procedure_3b:
	lea eax, [rdx - 0x200]
	cmp eax, 0x8
	ja .Ldefault_procedure_slow
	mov r11d, 0x197
	bt r11d, eax
	jae .Ldefault_procedure_slow
	xor eax, eax
	ret
.Ldefault_procedure_slow:
	.byte 0xe9
	.long 0
raven_prelude_default_procedure_end:

	.global raven_prelude_cursor_position, raven_prelude_cursor_position_end
raven_prelude_cursor_position:
	mov r10, qword ptr [rip + 0]
	test r10, r10
	je .Lcursor_position_slow
	cmp qword ptr [r10 + 0x710], 0x0
	je .Lcursor_position_slow
	test rcx, rcx
	je .Lcursor_position_slow
	mov rax, qword ptr [r10 + 0x7a0]
	mov qword ptr [rcx], rax
	mov eax, 0x1
	ret
.Lcursor_position_slow:
	.byte 0xe9
	.long 0
raven_prelude_cursor_position_end:

	.global raven_prelude_menu_count, raven_prelude_menu_count_end
raven_prelude_menu_count:
	mov r10, qword ptr [rip + 0]
	test r10, r10
	je .Lmenu_count_slow
	mov r11, qword ptr [r10 + 0x720]
	test r11, r11
	je .Lmenu_count_slow
	test rcx, rcx
	je .Lmenu_count_slow
	mov eax, ecx
	and eax, 0x3ff
	shl eax, 0x4
	add rax, r11
	cmp rcx, qword ptr [rax]
	jne .Lmenu_count_slow
	mov eax, dword ptr [rax + 0xc]
	ret
.Lmenu_count_slow:
	.byte 0xe9
	.long 0
raven_prelude_menu_count_end:

	.global raven_prelude_menu_submenu, raven_prelude_menu_submenu_end
raven_prelude_menu_submenu:
	mov r10, qword ptr [rip + 0]
	test r10, r10
	je .Lmenu_submenu_slow
	mov r11, qword ptr [r10 + 0x720]
	test r11, r11
	je .Lmenu_submenu_slow
	test rcx, rcx
	je .Lmenu_submenu_slow
	mov eax, ecx
	and eax, 0x3ff
	shl eax, 0x4
	add rax, r11
	cmp rcx, qword ptr [rax]
	jne .Lmenu_submenu_slow
	cmp edx, dword ptr [rax + 0xc]
	jae .Lmenu_submenu_47
	mov r10d, dword ptr [rax + 0x8]
	add r10, r11
	mov eax, edx
	shl rax, 0x5
	mov rax, qword ptr [r10 + rax * 1 + 0x10]
	ret
.Lmenu_submenu_47:
	xor eax, eax
	ret
.Lmenu_submenu_slow:
	.byte 0xe9
	.long 0
raven_prelude_menu_submenu_end:

	.global raven_prelude_menu_id, raven_prelude_menu_id_end
raven_prelude_menu_id:
	mov r10, qword ptr [rip + 0]
	test r10, r10
	je .Lmenu_id_slow
	mov r11, qword ptr [r10 + 0x720]
	test r11, r11
	je .Lmenu_id_slow
	test rcx, rcx
	je .Lmenu_id_slow
	mov eax, ecx
	and eax, 0x3ff
	shl eax, 0x4
	add rax, r11
	cmp rcx, qword ptr [rax]
	jne .Lmenu_id_slow
	cmp edx, dword ptr [rax + 0xc]
	jae .Lmenu_id_50
	mov r10d, dword ptr [rax + 0x8]
	add r10, r11
	mov eax, edx
	shl rax, 0x5
	add r10, rax
	cmp qword ptr [r10 + 0x10], 0x0
	jne .Lmenu_id_50
	mov eax, dword ptr [r10 + 0x8]
	ret
.Lmenu_id_50:
	mov eax, 0xffffffff
	ret
.Lmenu_id_slow:
	.byte 0xe9
	.long 0
raven_prelude_menu_id_end:

	.global raven_prelude_menu_state, raven_prelude_menu_state_end
raven_prelude_menu_state:
	mov r10, qword ptr [rip + 0]
	test r10, r10
	je .Lmenu_state_slow
	mov r11, qword ptr [r10 + 0x720]
	test r11, r11
	je .Lmenu_state_slow
	test rcx, rcx
	je .Lmenu_state_slow
	test r8d, 0x400
	je .Lmenu_state_5f
	mov eax, ecx
	and eax, 0x3ff
	shl eax, 0x4
	add rax, r11
	cmp rcx, qword ptr [rax]
	jne .Lmenu_state_slow
	cmp edx, dword ptr [rax + 0xc]
	jae .Lmenu_state_c1
	mov r10d, dword ptr [rax + 0x8]
	add r10, r11
	mov eax, edx
	shl rax, 0x5
	add r10, rax
	jmp .Lmenu_state_7e
.Lmenu_state_5f:
	push rbp
	mov rbp, rsp
	push rcx
	push rdx
	call .Lmenu_state_c7
	mov rcx, qword ptr [rbp - 0x8]
	mov rdx, qword ptr [rbp - 0x10]
	mov rsp, rbp
	pop rbp
	test rax, rax
	je .Lmenu_state_c1
	mov r10, rax
.Lmenu_state_7e:
	mov eax, dword ptr [r10]
	and eax, 0x964
	or eax, dword ptr [r10 + 0x4]
	cmp qword ptr [r10 + 0x10], 0x0
	je .Lmenu_state_c0
	mov r9, qword ptr [r10 + 0x10]
	mov r10d, r9d
	and r10d, 0x3ff
	shl r10d, 0x4
	add r10, r11
	cmp r9, qword ptr [r10]
	jne .Lmenu_state_slow
	movzx eax, al
	or eax, 0x10
	mov r9d, dword ptr [r10 + 0xc]
	shl r9d, 0x8
	or eax, r9d
.Lmenu_state_c0:
	ret
.Lmenu_state_c1:
	mov eax, 0xffffffff
	ret
.Lmenu_state_c7:
	mov rax, rbp
	sub rax, rsp
	cmp rax, 0x1000
	ja .Lmenu_state_145
	mov eax, ecx
	and eax, 0x3ff
	shl eax, 0x4
	add rax, r11
	cmp rcx, qword ptr [rax]
	jne .Lmenu_state_145
	mov r10d, dword ptr [rax + 0x8]
	add r10, r11
	mov eax, dword ptr [rax + 0xc]
	push r10
	push rax
.Lmenu_state_f4:
	test eax, eax
	je .Lmenu_state_115
	cmp qword ptr [r10 + 0x10], 0x0
	jne .Lmenu_state_10d
	cmp edx, dword ptr [r10 + 0x8]
	jne .Lmenu_state_10d
	mov rax, r10
	add rsp, 0x10
	ret
.Lmenu_state_10d:
	add r10, 0x20
	dec eax
	jmp .Lmenu_state_f4
.Lmenu_state_115:
	pop rax
	pop r10
.Lmenu_state_118:
	test eax, eax
	je .Lmenu_state_13d
	mov rcx, qword ptr [r10 + 0x10]
	test rcx, rcx
	je .Lmenu_state_135
	push r10
	push rax
	call .Lmenu_state_c7
	test rax, rax
	jne .Lmenu_state_140
	pop rax
	pop r10
.Lmenu_state_135:
	add r10, 0x20
	dec eax
	jmp .Lmenu_state_118
.Lmenu_state_13d:
	xor eax, eax
	ret
.Lmenu_state_140:
	add rsp, 0x10
	ret
.Lmenu_state_145:
	mov rcx, qword ptr [rbp - 0x8]
	mov rdx, qword ptr [rbp - 0x10]
	mov rsp, rbp
	pop rbp
.Lmenu_state_slow:
	.byte 0xe9
	.long 0
raven_prelude_menu_state_end:

	.global raven_prelude_menu_string, raven_prelude_menu_string_end
raven_prelude_menu_string:
	mov r10, qword ptr [rip + 0]
	test r10, r10
	je .Lmenu_string_slow
	mov r11, qword ptr [r10 + 0x720]
	test r11, r11
	je .Lmenu_string_slow
	test rcx, rcx
	je .Lmenu_string_slow
	test r9d, r9d
	js .Lmenu_string_slow
	test dword ptr [rsp + 0x28], 0x400
	je .Lmenu_string_69
	mov eax, ecx
	and eax, 0x3ff
	shl eax, 0x4
	add rax, r11
	cmp rcx, qword ptr [rax]
	jne .Lmenu_string_slow
	cmp edx, dword ptr [rax + 0xc]
	jae .Lmenu_string_c2
	mov r10d, dword ptr [rax + 0x8]
	add r10, r11
	mov eax, edx
	shl rax, 0x5
	add r10, rax
	jmp .Lmenu_string_88
.Lmenu_string_69:
	push rbp
	mov rbp, rsp
	push rcx
	push rdx
	call .Lmenu_string_c5
	mov rcx, qword ptr [rbp - 0x8]
	mov rdx, qword ptr [rbp - 0x10]
	mov rsp, rbp
	pop rbp
	test rax, rax
	je .Lmenu_string_c2
	mov r10, rax
.Lmenu_string_88:
	mov eax, dword ptr [r10 + 0xc]
	test r9d, r9d
	je .Lmenu_string_c1
	test r8, r8
	je .Lmenu_string_c1
	lea ecx, [r9 - 0x1]
	cmp eax, ecx
	cmova eax, ecx
	mov r10d, dword ptr [r10 + 0x18]
	add r10, r11
	xor ecx, ecx
.Lmenu_string_a8:
	cmp ecx, eax
	je .Lmenu_string_ba
	mov dx, word ptr [r10 + rcx * 2]
	mov word ptr [r8 + rcx * 2], dx
	inc ecx
	jmp .Lmenu_string_a8
.Lmenu_string_ba:
	mov word ptr [r8 + rcx * 2], 0x0
.Lmenu_string_c1:
	ret
.Lmenu_string_c2:
	xor eax, eax
	ret
.Lmenu_string_c5:
	mov rax, rbp
	sub rax, rsp
	cmp rax, 0x1000
	ja .Lmenu_string_143
	mov eax, ecx
	and eax, 0x3ff
	shl eax, 0x4
	add rax, r11
	cmp rcx, qword ptr [rax]
	jne .Lmenu_string_143
	mov r10d, dword ptr [rax + 0x8]
	add r10, r11
	mov eax, dword ptr [rax + 0xc]
	push r10
	push rax
.Lmenu_string_f2:
	test eax, eax
	je .Lmenu_string_113
	cmp qword ptr [r10 + 0x10], 0x0
	jne .Lmenu_string_10b
	cmp edx, dword ptr [r10 + 0x8]
	jne .Lmenu_string_10b
	mov rax, r10
	add rsp, 0x10
	ret
.Lmenu_string_10b:
	add r10, 0x20
	dec eax
	jmp .Lmenu_string_f2
.Lmenu_string_113:
	pop rax
	pop r10
.Lmenu_string_116:
	test eax, eax
	je .Lmenu_string_13b
	mov rcx, qword ptr [r10 + 0x10]
	test rcx, rcx
	je .Lmenu_string_133
	push r10
	push rax
	call .Lmenu_string_c5
	test rax, rax
	jne .Lmenu_string_13e
	pop rax
	pop r10
.Lmenu_string_133:
	add r10, 0x20
	dec eax
	jmp .Lmenu_string_116
.Lmenu_string_13b:
	xor eax, eax
	ret
.Lmenu_string_13e:
	add rsp, 0x10
	ret
.Lmenu_string_143:
	mov rcx, qword ptr [rbp - 0x8]
	mov rdx, qword ptr [rbp - 0x10]
	mov rsp, rbp
	pop rbp
.Lmenu_string_slow:
	.byte 0xe9
	.long 0
raven_prelude_menu_string_end:

	.global raven_prelude_bitmap_object, raven_prelude_bitmap_object_end
raven_prelude_bitmap_object:
	mov r10, qword ptr [rip + 0]
	test r10, r10
	je .Lbitmap_object_slow
	mov r11, qword ptr [r10 + 0x728]
	test r11, r11
	je .Lbitmap_object_slow
	test rcx, rcx
	je .Lbitmap_object_slow
	mov eax, ecx
	and eax, 0x3ff
	imul eax, eax, 0x30
	add r11, rax
	cmp rcx, qword ptr [r11]
	jne .Lbitmap_object_slow
	mov eax, 0x20
	test r8, r8
	je .Lbitmap_object_6c
	cmp edx, 0x20
	jl .Lbitmap_object_6d
	test dword ptr [r11 + 0x8], 0x1
	je .Lbitmap_object_4d
	cmp edx, 0x68
	jge .Lbitmap_object_slow
.Lbitmap_object_4d:
	mov r10, qword ptr [r11 + 0x10]
	mov qword ptr [r8], r10
	mov r10, qword ptr [r11 + 0x18]
	mov qword ptr [r8 + 0x8], r10
	mov r10, qword ptr [r11 + 0x20]
	mov qword ptr [r8 + 0x10], r10
	mov r10, qword ptr [r11 + 0x28]
	mov qword ptr [r8 + 0x18], r10
.Lbitmap_object_6c:
	ret
.Lbitmap_object_6d:
	xor eax, eax
	ret
.Lbitmap_object_slow:
	.byte 0xe9
	.long 0
raven_prelude_bitmap_object_end:

	.global raven_prelude_muldiv, raven_prelude_muldiv_end
raven_prelude_muldiv:
	mov r10, qword ptr [rip + 0]
	test r8d, r8d
	je .Lmuldiv_54
	movsxd rax, ecx
	movsxd r11, edx
	imul rax, r11
	movsxd r11, r8d
	mov r9, r11
	neg r9
	cmovs r9, r11
	mov r10, rax
	neg r10
	cmovs r10, rax
	xor rax, r11
	mov r11, rax
	mov rax, r9
	shr rax, 1
	add rax, r10
	xor edx, edx
	div r9
	test r11, r11
	jns .Lmuldiv_49
	neg rax
.Lmuldiv_49:
	movsxd r10, eax
	cmp r10, rax
	jne .Lmuldiv_54
	mov eax, eax
	ret
.Lmuldiv_54:
	mov eax, 0xffffffff
	ret
	.byte 0xe9
	.long 0
raven_prelude_muldiv_end:

	.global raven_prelude_offset_rect, raven_prelude_offset_rect_end
raven_prelude_offset_rect:
	mov r10, qword ptr [rip + 0]
	test rcx, rcx
	je .Loffset_rect_slow
	add dword ptr [rcx], edx
	add dword ptr [rcx + 0x8], edx
	add dword ptr [rcx + 0x4], r8d
	add dword ptr [rcx + 0xc], r8d
	mov eax, 0x1
	ret
.Loffset_rect_slow:
	.byte 0xe9
	.long 0
raven_prelude_offset_rect_end:

	.global raven_prelude_set_rect, raven_prelude_set_rect_end
raven_prelude_set_rect:
	mov r10, qword ptr [rip + 0]
	test rcx, rcx
	je .Lset_rect_slow
	mov dword ptr [rcx], edx
	mov dword ptr [rcx + 0x4], r8d
	mov dword ptr [rcx + 0x8], r9d
	mov eax, dword ptr [rsp + 0x28]
	mov dword ptr [rcx + 0xc], eax
	mov eax, 0x1
	ret
.Lset_rect_slow:
	.byte 0xe9
	.long 0
raven_prelude_set_rect_end:

	.global raven_prelude_intersect_rect, raven_prelude_intersect_rect_end
raven_prelude_intersect_rect:
	mov r10, qword ptr [rip + 0]
	test rcx, rcx
	je .Lintersect_rect_slow
	test rdx, rdx
	je .Lintersect_rect_slow
	test r8, r8
	je .Lintersect_rect_slow
	mov r9d, dword ptr [rdx]
	cmp r9d, dword ptr [r8]
	cmovl r9d, dword ptr [r8]
	mov r10d, dword ptr [rdx + 0x4]
	cmp r10d, dword ptr [r8 + 0x4]
	cmovl r10d, dword ptr [r8 + 0x4]
	mov r11d, dword ptr [rdx + 0x8]
	cmp r11d, dword ptr [r8 + 0x8]
	cmovg r11d, dword ptr [r8 + 0x8]
	mov eax, dword ptr [rdx + 0xc]
	cmp eax, dword ptr [r8 + 0xc]
	cmovg eax, dword ptr [r8 + 0xc]
	cmp r11d, r9d
	jle .Lintersect_rect_64
	cmp eax, r10d
	jle .Lintersect_rect_64
	mov dword ptr [rcx], r9d
	mov dword ptr [rcx + 0x4], r10d
	mov dword ptr [rcx + 0x8], r11d
	mov dword ptr [rcx + 0xc], eax
	mov eax, 0x1
	ret
.Lintersect_rect_64:
	xor eax, eax
	mov qword ptr [rcx], rax
	mov qword ptr [rcx + 0x8], rax
	ret
.Lintersect_rect_slow:
	.byte 0xe9
	.long 0
raven_prelude_intersect_rect_end:

	.global raven_prelude_window_theme, raven_prelude_window_theme_end
raven_prelude_window_theme:
	mov r10, qword ptr [rip + 0]
	xor eax, eax
	ret
	.byte 0xe9
	.long 0
raven_prelude_window_theme_end:

	.global raven_prelude_file_name, raven_prelude_file_name_end
raven_prelude_file_name:
	mov r10, qword ptr [rip + 0]
	test rcx, rcx
	je .Lfile_name_slow
	mov rax, rcx
	mov r10, rcx
.Lfile_name_12:
	movzx r11d, word ptr [r10]
	test r11d, r11d
	je .Lfile_name_4d
	cmp r11d, 0x5c
	je .Lfile_name_2d
	cmp r11d, 0x2f
	je .Lfile_name_2d
	cmp r11d, 0x3a
	jne .Lfile_name_47
.Lfile_name_2d:
	movzx r9d, word ptr [r10 + 0x2]
	test r9d, r9d
	je .Lfile_name_47
	cmp r9d, 0x5c
	je .Lfile_name_47
	cmp r9d, 0x2f
	je .Lfile_name_47
	lea rax, [r10 + 0x2]
.Lfile_name_47:
	add r10, 0x2
	jmp .Lfile_name_12
.Lfile_name_4d:
	ret
.Lfile_name_slow:
	.byte 0xe9
	.long 0
raven_prelude_file_name_end:

	.global raven_prelude_call_procedure, raven_prelude_call_procedure_end
raven_prelude_call_procedure:
	mov r10, qword ptr [rip + 0]
	test rcx, rcx
	je .Lcall_procedure_slow
	mov rax, rcx
	mov rcx, rdx
	mov rdx, r8
	mov r8, r9
	mov r9, qword ptr [rsp + 0x28]
	jmp rax
.Lcall_procedure_slow:
	.byte 0xe9
	.long 0
raven_prelude_call_procedure_end:

	.global raven_prelude_own_thread_send, raven_prelude_own_thread_send_end
raven_prelude_own_thread_send:
	mov r10, qword ptr [rip + 0]
	test r10, r10
	je .Lown_thread_send_slow
	mov r11, qword ptr [r10 + 0x700]
	test r11, r11
	je .Lown_thread_send_slow
	test rcx, rcx
	je .Lown_thread_send_slow
	mov rax, qword ptr [r10 + 0x7b0]
	test al, 0x1
	jne .Lown_thread_send_slow
	push rsi
	mov esi, ecx
	and esi, 0x1ff
	shl esi, 0x8
	add r11, rsi
	cmp rcx, qword ptr [r11]
	jne .Lown_thread_send_67
	mov esi, dword ptr gs:0x48
	test esi, esi
	je .Lown_thread_send_67
	cmp esi, dword ptr [r11 + 0xf0]
	jne .Lown_thread_send_67
	mov r11, qword ptr [r11 + 0x10]
	test r11, r11
	je .Lown_thread_send_67
	cmp rax, qword ptr [r10 + 0x7b0]
	jne .Lown_thread_send_67
	pop rsi
	jmp r11
.Lown_thread_send_67:
	pop rsi
.Lown_thread_send_slow:
	.byte 0xe9
	.long 0
raven_prelude_own_thread_send_end:

	.global raven_prelude_system_metrics, raven_prelude_system_metrics_end
raven_prelude_system_metrics:
	mov r10, qword ptr [rip + 0]
	test r10, r10
	je .Lsystem_metrics_slow
	mov r11, qword ptr [r10 + 0x730]
	test r11, r11
	je .Lsystem_metrics_slow
	cmp ecx, 0x80
	jae .Lsystem_metrics_slow
	mov eax, ecx
	mov eax, dword ptr [r11 + rax * 4]
	ret
.Lsystem_metrics_slow:
	.byte 0xe9
	.long 0
raven_prelude_system_metrics_end:

	.global raven_prelude_system_colors, raven_prelude_system_colors_end
raven_prelude_system_colors:
	mov r10, qword ptr [rip + 0]
	test r10, r10
	je .Lsystem_colors_slow
	mov r11, qword ptr [r10 + 0x730]
	test r11, r11
	je .Lsystem_colors_slow
	cmp ecx, 0x40
	jae .Lsystem_colors_slow
	mov eax, ecx
	mov eax, dword ptr [r11 + rax * 4 + 0x200]
	ret
.Lsystem_colors_slow:
	.byte 0xe9
	.long 0
raven_prelude_system_colors_end:

	.global raven_prelude_translate_message, raven_prelude_translate_message_end
raven_prelude_translate_message:
	mov r10, qword ptr [rip + 0]
	test rcx, rcx
	je .Ltranslate_message_slow
	mov eax, dword ptr [rcx + 0x8]
	sub eax, 0x100
	cmp eax, 0x9
	jbe .Ltranslate_message_slow
	xor eax, eax
	ret
.Ltranslate_message_slow:
	.byte 0xe9
	.long 0
raven_prelude_translate_message_end:

	.global raven_prelude_message_filter, raven_prelude_message_filter_end
raven_prelude_message_filter:
	mov r10, qword ptr [rip + 0]
	test r10, r10
	je .Lmessage_filter_slow
	mov eax, dword ptr [r10 + 0x738]
	test al, 0x1
	je .Lmessage_filter_slow
	test al, 0x2
	jne .Lmessage_filter_slow
	xor eax, eax
	ret
.Lmessage_filter_slow:
	.byte 0xe9
	.long 0
raven_prelude_message_filter_end:

	.global raven_prelude_desktop_window, raven_prelude_desktop_window_end
raven_prelude_desktop_window:
	mov r10, qword ptr [rip + 0]
	test r10, r10
	je .Ldesktop_window_slow
	mov rax, qword ptr [r10 + 0x740]
	test rax, rax
	je .Ldesktop_window_slow
	ret
.Ldesktop_window_slow:
	.byte 0xe9
	.long 0
raven_prelude_desktop_window_end:

	.global raven_prelude_accelerator, raven_prelude_accelerator_end
raven_prelude_accelerator:
	mov r10, qword ptr [rip + 0]
	test r10, r10
	je .Laccelerator_slow
	test r8, r8
	je .Laccelerator_slow
	mov eax, dword ptr [r8 + 0x8]
	cmp eax, 0x100
	je .Laccelerator_slow
	cmp eax, 0x102
	je .Laccelerator_slow
	cmp eax, 0x104
	je .Laccelerator_slow
	mov r11, qword ptr [r10 + 0x700]
	test r11, r11
	je .Laccelerator_slow
	test rcx, rcx
	je .Laccelerator_slow
	mov eax, ecx
	and eax, 0x1ff
	shl eax, 0x8
	cmp rcx, qword ptr [r11 + rax * 1]
	jne .Laccelerator_slow
	mov r11, qword ptr [r10 + 0x730]
	test r11, r11
	je .Laccelerator_slow
	test rdx, rdx
	je .Laccelerator_slow
	mov eax, dword ptr [r11 + 0x400]
	cmp eax, 0x40
	ja .Laccelerator_slow
.Laccelerator_68:
	test eax, eax
	je .Laccelerator_slow
	dec eax
	cmp rdx, qword ptr [r11 + rax * 8 + 0x408]
	jne .Laccelerator_68
	xor eax, eax
	ret
.Laccelerator_slow:
	.byte 0xe9
	.long 0
raven_prelude_accelerator_end:

	.global raven_prelude_dc_text_color, raven_prelude_dc_text_color_end
raven_prelude_dc_text_color:
	mov r10, qword ptr [rip + 0]
	test r10, r10
	je .Ldc_text_color_slow
	mov r11, qword ptr [r10 + 0x748]
	test r11, r11
	je .Ldc_text_color_slow
	test rcx, rcx
	je .Ldc_text_color_slow
	cmp rcx, qword ptr [r10 + 0x600]
	je .Ldc_text_color_slow
	mov eax, ecx
	and eax, 0x3ff
	shl eax, 0x5
	lea rax, [r11 + rax * 1 + 0x1000]
	cmp rcx, qword ptr [rax]
	jne .Ldc_text_color_slow
	test edx, 0xff000000
	jne .Ldc_text_color_slow
	mov r10, rax
	mov eax, dword ptr [r11]
	cmp eax, 0x40
	jae .Ldc_text_color_slow
	mov qword ptr [r11 + rax * 8 + 0x8], rcx
	inc eax
	mov dword ptr [r11], eax
	mov eax, dword ptr [r10 + 0x8]
	mov dword ptr [r10 + 0x8], edx
	ret
.Ldc_text_color_slow:
	.byte 0xe9
	.long 0
raven_prelude_dc_text_color_end:

	.global raven_prelude_dc_background_color, raven_prelude_dc_background_color_end
raven_prelude_dc_background_color:
	mov r10, qword ptr [rip + 0]
	test r10, r10
	je .Ldc_background_color_slow
	mov r11, qword ptr [r10 + 0x748]
	test r11, r11
	je .Ldc_background_color_slow
	test rcx, rcx
	je .Ldc_background_color_slow
	cmp rcx, qword ptr [r10 + 0x600]
	je .Ldc_background_color_slow
	mov eax, ecx
	and eax, 0x3ff
	shl eax, 0x5
	lea rax, [r11 + rax * 1 + 0x1000]
	cmp rcx, qword ptr [rax]
	jne .Ldc_background_color_slow
	test edx, 0xff000000
	jne .Ldc_background_color_slow
	mov r10, rax
	mov eax, dword ptr [r11]
	cmp eax, 0x40
	jae .Ldc_background_color_slow
	mov qword ptr [r11 + rax * 8 + 0x8], rcx
	inc eax
	mov dword ptr [r11], eax
	mov eax, dword ptr [r10 + 0xc]
	mov dword ptr [r10 + 0xc], edx
	ret
.Ldc_background_color_slow:
	.byte 0xe9
	.long 0
raven_prelude_dc_background_color_end:

	.global raven_prelude_dc_background_mode, raven_prelude_dc_background_mode_end
raven_prelude_dc_background_mode:
	mov r10, qword ptr [rip + 0]
	test r10, r10
	je .Ldc_background_mode_slow
	mov r11, qword ptr [r10 + 0x748]
	test r11, r11
	je .Ldc_background_mode_slow
	test rcx, rcx
	je .Ldc_background_mode_slow
	cmp rcx, qword ptr [r10 + 0x600]
	je .Ldc_background_mode_slow
	mov eax, ecx
	and eax, 0x3ff
	shl eax, 0x5
	lea rax, [r11 + rax * 1 + 0x1000]
	cmp rcx, qword ptr [rax]
	jne .Ldc_background_mode_slow
	cmp edx, 0x1
	je .Ldc_background_mode_47
	cmp edx, 0x2
	jne .Ldc_background_mode_slow
.Ldc_background_mode_47:
	mov r10, rax
	mov eax, dword ptr [r11]
	cmp eax, 0x40
	jae .Ldc_background_mode_slow
	mov qword ptr [r11 + rax * 8 + 0x8], rcx
	inc eax
	mov dword ptr [r11], eax
	mov eax, dword ptr [r10 + 0x10]
	mov dword ptr [r10 + 0x10], edx
	ret
.Ldc_background_mode_slow:
	.byte 0xe9
	.long 0
raven_prelude_dc_background_mode_end:

	.global raven_prelude_dc_text_align, raven_prelude_dc_text_align_end
raven_prelude_dc_text_align:
	mov r10, qword ptr [rip + 0]
	test r10, r10
	je .Ldc_text_align_slow
	mov r11, qword ptr [r10 + 0x748]
	test r11, r11
	je .Ldc_text_align_slow
	test rcx, rcx
	je .Ldc_text_align_slow
	cmp rcx, qword ptr [r10 + 0x600]
	je .Ldc_text_align_slow
	mov eax, ecx
	and eax, 0x3ff
	shl eax, 0x5
	lea rax, [r11 + rax * 1 + 0x1000]
	cmp rcx, qword ptr [rax]
	jne .Ldc_text_align_slow
	mov r10, rax
	mov eax, dword ptr [r11]
	cmp eax, 0x40
	jae .Ldc_text_align_slow
	mov qword ptr [r11 + rax * 8 + 0x8], rcx
	inc eax
	mov dword ptr [r11], eax
	mov eax, dword ptr [r10 + 0x14]
	mov dword ptr [r10 + 0x14], edx
	ret
.Ldc_text_align_slow:
	.byte 0xe9
	.long 0
raven_prelude_dc_text_align_end:

	.global raven_prelude_dc_to_device, raven_prelude_dc_to_device_end
raven_prelude_dc_to_device:
	mov r10, qword ptr [rip + 0]
	test r10, r10
	je .Ldc_to_device_slow
	mov r11, qword ptr [r10 + 0x748]
	test r11, r11
	je .Ldc_to_device_slow
	test rcx, rcx
	je .Ldc_to_device_slow
	cmp rcx, qword ptr [r10 + 0x600]
	je .Ldc_to_device_slow
	mov eax, ecx
	and eax, 0x3ff
	shl eax, 0x5
	lea rax, [r11 + rax * 1 + 0x1000]
	cmp rcx, qword ptr [rax]
	jne .Ldc_to_device_slow
	test rdx, rdx
	je .Ldc_to_device_slow
	test r8d, r8d
	jle .Ldc_to_device_slow
	cmp r8d, 0x40
	ja .Ldc_to_device_slow
	mov r11d, dword ptr [rax + 0x1c]
	cmp r11d, 0x80008000
	je .Ldc_to_device_slow
	movsx r10d, r11w
	sar r11d, 0x10
	mov eax, r8d
.Ldc_to_device_65:
	add dword ptr [rdx], r10d
	add dword ptr [rdx + 0x4], r11d
	add rdx, 0x8
	dec eax
	jne .Ldc_to_device_65
	mov eax, 0x1
	ret
.Ldc_to_device_slow:
	.byte 0xe9
	.long 0
raven_prelude_dc_to_device_end:

	.global raven_prelude_dc_to_logical, raven_prelude_dc_to_logical_end
raven_prelude_dc_to_logical:
	mov r10, qword ptr [rip + 0]
	test r10, r10
	je .Ldc_to_logical_slow
	mov r11, qword ptr [r10 + 0x748]
	test r11, r11
	je .Ldc_to_logical_slow
	test rcx, rcx
	je .Ldc_to_logical_slow
	cmp rcx, qword ptr [r10 + 0x600]
	je .Ldc_to_logical_slow
	mov eax, ecx
	and eax, 0x3ff
	shl eax, 0x5
	lea rax, [r11 + rax * 1 + 0x1000]
	cmp rcx, qword ptr [rax]
	jne .Ldc_to_logical_slow
	test rdx, rdx
	je .Ldc_to_logical_slow
	test r8d, r8d
	jle .Ldc_to_logical_slow
	cmp r8d, 0x40
	ja .Ldc_to_logical_slow
	mov r11d, dword ptr [rax + 0x1c]
	cmp r11d, 0x80008000
	je .Ldc_to_logical_slow
	movsx r10d, r11w
	sar r11d, 0x10
	mov eax, r8d
.Ldc_to_logical_65:
	sub dword ptr [rdx], r10d
	sub dword ptr [rdx + 0x4], r11d
	add rdx, 0x8
	dec eax
	jne .Ldc_to_logical_65
	mov eax, 0x1
	ret
.Ldc_to_logical_slow:
	.byte 0xe9
	.long 0
raven_prelude_dc_to_logical_end:

	.global raven_prelude_dc_get_text_color, raven_prelude_dc_get_text_color_end
raven_prelude_dc_get_text_color:
	mov r10, qword ptr [rip + 0]
	test r10, r10
	je .Ldc_get_text_color_slow
	mov r11, qword ptr [r10 + 0x748]
	test r11, r11
	je .Ldc_get_text_color_slow
	test rcx, rcx
	je .Ldc_get_text_color_slow
	cmp rcx, qword ptr [r10 + 0x600]
	je .Ldc_get_text_color_slow
	mov eax, ecx
	and eax, 0x3ff
	shl eax, 0x5
	lea rax, [r11 + rax * 1 + 0x1000]
	cmp rcx, qword ptr [rax]
	jne .Ldc_get_text_color_slow
	mov eax, dword ptr [rax + 0x8]
	ret
.Ldc_get_text_color_slow:
	.byte 0xe9
	.long 0
raven_prelude_dc_get_text_color_end:

	.global raven_prelude_dc_get_background_color, raven_prelude_dc_get_background_color_end
raven_prelude_dc_get_background_color:
	mov r10, qword ptr [rip + 0]
	test r10, r10
	je .Ldc_get_background_color_slow
	mov r11, qword ptr [r10 + 0x748]
	test r11, r11
	je .Ldc_get_background_color_slow
	test rcx, rcx
	je .Ldc_get_background_color_slow
	cmp rcx, qword ptr [r10 + 0x600]
	je .Ldc_get_background_color_slow
	mov eax, ecx
	and eax, 0x3ff
	shl eax, 0x5
	lea rax, [r11 + rax * 1 + 0x1000]
	cmp rcx, qword ptr [rax]
	jne .Ldc_get_background_color_slow
	mov eax, dword ptr [rax + 0xc]
	ret
.Ldc_get_background_color_slow:
	.byte 0xe9
	.long 0
raven_prelude_dc_get_background_color_end:

	.global raven_prelude_dc_get_background_mode, raven_prelude_dc_get_background_mode_end
raven_prelude_dc_get_background_mode:
	mov r10, qword ptr [rip + 0]
	test r10, r10
	je .Ldc_get_background_mode_slow
	mov r11, qword ptr [r10 + 0x748]
	test r11, r11
	je .Ldc_get_background_mode_slow
	test rcx, rcx
	je .Ldc_get_background_mode_slow
	cmp rcx, qword ptr [r10 + 0x600]
	je .Ldc_get_background_mode_slow
	mov eax, ecx
	and eax, 0x3ff
	shl eax, 0x5
	lea rax, [r11 + rax * 1 + 0x1000]
	cmp rcx, qword ptr [rax]
	jne .Ldc_get_background_mode_slow
	mov eax, dword ptr [rax + 0x10]
	ret
.Ldc_get_background_mode_slow:
	.byte 0xe9
	.long 0
raven_prelude_dc_get_background_mode_end:

	.global raven_prelude_dc_get_text_align, raven_prelude_dc_get_text_align_end
raven_prelude_dc_get_text_align:
	mov r10, qword ptr [rip + 0]
	test r10, r10
	je .Ldc_get_text_align_slow
	mov r11, qword ptr [r10 + 0x748]
	test r11, r11
	je .Ldc_get_text_align_slow
	test rcx, rcx
	je .Ldc_get_text_align_slow
	cmp rcx, qword ptr [r10 + 0x600]
	je .Ldc_get_text_align_slow
	mov eax, ecx
	and eax, 0x3ff
	shl eax, 0x5
	lea rax, [r11 + rax * 1 + 0x1000]
	cmp rcx, qword ptr [rax]
	jne .Ldc_get_text_align_slow
	mov eax, dword ptr [rax + 0x14]
	ret
.Ldc_get_text_align_slow:
	.byte 0xe9
	.long 0
raven_prelude_dc_get_text_align_end:

	.global raven_prelude_destroy_icon, raven_prelude_destroy_icon_end
raven_prelude_destroy_icon:
	mov r10, qword ptr [rip + 0]
	test r10, r10
	je .Ldestroy_icon_slow
	mov r11, qword ptr [r10 + 0x758]
	test r11, r11
	je .Ldestroy_icon_slow
	test rcx, rcx
	je .Ldestroy_icon_slow
	mov eax, ecx
	and eax, 0x3ff
	lea r8, [r11 + rax * 8]
	cmp rcx, qword ptr [r8]
	jne .Ldestroy_icon_slow
	mov r11, qword ptr [r10 + 0x750]
	test r11, r11
	je .Ldestroy_icon_slow
	mov eax, dword ptr [r11]
	cmp eax, 0xff
	jae .Ldestroy_icon_slow
	mov r9d, eax
	shl r9, 0x4
	mov qword ptr [r11 + r9 * 1 + 0x10], rcx
	mov qword ptr [r11 + r9 * 1 + 0x18], 0x1
	inc eax
	mov dword ptr [r11], eax
	mov qword ptr [r8], 0x0
	mov eax, 0x1
	ret
.Ldestroy_icon_slow:
	.byte 0xe9
	.long 0
raven_prelude_destroy_icon_end:

	.global raven_prelude_delete_dc, raven_prelude_delete_dc_end
raven_prelude_delete_dc:
	mov r10, qword ptr [rip + 0]
	test r10, r10
	je .Ldelete_dc_slow
	mov r11, qword ptr [r10 + 0x748]
	test r11, r11
	je .Ldelete_dc_slow
	test rcx, rcx
	je .Ldelete_dc_slow
	mov eax, ecx
	and eax, 0x3ff
	shl eax, 0x5
	lea r8, [r11 + rax * 1 + 0x1000]
	cmp rcx, qword ptr [r8]
	jne .Ldelete_dc_slow
	test dword ptr [r8 + 0x18], 0x1
	jne .Ldelete_dc_slow
	mov r11, qword ptr [r10 + 0x750]
	test r11, r11
	je .Ldelete_dc_slow
	mov eax, dword ptr [r11]
	cmp eax, 0xff
	jae .Ldelete_dc_slow
	mov r9d, eax
	shl r9, 0x4
	mov qword ptr [r11 + r9 * 1 + 0x10], rcx
	mov qword ptr [r11 + r9 * 1 + 0x18], 0x2
	inc eax
	mov dword ptr [r11], eax
	mov qword ptr [r8], 0x0
	mov eax, 0x1
	ret
.Ldelete_dc_slow:
	.byte 0xe9
	.long 0
raven_prelude_delete_dc_end:

	.global raven_prelude_delete_object, raven_prelude_delete_object_end
raven_prelude_delete_object:
	mov r10, qword ptr [rip + 0]
	test r10, r10
	je .Ldelete_object_slow
	mov r11, qword ptr [r10 + 0x728]
	test r11, r11
	je .Ldelete_object_slow
	test rcx, rcx
	je .Ldelete_object_slow
	mov eax, ecx
	and eax, 0x3ff
	imul eax, eax, 0x30
	lea r8, [r11 + rax * 1]
	cmp rcx, qword ptr [r8]
	jne .Ldelete_object_slow
	test dword ptr [r8 + 0x8], 0x2
	je .Ldelete_object_slow
	mov r11, qword ptr [r10 + 0x750]
	test r11, r11
	je .Ldelete_object_slow
	mov eax, dword ptr [r11]
	cmp eax, 0xff
	jae .Ldelete_object_slow
	mov r9d, eax
	shl r9, 0x4
	mov qword ptr [r11 + r9 * 1 + 0x10], rcx
	mov qword ptr [r11 + r9 * 1 + 0x18], 0x3
	inc eax
	mov dword ptr [r11], eax
	mov qword ptr [r8], 0x0
	mov eax, 0x1
	ret
.Ldelete_object_slow:
	.byte 0xe9
	.long 0
raven_prelude_delete_object_end:

/* lea r11,[rip+block]; test edx,-4; jnz slow; mov rax,[rcx]; test rax,rax; jz claim; test al,1; jnz slow; test al,2; jz slow mov dword
 * [r8],0; test r9,r9; jz one; and rax,-4; mov [r9],rax; one: mov eax,1; ret claim: test dl,1; jnz slow; mov r10d,1; lock cmpxchg
 * [rcx],r10; jnz slow; mov dword [r8],1; jmp one slow: jmp trap */
	.global raven_prelude_init_once_begin, raven_prelude_init_once_begin_end
raven_prelude_init_once_begin:
	lea r11, [rip + 0]
	test edx, 0xfffffffc
	jne .Linit_once_begin_slow
	mov rax, qword ptr [rcx]
	test rax, rax
	je .Linit_once_begin_38
	test al, 0x1
	jne .Linit_once_begin_slow
	test al, 0x2
	je .Linit_once_begin_slow
	mov dword ptr [r8], 0x0
	test r9, r9
	je .Linit_once_begin_32
	and rax, 0xfffffffffffffffc
	mov qword ptr [r9], rax
.Linit_once_begin_32:
	mov eax, 0x1
	ret
.Linit_once_begin_38:
	test dl, 0x1
	jne .Linit_once_begin_slow
	mov r10d, 0x1
	lock cmpxchg qword ptr [rcx], r10
	jne .Linit_once_begin_slow
	mov dword ptr [r8], 0x1
	jmp .Linit_once_begin_32
.Linit_once_begin_slow:
	.byte 0xe9
	.long 0
raven_prelude_init_once_begin_end:

/* lea r11,[rip+block]; test rcx,rcx; jz slow; mov eax,[rcx]; cmp eax,[rcx+8]; jge empty; mov eax,[rcx+4]; cmp eax,[rcx+12]; jge empty;
 * xor eax,eax; ret; empty: mov eax,1; ret; slow: jmp trap */
	.global raven_prelude_is_rect_empty, raven_prelude_is_rect_empty_end
raven_prelude_is_rect_empty:
	lea r11, [rip + 0]
	test rcx, rcx
	je .Lis_rect_empty_slow
	mov eax, dword ptr [rcx]
	cmp eax, dword ptr [rcx + 0x8]
	jge .Lis_rect_empty_1e
	mov eax, dword ptr [rcx + 0x4]
	cmp eax, dword ptr [rcx + 0xc]
	jge .Lis_rect_empty_1e
	xor eax, eax
	ret
.Lis_rect_empty_1e:
	mov eax, 0x1
	ret
.Lis_rect_empty_slow:
	.byte 0xe9
	.long 0
raven_prelude_is_rect_empty_end:

/* lea r11,[rip+block]; test rcx,rcx; jz slow; xor eax,eax; mov [rcx],rax; mov [rcx+8],rax; mov eax,1; ret; slow: jmp trap */
	.global raven_prelude_set_rect_empty, raven_prelude_set_rect_empty_end
raven_prelude_set_rect_empty:
	lea r11, [rip + 0]
	test rcx, rcx
	je .Lset_rect_empty_slow
	xor eax, eax
	mov qword ptr [rcx], rax
	mov qword ptr [rcx + 0x8], rax
	mov eax, 0x1
	ret
.Lset_rect_empty_slow:
	.byte 0xe9
	.long 0
raven_prelude_set_rect_empty_end:

/* lea r11,[rip+block]; test rcx,rcx; jz slow; test rdx,rdx; jz slow; mov rax,[rdx]; mov r10,[rdx+8]; mov [rcx],rax; mov [rcx+8],r10;
 * mov eax,1; ret; slow: jmp trap */
	.global raven_prelude_copy_rect, raven_prelude_copy_rect_end
raven_prelude_copy_rect:
	lea r11, [rip + 0]
	test rcx, rcx
	je .Lcopy_rect_slow
	test rdx, rdx
	je .Lcopy_rect_slow
	mov rax, qword ptr [rdx]
	mov r10, qword ptr [rdx + 0x8]
	mov qword ptr [rcx], rax
	mov qword ptr [rcx + 0x8], r10
	mov eax, 0x1
	ret
.Lcopy_rect_slow:
	.byte 0xe9
	.long 0
raven_prelude_copy_rect_end:

/* lea r11,[rip+block]; test rcx,rcx; jz slow; sub [rcx],edx; add [rcx+8],edx; sub [rcx+4],r8d; add [rcx+12],r8d; mov eax,1; ret; slow:
 * jmp trap */
	.global raven_prelude_inflate_rect, raven_prelude_inflate_rect_end
raven_prelude_inflate_rect:
	lea r11, [rip + 0]
	test rcx, rcx
	je .Linflate_rect_slow
	sub dword ptr [rcx], edx
	add dword ptr [rcx + 0x8], edx
	sub dword ptr [rcx + 0x4], r8d
	add dword ptr [rcx + 0xc], r8d
	mov eax, 0x1
	ret
.Linflate_rect_slow:
	.byte 0xe9
	.long 0
raven_prelude_inflate_rect_end:

/* lea r11,[rip+block]; the three pointers tested, slow when one is missing; a empty: b, or zeros when b is empty too; b empty: a; else
 * the lesser left and top and the greater right and bottom in eax, r9d, r10d, r11d, stored; mov eax,1; ret; slow: jmp trap */
	.global raven_prelude_union_rect, raven_prelude_union_rect_end
raven_prelude_union_rect:
	lea r11, [rip + 0]
	test rcx, rcx
	je .Lunion_rect_slow
	test rdx, rdx
	je .Lunion_rect_slow
	test r8, r8
	je .Lunion_rect_slow
	mov eax, dword ptr [rdx]
	cmp eax, dword ptr [rdx + 0x8]
	jge .Lunion_rect_88
	mov eax, dword ptr [rdx + 0x4]
	cmp eax, dword ptr [rdx + 0xc]
	jge .Lunion_rect_88
	mov eax, dword ptr [r8]
	cmp eax, dword ptr [r8 + 0x8]
	jge .Lunion_rect_af
	mov eax, dword ptr [r8 + 0x4]
	cmp eax, dword ptr [r8 + 0xc]
	jge .Lunion_rect_af
	mov eax, dword ptr [rdx]
	cmp eax, dword ptr [r8]
	cmovg eax, dword ptr [r8]
	mov r9d, dword ptr [rdx + 0x4]
	cmp r9d, dword ptr [r8 + 0x4]
	cmovg r9d, dword ptr [r8 + 0x4]
	mov r10d, dword ptr [rdx + 0x8]
	cmp r10d, dword ptr [r8 + 0x8]
	cmovl r10d, dword ptr [r8 + 0x8]
	mov r11d, dword ptr [rdx + 0xc]
	cmp r11d, dword ptr [r8 + 0xc]
	cmovl r11d, dword ptr [r8 + 0xc]
	mov dword ptr [rcx], eax
	mov dword ptr [rcx + 0x4], r9d
	mov dword ptr [rcx + 0x8], r10d
	mov dword ptr [rcx + 0xc], r11d
	mov eax, 0x1
	ret
.Lunion_rect_88:
	mov eax, dword ptr [r8]
	cmp eax, dword ptr [r8 + 0x8]
	jge .Lunion_rect_c3
	mov eax, dword ptr [r8 + 0x4]
	cmp eax, dword ptr [r8 + 0xc]
	jge .Lunion_rect_c3
	mov rax, qword ptr [r8]
	mov r9, qword ptr [r8 + 0x8]
	mov qword ptr [rcx], rax
	mov qword ptr [rcx + 0x8], r9
	mov eax, 0x1
	ret
.Lunion_rect_af:
	mov rax, qword ptr [rdx]
	mov r9, qword ptr [rdx + 0x8]
	mov qword ptr [rcx], rax
	mov qword ptr [rcx + 0x8], r9
	mov eax, 0x1
	ret
.Lunion_rect_c3:
	xor eax, eax
	mov qword ptr [rcx], rax
	mov qword ptr [rcx + 0x8], rax
	ret
.Lunion_rect_slow:
	.byte 0xe9
	.long 0
raven_prelude_union_rect_end:

/* lea r11,[rip+block]; test rcx,rcx; jz slow; test rdx,rdx; jz slow; mov rax,[rcx]; mov r10,[rcx+8]; xor rax,[rdx]; xor r10,[rdx+8]; or
 * rax,r10; sete al; movzx eax,al; ret; slow: jmp trap */
	.global raven_prelude_equal_rect, raven_prelude_equal_rect_end
raven_prelude_equal_rect:
	lea r11, [rip + 0]
	test rcx, rcx
	je .Lequal_rect_slow
	test rdx, rdx
	je .Lequal_rect_slow
	mov rax, qword ptr [rcx]
	mov r10, qword ptr [rcx + 0x8]
	xor rax, qword ptr [rdx]
	xor r10, qword ptr [rdx + 0x8]
	or rax, r10
	sete al
	movzx eax, al
	ret
.Lequal_rect_slow:
	.byte 0xe9
	.long 0
raven_prelude_equal_rect_end:

/* mov r10,[pointer]; test r10,r10; jz slow; mov r11,[r10+multiplier]; test r11,r11; jz slow; mov r8,[r10+end]; test r8,r8; jz slow; mov
 * r9,rcx; the counter as in the clock prelude; mov rcx,[r10+start]; cmp rax,rcx; jb restore; cmp rax,r8; jae restore; sub rax,rcx; xor
 * edx,edx; mov ecx,10000; div rcx; copy the 16 bytes of the time to [r9]; mov [r9+14],ax; cmp r8,[r10+end]; jne restore; ret; restore:
 * mov rcx,r9; slow: jmp breakpoint */
	.global raven_prelude_local_time, raven_prelude_local_time_end
raven_prelude_local_time:
	mov r10, qword ptr [rip + 0]
	test r10, r10
	je .Llocal_time_slow
	mov r11, qword ptr [r10 + 0x800]
	test r11, r11
	je .Llocal_time_slow
	mov r8, qword ptr [r10 + 0x848]
	test r8, r8
	je .Llocal_time_slow
	mov r9, rcx
	rdtsc
	shl rdx, 0x20
	or rax, rdx
	add r11, r10
	sub rax, qword ptr [r11]
	mul qword ptr [r11 + 0x10]
	shrd rax, rdx, 0x20
	add rax, qword ptr [r11 + 0x8]
	mov rcx, qword ptr [r10 + 0x840]
	cmp rax, rcx
	jb .Llocal_time_88
	cmp rax, r8
	jae .Llocal_time_88
	sub rax, rcx
	xor edx, edx
	mov ecx, 0x2710
	div rcx
	mov rdx, qword ptr [r10 + 0x850]
	mov qword ptr [r9], rdx
	mov rdx, qword ptr [r10 + 0x858]
	mov qword ptr [r9 + 0x8], rdx
	mov word ptr [r9 + 0xe], ax
	cmp r8, qword ptr [r10 + 0x848]
	jne .Llocal_time_88
	ret
.Llocal_time_88:
	mov rcx, r9
.Llocal_time_slow:
	.byte 0xe9
	.long 0
raven_prelude_local_time_end:

	.global raven_prelude_monitor, raven_prelude_monitor_end
raven_prelude_monitor:
	lea r11, [rip + 0]
	mov eax, 0x1
	ret
	.byte 0xe9
	.long 0
raven_prelude_monitor_end:

	.global raven_prelude_window_dpi_context, raven_prelude_window_dpi_context_end
raven_prelude_window_dpi_context:
	lea r11, [rip + 0]
	mov rax, 0xfffffffffffffffe
	ret
	.byte 0xe9
	.long 0
raven_prelude_window_dpi_context_end:

/* mov r10,[rip+block]; test r10,r10; jz slow; mov r11,[r10+0x700]; test r11,r11; jz slow; mov r9d,64 next: test rcx,rcx; jz slow; mov
 * eax,ecx; and eax,0x1ff; shl eax,8; lea r8,[r11+rax]; cmp rcx,[r8]; jne slow test dword [r8+0x30],WS_VISIBLE; jz no; mov
 * rcx,[r8+0x58]; test rcx,rcx; jz yes; dec r9d; jnz next; jmp slow yes: mov eax,1; ret; no: xor eax,eax; ret; slow: jmp trap */
	.global raven_prelude_window_visible, raven_prelude_window_visible_end
raven_prelude_window_visible:
	mov r10, qword ptr [rip + 0]
	test r10, r10
	je .Lwindow_visible_slow
	mov r11, qword ptr [r10 + 0x700]
	test r11, r11
	je .Lwindow_visible_slow
	mov r9d, 0x40
.Lwindow_visible_1e:
	test rcx, rcx
	je .Lwindow_visible_slow
	mov eax, ecx
	and eax, 0x1ff
	shl eax, 0x8
	lea r8, [r11 + rax * 1]
	cmp rcx, qword ptr [r8]
	jne .Lwindow_visible_slow
	test dword ptr [r8 + 0x30], 0x10000000
	je .Lwindow_visible_56
	mov rcx, qword ptr [r8 + 0x58]
	test rcx, rcx
	je .Lwindow_visible_50
	dec r9d
	jne .Lwindow_visible_1e
	jmp .Lwindow_visible_slow
.Lwindow_visible_50:
	mov eax, 0x1
	ret
.Lwindow_visible_56:
	xor eax, eax
	ret
.Lwindow_visible_slow:
	.byte 0xe9
	.long 0
raven_prelude_window_visible_end:

/* mov r10,[rip+block]; test r10,r10; jz slow; mov r11,[r10+0x700]; test r11,r11; jz slow; test rcx,rcx; jz slow mov eax,ecx; and
 * eax,0x1ff; shl eax,8; add r11,rax; cmp rcx,[r11]; jne slow; mov eax,[r11+0x30]; shr eax,29; and eax,1; ret slow: jmp trap */
	.global raven_prelude_window_iconic, raven_prelude_window_iconic_end
raven_prelude_window_iconic:
	mov r10, qword ptr [rip + 0]
	test r10, r10
	je .Lwindow_iconic_slow
	mov r11, qword ptr [r10 + 0x700]
	test r11, r11
	je .Lwindow_iconic_slow
	test rcx, rcx
	je .Lwindow_iconic_slow
	mov eax, ecx
	and eax, 0x1ff
	shl eax, 0x8
	add r11, rax
	cmp rcx, qword ptr [r11]
	jne .Lwindow_iconic_slow
	mov eax, dword ptr [r11 + 0x30]
	shr eax, 0x1d
	and eax, 0x1
	ret
.Lwindow_iconic_slow:
	.byte 0xe9
	.long 0
raven_prelude_window_iconic_end:

	.global raven_prelude_dpi_set, raven_prelude_dpi_set_end
raven_prelude_dpi_set:
	mov r10, qword ptr [rip + 0]
	test r10, r10
	je .Ldpi_set_slow
	lea rax, [rcx + 0x5]
	cmp rax, 0x4
	ja .Ldpi_set_slow
	mov rax, rcx
	xchg qword ptr [r10 + 0x760], rax
	test rax, rax
	jne .Ldpi_set_2c
	mov rax, 0xffffffffffffffff
.Ldpi_set_2c:
	ret
.Ldpi_set_slow:
	.byte 0xe9
	.long 0
raven_prelude_dpi_set_end:

	.global raven_prelude_dpi_get, raven_prelude_dpi_get_end
raven_prelude_dpi_get:
	mov r10, qword ptr [rip + 0]
	test r10, r10
	je .Ldpi_get_slow
	mov rax, qword ptr [r10 + 0x760]
	test rax, rax
	jne .Ldpi_get_1f
	mov rax, 0xffffffffffffffff
.Ldpi_get_1f:
	ret
.Ldpi_get_slow:
	.byte 0xe9
	.long 0
raven_prelude_dpi_get_end:

	.global raven_prelude_keyboard_layout, raven_prelude_keyboard_layout_end
raven_prelude_keyboard_layout:
	lea r11, [rip + 0]
	mov eax, 0x4090409
	ret
	.byte 0xe9
	.long 0
raven_prelude_keyboard_layout_end:

	.global raven_prelude_win_event_hook, raven_prelude_win_event_hook_end
raven_prelude_win_event_hook:
	lea r11, [rip + 0]
	xor eax, eax
	ret
	.byte 0xe9
	.long 0
raven_prelude_win_event_hook_end:

	.global raven_prelude_slist_depth, raven_prelude_slist_depth_end
raven_prelude_slist_depth:
	lea r11, [rip + 0]
	test rcx, rcx
	je .Lslist_depth_slow
	movzx eax, word ptr [rcx + 0x8]
	ret
.Lslist_depth_slow:
	.byte 0xe9
	.long 0
raven_prelude_slist_depth_end:

	.global raven_prelude_point_in_rect, raven_prelude_point_in_rect_end
raven_prelude_point_in_rect:
	lea r11, [rip + 0]
	test rcx, rcx
	je .Lpoint_in_rect_slow
	mov r8, rdx
	shr r8, 0x20
	xor eax, eax
	cmp edx, dword ptr [rcx]
	jl .Lpoint_in_rect_2f
	cmp edx, dword ptr [rcx + 0x8]
	jge .Lpoint_in_rect_2f
	cmp r8d, dword ptr [rcx + 0x4]
	jl .Lpoint_in_rect_2f
	cmp r8d, dword ptr [rcx + 0xc]
	jge .Lpoint_in_rect_2f
	mov eax, 0x1
.Lpoint_in_rect_2f:
	ret
.Lpoint_in_rect_slow:
	.byte 0xe9
	.long 0
raven_prelude_point_in_rect_end:

	.global raven_prelude_guid_text, raven_prelude_guid_text_end
raven_prelude_guid_text:
	lea r11, [rip + 0]
	test rcx, rcx
	je .Lguid_text_slow
	test rdx, rdx
	je .Lguid_text_slow
	cmp r8d, 0x27
	jb .Lguid_text_a3
	push rbx
	lea r9, [rip + .Lguid_text_table]
	mov word ptr [rdx], 0x7b
	mov word ptr [rdx + 0x4a], 0x7d
	mov word ptr [rdx + 0x4c], 0x0
	mov word ptr [rdx + 0x12], 0x2d
	mov word ptr [rdx + 0x1c], 0x2d
	mov word ptr [rdx + 0x26], 0x2d
	mov word ptr [rdx + 0x30], 0x2d
	xor r10d, r10d
.Lguid_text_57:
	movzx eax, byte ptr [r9 + r10 * 2]
	movzx ebx, byte ptr [r9 + r10 * 2 + 0x1]
	movzx eax, byte ptr [rcx + rax * 1]
	mov r11d, eax
	shr eax, 0x4
	and r11d, 0xf
	cmp eax, 0xa
	jb .Lguid_text_78
	add eax, 0x7
.Lguid_text_78:
	add eax, 0x30
	cmp r11d, 0xa
	jb .Lguid_text_85
	add r11d, 0x7
.Lguid_text_85:
	add r11d, 0x30
	mov word ptr [rdx + rbx * 2], ax
	mov word ptr [rdx + rbx * 2 + 0x2], r11w
	inc r10d
	cmp r10d, 0x10
	jb .Lguid_text_57
	pop rbx
	mov eax, 0x27
	ret
.Lguid_text_a3:
	xor eax, eax
	ret
.Lguid_text_table:
	.byte 0x03, 0x01, 0x02, 0x03, 0x01, 0x05, 0x00, 0x07, 0x05, 0x0a, 0x04, 0x0c
	.byte 0x07, 0x0f, 0x06, 0x11, 0x08, 0x14, 0x09, 0x16, 0x0a, 0x19, 0x0b, 0x1b
	.byte 0x0c, 0x1d, 0x0d, 0x1f, 0x0e, 0x21, 0x0f, 0x23
.Lguid_text_slow:
	.byte 0xe9
	.long 0
raven_prelude_guid_text_end:

	.global raven_prelude_rect_in_region, raven_prelude_rect_in_region_end
raven_prelude_rect_in_region:
	mov r10, qword ptr [rip + 0]
	test r10, r10
	je .Lrect_in_region_slow
	mov r11, qword ptr [r10 + 0x768]
	test r11, r11
	je .Lrect_in_region_slow
	test rdx, rdx
	je .Lrect_in_region_slow
	movzx eax, cl
	shl eax, 0x8
	add r11, rax
	cmp rcx, qword ptr [r11]
	jne .Lrect_in_region_slow
	mov r9d, dword ptr [r11 + 0x8]
	cmp r9d, 0xe
	ja .Lrect_in_region_slow
	mov eax, dword ptr [rdx]
	add eax, 0x4000000
	cmp eax, 0x8000000
	jae .Lrect_in_region_slow
	mov eax, dword ptr [rdx + 0x4]
	add eax, 0x4000000
	cmp eax, 0x8000000
	jae .Lrect_in_region_slow
	mov eax, dword ptr [rdx + 0x8]
	add eax, 0x4000000
	cmp eax, 0x8000000
	jae .Lrect_in_region_slow
	mov eax, dword ptr [rdx + 0xc]
	add eax, 0x4000000
	cmp eax, 0x8000000
	jae .Lrect_in_region_slow
	mov eax, dword ptr [rdx]
	mov r8d, dword ptr [rdx + 0x8]
	mov ecx, dword ptr [rdx + 0x4]
	mov edx, dword ptr [rdx + 0xc]
	cmp eax, r8d
	jle .Lrect_in_region_9b
	xchg r8d, eax
.Lrect_in_region_9b:
	cmp ecx, edx
	jle .Lrect_in_region_a1
	xchg ecx, edx
.Lrect_in_region_a1:
	cmp eax, r8d
	je .Lrect_in_region_d9
	cmp ecx, edx
	je .Lrect_in_region_d9
	lea r10, [r11 + 0x20]
.Lrect_in_region_ae:
	test r9d, r9d
	je .Lrect_in_region_d9
	cmp eax, dword ptr [r10 + 0x8]
	jge .Lrect_in_region_d0
	cmp r8d, dword ptr [r10]
	jle .Lrect_in_region_d0
	cmp ecx, dword ptr [r10 + 0xc]
	jge .Lrect_in_region_d0
	cmp edx, dword ptr [r10 + 0x4]
	jle .Lrect_in_region_d0
	mov eax, 0x1
	ret
.Lrect_in_region_d0:
	add r10, 0x10
	dec r9d
	jmp .Lrect_in_region_ae
.Lrect_in_region_d9:
	xor eax, eax
	ret
.Lrect_in_region_slow:
	.byte 0xe9
	.long 0
raven_prelude_rect_in_region_end:

	.global raven_prelude_point_in_region, raven_prelude_point_in_region_end
raven_prelude_point_in_region:
	mov r10, qword ptr [rip + 0]
	test r10, r10
	je .Lpoint_in_region_slow
	mov r11, qword ptr [r10 + 0x768]
	test r11, r11
	je .Lpoint_in_region_slow
	movzx eax, cl
	shl eax, 0x8
	add r11, rax
	cmp rcx, qword ptr [r11]
	jne .Lpoint_in_region_slow
	mov r9d, dword ptr [r11 + 0x8]
	cmp r9d, 0xe
	ja .Lpoint_in_region_slow
	lea r10, [r11 + 0x20]
.Lpoint_in_region_34:
	test r9d, r9d
	je .Lpoint_in_region_5f
	cmp edx, dword ptr [r10]
	jl .Lpoint_in_region_56
	cmp edx, dword ptr [r10 + 0x8]
	jge .Lpoint_in_region_56
	cmp r8d, dword ptr [r10 + 0x4]
	jl .Lpoint_in_region_56
	cmp r8d, dword ptr [r10 + 0xc]
	jge .Lpoint_in_region_56
	mov eax, 0x1
	ret
.Lpoint_in_region_56:
	add r10, 0x10
	dec r9d
	jmp .Lpoint_in_region_34
.Lpoint_in_region_5f:
	xor eax, eax
	ret
.Lpoint_in_region_slow:
	.byte 0xe9
	.long 0
raven_prelude_point_in_region_end:

	.global raven_prelude_region_box, raven_prelude_region_box_end
raven_prelude_region_box:
	mov r10, qword ptr [rip + 0]
	test r10, r10
	je .Lregion_box_slow
	mov r11, qword ptr [r10 + 0x768]
	test r11, r11
	je .Lregion_box_slow
	test rdx, rdx
	je .Lregion_box_slow
	movzx eax, cl
	shl eax, 0x8
	add r11, rax
	cmp rcx, qword ptr [r11]
	jne .Lregion_box_slow
	mov rax, qword ptr [r11 + 0x10]
	mov qword ptr [rdx], rax
	mov rax, qword ptr [r11 + 0x18]
	mov qword ptr [rdx + 0x8], rax
	mov eax, dword ptr [r11 + 0xc]
	ret
.Lregion_box_slow:
	.byte 0xe9
	.long 0
raven_prelude_region_box_end:

	.global raven_prelude_quiet_peek, raven_prelude_quiet_peek_end
raven_prelude_quiet_peek:
	mov r10, qword ptr [rip + 0]
	test r10, r10
	je .Lquiet_peek_slow
	mov r10, qword ptr [r10 + 0x770]
	test r10, r10
	je .Lquiet_peek_slow
	mov eax, dword ptr gs:0x48
	mov r11d, eax
	and r11d, 0x3f
	shl r11d, 0x4
	add r11, r10
	cmp eax, dword ptr [r11]
	jne .Lquiet_peek_slow
	cmp dword ptr [r11 + 0x4], 0x0
	jne .Lquiet_peek_slow
	mov r10, rdx
	rdtsc
	shl rdx, 0x20
	or rax, rdx
	mov rdx, r10
	cmp rax, qword ptr [r11 + 0x8]
	jae .Lquiet_peek_slow
	xor eax, eax
	ret
.Lquiet_peek_slow:
	.byte 0xe9
	.long 0
raven_prelude_quiet_peek_end:

	.global raven_prelude_quiet_input, raven_prelude_quiet_input_end
raven_prelude_quiet_input:
	mov r10, qword ptr [rip + 0]
	test r10, r10
	je .Lquiet_input_slow
	mov r10, qword ptr [r10 + 0x770]
	test r10, r10
	je .Lquiet_input_slow
	mov eax, dword ptr gs:0x48
	mov r11d, eax
	and r11d, 0x3f
	shl r11d, 0x4
	add r11, r10
	cmp eax, dword ptr [r11]
	jne .Lquiet_input_slow
	test dword ptr [r11 + 0x4], 0x5
	jne .Lquiet_input_slow
	mov r10, rdx
	rdtsc
	shl rdx, 0x20
	or rax, rdx
	mov rdx, r10
	cmp rax, qword ptr [r11 + 0x8]
	jae .Lquiet_input_slow
	xor eax, eax
	ret
.Lquiet_input_slow:
	.byte 0xe9
	.long 0
raven_prelude_quiet_input_end:

	.global raven_prelude_quiet_status, raven_prelude_quiet_status_end
raven_prelude_quiet_status:
	mov r10, qword ptr [rip + 0]
	test r10, r10
	je .Lquiet_status_slow
	mov r10, qword ptr [r10 + 0x770]
	test r10, r10
	je .Lquiet_status_slow
	mov eax, dword ptr gs:0x48
	mov r11d, eax
	and r11d, 0x3f
	shl r11d, 0x4
	add r11, r10
	cmp eax, dword ptr [r11]
	jne .Lquiet_status_slow
	test ecx, 0xffffe200
	jne .Lquiet_status_slow
	test dword ptr [r11 + 0x4], ecx
	jne .Lquiet_status_slow
	mov r10, rdx
	rdtsc
	shl rdx, 0x20
	or rax, rdx
	mov rdx, r10
	cmp rax, qword ptr [r11 + 0x8]
	jae .Lquiet_status_slow
	xor eax, eax
	ret
.Lquiet_status_slow:
	.byte 0xe9
	.long 0
raven_prelude_quiet_status_end:

	.global raven_prelude_key_state, raven_prelude_key_state_end
raven_prelude_key_state:
	mov r10, qword ptr [rip + 0]
	test r10, r10
	je .Lkey_state_slow
	mov r11, qword ptr [r10 + 0x788]
	test r11, r11
	je .Lkey_state_slow
	movzx eax, cl
	movzx eax, byte ptr [r11 + rax * 1]
	mov edx, eax
	and edx, 0x1
	test al, 0x80
	je .Lkey_state_2f
	or edx, 0xff80
.Lkey_state_2f:
	mov eax, edx
	ret
.Lkey_state_slow:
	.byte 0xe9
	.long 0
raven_prelude_key_state_end:

	.global raven_prelude_context_activate, raven_prelude_context_activate_end
raven_prelude_context_activate:
	mov r10, qword ptr [rip + 0]
	test r10, r10
	je .Lcontext_activate_slow
	mov r10, qword ptr [r10 + 0x780]
	test r10, r10
	je .Lcontext_activate_slow
	mov eax, dword ptr gs:0x48
	mov r11d, eax
	shr r11d, 0x2
	and r11d, 0x3f
	shl r11d, 0x4
	add r11, r10
	cmp eax, dword ptr [r11]
	jne .Lcontext_activate_slow
	mov r10, qword ptr [r11 + 0x8]
	cmp eax, dword ptr [r11]
	jne .Lcontext_activate_slow
	cmp rcx, r10
	jne .Lcontext_activate_slow
	cmp rcx, 0xffffffffffffffff
	je .Lcontext_activate_slow
	test rdx, rdx
	je .Lcontext_activate_5d
	movabs rax, 0x5276000000000000
	mov qword ptr [rdx], rax
.Lcontext_activate_5d:
	mov eax, 0x1
	ret
.Lcontext_activate_slow:
	.byte 0xe9
	.long 0
raven_prelude_context_activate_end:

	.global raven_prelude_context_deactivate, raven_prelude_context_deactivate_end
raven_prelude_context_deactivate:
	mov r10, qword ptr [rip + 0]
	mov rax, rdx
	shr rax, 0x30
	cmp eax, 0x5276
	jne .Lcontext_deactivate_slow
	mov eax, 0x1
	ret
.Lcontext_deactivate_slow:
	.byte 0xe9
	.long 0
raven_prelude_context_deactivate_end:

	.global raven_prelude_window_parent, raven_prelude_window_parent_end
raven_prelude_window_parent:
	mov r10, qword ptr [rip + 0]
	test r10, r10
	je .Lwindow_parent_slow
	mov r11, qword ptr [r10 + 0x700]
	test r11, r11
	je .Lwindow_parent_slow
	test rcx, rcx
	je .Lwindow_parent_slow
	mov eax, ecx
	and eax, 0x1ff
	shl eax, 0x8
	add rax, r11
	cmp qword ptr [rax], rcx
	jne .Lwindow_parent_slow
	mov rdx, qword ptr [rax + 0x58]
	test rdx, rdx
	jne .Lwindow_parent_5c
	test dword ptr [rax + 0x30], 0x80000000
	je .Lwindow_parent_60
	mov rdx, qword ptr [rax + 0x20]
	test rdx, rdx
	je .Lwindow_parent_60
	mov eax, edx
	and eax, 0x1ff
	shl eax, 0x8
	add rax, r11
	cmp qword ptr [rax], rdx
	jne .Lwindow_parent_slow
.Lwindow_parent_5c:
	mov rax, rdx
	ret
.Lwindow_parent_60:
	xor eax, eax
	ret
.Lwindow_parent_slow:
	.byte 0xe9
	.long 0
raven_prelude_window_parent_end:

	.global raven_prelude_window_ancestor, raven_prelude_window_ancestor_end
raven_prelude_window_ancestor:
	mov r10, qword ptr [rip + 0]
	test r10, r10
	je .Lwindow_ancestor_slow
	mov r11, qword ptr [r10 + 0x700]
	test r11, r11
	je .Lwindow_ancestor_slow
	test rcx, rcx
	je .Lwindow_ancestor_slow
	lea eax, [rdx - 0x1]
	cmp eax, 0x2
	ja .Lwindow_ancestor_slow
	mov r8, rcx
	mov r9d, 0x40
.Lwindow_ancestor_2e:
	mov eax, r8d
	and eax, 0x1ff
	shl eax, 0x8
	add rax, r11
	cmp qword ptr [rax], r8
	jne .Lwindow_ancestor_slow
	mov r10, qword ptr [rax + 0x58]
	cmp edx, 0x1
	jne .Lwindow_ancestor_4e
	mov rax, r10
	ret
.Lwindow_ancestor_4e:
	test r10, r10
	je .Lwindow_ancestor_5d
	mov r8, r10
	dec r9d
	jne .Lwindow_ancestor_2e
	jmp .Lwindow_ancestor_slow
.Lwindow_ancestor_5d:
	cmp edx, 0x3
	jne .Lwindow_ancestor_75
	mov r10, qword ptr [rax + 0x20]
	test r10, r10
	je .Lwindow_ancestor_75
	mov r8, r10
	dec r9d
	jne .Lwindow_ancestor_2e
	jmp .Lwindow_ancestor_slow
.Lwindow_ancestor_75:
	mov rax, r8
	ret
.Lwindow_ancestor_slow:
	.byte 0xe9
	.long 0
raven_prelude_window_ancestor_end:

	.global raven_prelude_map_points, raven_prelude_map_points_end
raven_prelude_map_points:
	mov r10, qword ptr [rip + 0]
	test r10, r10
	je .Lmap_points_slow
	mov r11, qword ptr [r10 + 0x700]
	test r11, r11
	je .Lmap_points_slow
	cmp r9d, 0x40
	ja .Lmap_points_slow
	push rbx
	push rsi
	xor ebx, ebx
	xor esi, esi
	test rcx, rcx
	je .Lmap_points_4f
	mov eax, ecx
	and eax, 0x1ff
	shl eax, 0x8
	add rax, r11
	cmp qword ptr [rax], rcx
	jne .Lmap_points_9f
	mov ebx, dword ptr [rax + 0xf8]
	mov esi, dword ptr [rax + 0xfc]
.Lmap_points_4f:
	test rdx, rdx
	je .Lmap_points_72
	mov eax, edx
	and eax, 0x1ff
	shl eax, 0x8
	add rax, r11
	cmp qword ptr [rax], rdx
	jne .Lmap_points_9f
	sub ebx, dword ptr [rax + 0xf8]
	sub esi, dword ptr [rax + 0xfc]
.Lmap_points_72:
	test r9d, r9d
	je .Lmap_points_91
	test r8, r8
	je .Lmap_points_9f
	mov eax, r9d
	mov r10, r8
.Lmap_points_82:
	add dword ptr [r10], ebx
	add dword ptr [r10 + 0x4], esi
	add r10, 0x8
	dec eax
	jne .Lmap_points_82
.Lmap_points_91:
	movzx eax, bx
	movzx esi, si
	shl esi, 0x10
	or eax, esi
	pop rsi
	pop rbx
	ret
.Lmap_points_9f:
	pop rsi
	pop rbx
.Lmap_points_slow:
	.byte 0xe9
	.long 0
raven_prelude_map_points_end:

	.global raven_prelude_locale_number, raven_prelude_locale_number_end
raven_prelude_locale_number:
	mov r10, qword ptr [rip + 0]
	test r10, r10
	je .Llocale_number_slow
	test edx, edx
	jne .Llocale_number_slow
	test rcx, rcx
	je .Llocale_number_52
	movzx eax, word ptr [rcx]
	or eax, 0x20
	cmp eax, 0x65
	jne .Llocale_number_slow
	movzx eax, word ptr [rcx + 0x2]
	or eax, 0x20
	cmp eax, 0x6e
	jne .Llocale_number_slow
	cmp word ptr [rcx + 0x4], 0x2d
	jne .Llocale_number_slow
	movzx eax, word ptr [rcx + 0x6]
	or eax, 0x20
	cmp eax, 0x75
	jne .Llocale_number_slow
	movzx eax, word ptr [rcx + 0x8]
	or eax, 0x20
	cmp eax, 0x73
	jne .Llocale_number_slow
	cmp word ptr [rcx + 0xa], 0x0
	jne .Llocale_number_slow
.Llocale_number_52:
	mov eax, 0x409
	ret
.Llocale_number_slow:
	.byte 0xe9
	.long 0
raven_prelude_locale_number_end:

	.global raven_prelude_resource_load, raven_prelude_resource_load_end
raven_prelude_resource_load:
	mov r10, qword ptr [rip + 0]
	test r10, r10
	je .Lresource_load_slow
	test rcx, rcx
	je .Lresource_load_slow
	test cl, 0x3
	jne .Lresource_load_slow
	test rdx, rdx
	je .Lresource_load_slow
	mov eax, dword ptr [rdx]
	add rax, rcx
	ret
.Lresource_load_slow:
	.byte 0xe9
	.long 0
raven_prelude_resource_load_end:

	.global raven_prelude_resource_size, raven_prelude_resource_size_end
raven_prelude_resource_size:
	mov r10, qword ptr [rip + 0]
	test r10, r10
	je .Lresource_size_slow
	test rdx, rdx
	je .Lresource_size_slow
	mov eax, dword ptr [rdx + 0x4]
	ret
.Lresource_size_slow:
	.byte 0xe9
	.long 0
raven_prelude_resource_size_end:

	.global raven_prelude_resource_lock, raven_prelude_resource_lock_end
raven_prelude_resource_lock:
	mov r10, qword ptr [rip + 0]
	mov rax, rcx
	ret
	.byte 0xe9
	.long 0
raven_prelude_resource_lock_end:

	.global raven_prelude_next_hook, raven_prelude_next_hook_end
raven_prelude_next_hook:
	mov r10, qword ptr [rip + 0]
	test r10, r10
	je .Lnext_hook_slow
	mov r11, qword ptr [r10 + 0x770]
	test r11, r11
	je .Lnext_hook_slow
	add r11, 0x400
	mov eax, dword ptr [r11]
	cmp eax, 0x40
	ja .Lnext_hook_slow
	add r11, 0x10
.Lnext_hook_2b:
	test eax, eax
	je .Lnext_hook_slow
	cmp qword ptr [r11], rcx
	je .Lnext_hook_3c
	add r11, 0x10
	dec eax
	jmp .Lnext_hook_2b
.Lnext_hook_3c:
	mov r10d, dword ptr [r11 + 0x8]
.Lnext_hook_40:
	dec eax
	je .Lnext_hook_67
	add r11, 0x10
	cmp dword ptr [r11 + 0x8], r10d
	jne .Lnext_hook_40
	cmp dword ptr [r11 + 0xc], 0x0
	je .Lnext_hook_slow
	push rax
	mov eax, dword ptr gs:0x48
	cmp dword ptr [r11 + 0xc], eax
	pop rax
	je .Lnext_hook_slow
	jmp .Lnext_hook_40
.Lnext_hook_67:
	xor eax, eax
	ret
.Lnext_hook_slow:
	.byte 0xe9
	.long 0
raven_prelude_next_hook_end:

	.global raven_prelude_silent_trace, raven_prelude_silent_trace_end
raven_prelude_silent_trace:
	lea r11, [rip + 0]
	ret
	.byte 0xe9
	.long 0
raven_prelude_silent_trace_end:

	.global raven_prelude_theme_state, raven_prelude_theme_state_end
raven_prelude_theme_state:
	mov r10, qword ptr [rip + 0]
	test r10, r10
	je .Ltheme_state_slow
	mov eax, dword ptr [r10 + 0x790]
	test eax, eax
	je .Ltheme_state_slow
	dec eax
	ret
.Ltheme_state_slow:
	.byte 0xe9
	.long 0
raven_prelude_theme_state_end:

	.global raven_prelude_theme_flags, raven_prelude_theme_flags_end
raven_prelude_theme_flags:
	mov r10, qword ptr [rip + 0]
	test r10, r10
	je .Ltheme_flags_slow
	mov eax, dword ptr [r10 + 0x790]
	test eax, eax
	je .Ltheme_flags_slow
	dec eax
	imul eax, eax, 0x7
	ret
.Ltheme_flags_slow:
	.byte 0xe9
	.long 0
raven_prelude_theme_flags_end:

	.global raven_prelude_string_compare, raven_prelude_string_compare_end
raven_prelude_string_compare:
	test r8, r8
	je .Lstring_compare_slow
	mov r10, qword ptr [rsp + 0x28]
	test r10, r10
	je .Lstring_compare_slow
	mov r11d, dword ptr [rsp + 0x30]
	mov r9d, r9d
	test r9d, r9d
	jns .Lstring_compare_34
	xor r9d, r9d
.Lstring_compare_27:
	cmp word ptr [r8 + r9 * 2], 0x0
	je .Lstring_compare_34
	inc r9
	jmp .Lstring_compare_27
.Lstring_compare_34:
	test r11d, r11d
	jns .Lstring_compare_49
	xor r11d, r11d
.Lstring_compare_3c:
	cmp word ptr [r10 + r11 * 2], 0x0
	je .Lstring_compare_49
	inc r11
	jmp .Lstring_compare_3c
.Lstring_compare_49:
	push rbx
	push rsi
	xor ecx, ecx
.Lstring_compare_4d:
	cmp rcx, r9
	je .Lstring_compare_91
	cmp rcx, r11
	je .Lstring_compare_91
	movzx eax, word ptr [r8 + rcx * 2]
	movzx ebx, word ptr [r10 + rcx * 2]
	cmp eax, 0x80
	jae .Lstring_compare_d8
	cmp ebx, 0x80
	jae .Lstring_compare_d8
	lea esi, [rax - 0x61]
	cmp esi, 0x19
	ja .Lstring_compare_7b
	sub eax, 0x20
.Lstring_compare_7b:
	lea esi, [rbx - 0x61]
	cmp esi, 0x19
	ja .Lstring_compare_86
	sub ebx, 0x20
.Lstring_compare_86:
	cmp eax, ebx
	jb .Lstring_compare_c2
	ja .Lstring_compare_c9
	inc rcx
	jmp .Lstring_compare_4d
.Lstring_compare_91:
	cmp r9, r11
	jb .Lstring_compare_c2
	ja .Lstring_compare_c9
	test edx, 0x10000011
	jne .Lstring_compare_d0
	xor ecx, ecx
.Lstring_compare_a2:
	cmp rcx, r9
	je .Lstring_compare_d0
	movzx eax, word ptr [r8 + rcx * 2]
	movzx ebx, word ptr [r10 + rcx * 2]
	cmp eax, ebx
	jne .Lstring_compare_ba
	inc rcx
	jmp .Lstring_compare_a2
.Lstring_compare_ba:
	lea esi, [rax - 0x41]
	cmp esi, 0x19
	jbe .Lstring_compare_c9
.Lstring_compare_c2:
	mov eax, 0x1
	jmp .Lstring_compare_d5
.Lstring_compare_c9:
	mov eax, 0x3
	jmp .Lstring_compare_d5
.Lstring_compare_d0:
	mov eax, 0x2
.Lstring_compare_d5:
	pop rsi
	pop rbx
	ret
.Lstring_compare_d8:
	pop rsi
	pop rbx
.Lstring_compare_slow:
	.byte 0xe9
	.long 0
raven_prelude_string_compare_end:

	.global raven_prelude_string_lstrlen_w, raven_prelude_string_lstrlen_w_end
raven_prelude_string_lstrlen_w:
	test rcx, rcx
	je .Lstring_lstrlen_w_1b
	mov rax, rcx
.Lstring_lstrlen_w_8:
	cmp word ptr [rax], 0x0
	je .Lstring_lstrlen_w_14
	add rax, 0x2
	jmp .Lstring_lstrlen_w_8
.Lstring_lstrlen_w_14:
	sub rax, rcx
	shr rax, 1
	ret
.Lstring_lstrlen_w_1b:
	xor eax, eax
	ret
	.byte 0xe9
	.long 0
raven_prelude_string_lstrlen_w_end:

	.global raven_prelude_string_lstrlen_a, raven_prelude_string_lstrlen_a_end
raven_prelude_string_lstrlen_a:
	test rcx, rcx
	je .Lstring_lstrlen_a_16
	mov rax, rcx
.Lstring_lstrlen_a_8:
	cmp byte ptr [rax], 0x0
	je .Lstring_lstrlen_a_12
	inc rax
	jmp .Lstring_lstrlen_a_8
.Lstring_lstrlen_a_12:
	sub rax, rcx
	ret
.Lstring_lstrlen_a_16:
	xor eax, eax
	ret
	.byte 0xe9
	.long 0
raven_prelude_string_lstrlen_a_end:

	.global raven_prelude_string_lstrcpyn_w, raven_prelude_string_lstrcpyn_w_end
raven_prelude_string_lstrcpyn_w:
	test rcx, rcx
	je .Lstring_lstrcpyn_w_slow
	test rdx, rdx
	je .Lstring_lstrcpyn_w_slow
	test r8d, r8d
	jle .Lstring_lstrcpyn_w_slow
	mov rax, rcx
	lea r9d, [r8 - 0x1]
.Lstring_lstrcpyn_w_16:
	test r9d, r9d
	je .Lstring_lstrcpyn_w_35
	movzx r10d, word ptr [rdx]
	test r10d, r10d
	je .Lstring_lstrcpyn_w_35
	mov word ptr [rcx], r10w
	add rcx, 0x2
	add rdx, 0x2
	dec r9d
	jmp .Lstring_lstrcpyn_w_16
.Lstring_lstrcpyn_w_35:
	mov word ptr [rcx], 0x0
	ret
.Lstring_lstrcpyn_w_slow:
	.byte 0xe9
	.long 0
raven_prelude_string_lstrcpyn_w_end:

	.global raven_prelude_string_lstrcpyn_a, raven_prelude_string_lstrcpyn_a_end
raven_prelude_string_lstrcpyn_a:
	test rcx, rcx
	je .Lstring_lstrcpyn_a_slow
	test rdx, rdx
	je .Lstring_lstrcpyn_a_slow
	test r8d, r8d
	jle .Lstring_lstrcpyn_a_slow
	mov rax, rcx
	lea r9d, [r8 - 0x1]
.Lstring_lstrcpyn_a_16:
	test r9d, r9d
	je .Lstring_lstrcpyn_a_32
	movzx r10d, byte ptr [rdx]
	test r10d, r10d
	je .Lstring_lstrcpyn_a_32
	mov byte ptr [rcx], r10b
	inc rcx
	inc rdx
	dec r9d
	jmp .Lstring_lstrcpyn_a_16
.Lstring_lstrcpyn_a_32:
	mov byte ptr [rcx], 0x0
	ret
.Lstring_lstrcpyn_a_slow:
	.byte 0xe9
	.long 0
raven_prelude_string_lstrcpyn_a_end:

	.global raven_prelude_string_lstrcmp_w, raven_prelude_string_lstrcmp_w_end
raven_prelude_string_lstrcmp_w:
	test rcx, rcx
	je .Lstring_lstrcmp_w_slow
	test rdx, rdx
	je .Lstring_lstrcmp_w_slow
	xor r8d, r8d
.Lstring_lstrcmp_w_11:
	movzx eax, word ptr [rcx + r8 * 2]
	movzx r9d, word ptr [rdx + r8 * 2]
	cmp eax, 0x80
	jae .Lstring_lstrcmp_w_slow
	cmp r9d, 0x80
	jae .Lstring_lstrcmp_w_slow
	lea r10d, [rax - 0x61]
	cmp r10d, 0x19
	ja .Lstring_lstrcmp_w_38
	sub eax, 0x20
.Lstring_lstrcmp_w_38:
	lea r10d, [r9 - 0x61]
	cmp r10d, 0x19
	ja .Lstring_lstrcmp_w_46
	sub r9d, 0x20
.Lstring_lstrcmp_w_46:
	cmp eax, r9d
	jb .Lstring_lstrcmp_w_7b
	ja .Lstring_lstrcmp_w_81
	test eax, eax
	je .Lstring_lstrcmp_w_56
	inc r8
	jmp .Lstring_lstrcmp_w_11
.Lstring_lstrcmp_w_56:
	xor r8d, r8d
.Lstring_lstrcmp_w_59:
	movzx eax, word ptr [rcx + r8 * 2]
	movzx r9d, word ptr [rdx + r8 * 2]
	cmp eax, r9d
	jne .Lstring_lstrcmp_w_71
	test eax, eax
	je .Lstring_lstrcmp_w_87
	inc r8
	jmp .Lstring_lstrcmp_w_59
.Lstring_lstrcmp_w_71:
	lea r10d, [rax - 0x41]
	cmp r10d, 0x19
	jbe .Lstring_lstrcmp_w_81
.Lstring_lstrcmp_w_7b:
	mov eax, 0xffffffff
	ret
.Lstring_lstrcmp_w_81:
	mov eax, 0x1
	ret
.Lstring_lstrcmp_w_87:
	xor eax, eax
	ret
.Lstring_lstrcmp_w_slow:
	.byte 0xe9
	.long 0
raven_prelude_string_lstrcmp_w_end:

	.global raven_prelude_string_multi_byte_to_wide_char, raven_prelude_string_multi_byte_to_wide_char_end
raven_prelude_string_multi_byte_to_wide_char:
	cmp ecx, 0xfde9
	jne .Lstring_multi_byte_to_wide_char_16
	test edx, 0xfffffff7
	jne .Lstring_multi_byte_to_wide_char_slow
	jmp .Lstring_multi_byte_to_wide_char_2a
.Lstring_multi_byte_to_wide_char_16:
	test ecx, ecx
	je .Lstring_multi_byte_to_wide_char_22
	cmp ecx, 0x4e4
	jne .Lstring_multi_byte_to_wide_char_slow
.Lstring_multi_byte_to_wide_char_22:
	test edx, 0xfffffff6
	jne .Lstring_multi_byte_to_wide_char_slow
.Lstring_multi_byte_to_wide_char_2a:
	test r8, r8
	je .Lstring_multi_byte_to_wide_char_slow
	mov eax, r9d
	cmp eax, 0xffffffff
	jne .Lstring_multi_byte_to_wide_char_4e
	mov r10, r8
.Lstring_multi_byte_to_wide_char_3a:
	cmp byte ptr [r10], 0x0
	je .Lstring_multi_byte_to_wide_char_45
	inc r10
	jmp .Lstring_multi_byte_to_wide_char_3a
.Lstring_multi_byte_to_wide_char_45:
	sub r10, r8
	lea eax, [r10 + 0x1]
	jmp .Lstring_multi_byte_to_wide_char_52
.Lstring_multi_byte_to_wide_char_4e:
	test eax, eax
	jle .Lstring_multi_byte_to_wide_char_slow
.Lstring_multi_byte_to_wide_char_52:
	xor r10d, r10d
.Lstring_multi_byte_to_wide_char_55:
	cmp r10d, eax
	jae .Lstring_multi_byte_to_wide_char_66
	test byte ptr [r8 + r10 * 1], 0x80
	jne .Lstring_multi_byte_to_wide_char_slow
	inc r10d
	jmp .Lstring_multi_byte_to_wide_char_55
.Lstring_multi_byte_to_wide_char_66:
	mov r11, qword ptr [rsp + 0x28]
	mov r10d, dword ptr [rsp + 0x30]
	test r10d, r10d
	je .Lstring_multi_byte_to_wide_char_9b
	test r11, r11
	je .Lstring_multi_byte_to_wide_char_slow
	cmp r11, r8
	je .Lstring_multi_byte_to_wide_char_slow
	cmp r10d, eax
	jl .Lstring_multi_byte_to_wide_char_slow
	xor r10d, r10d
.Lstring_multi_byte_to_wide_char_87:
	cmp r10d, eax
	jae .Lstring_multi_byte_to_wide_char_9b
	movzx edx, byte ptr [r8 + r10 * 1]
	mov word ptr [r11 + r10 * 2], dx
	inc r10d
	jmp .Lstring_multi_byte_to_wide_char_87
.Lstring_multi_byte_to_wide_char_9b:
	ret
.Lstring_multi_byte_to_wide_char_slow:
	.byte 0xe9
	.long 0
raven_prelude_string_multi_byte_to_wide_char_end:

	.global raven_prelude_string_wide_char_to_multi_byte, raven_prelude_string_wide_char_to_multi_byte_end
raven_prelude_string_wide_char_to_multi_byte:
	test ecx, ecx
	je .Lstring_wide_char_to_multi_byte_30
	cmp ecx, 0x4e4
	je .Lstring_wide_char_to_multi_byte_30
	cmp ecx, 0xfde9
	jne .Lstring_wide_char_to_multi_byte_slow
	cmp qword ptr [rsp + 0x38], 0x0
	jne .Lstring_wide_char_to_multi_byte_slow
	cmp qword ptr [rsp + 0x40], 0x0
	jne .Lstring_wide_char_to_multi_byte_slow
.Lstring_wide_char_to_multi_byte_30:
	test edx, edx
	jne .Lstring_wide_char_to_multi_byte_slow
	test r8, r8
	je .Lstring_wide_char_to_multi_byte_slow
	mov eax, r9d
	cmp eax, 0xffffffff
	jne .Lstring_wide_char_to_multi_byte_61
	mov r10, r8
.Lstring_wide_char_to_multi_byte_48:
	cmp word ptr [r10], 0x0
	je .Lstring_wide_char_to_multi_byte_55
	add r10, 0x2
	jmp .Lstring_wide_char_to_multi_byte_48
.Lstring_wide_char_to_multi_byte_55:
	sub r10, r8
	shr r10, 1
	lea eax, [r10 + 0x1]
	jmp .Lstring_wide_char_to_multi_byte_65
.Lstring_wide_char_to_multi_byte_61:
	test eax, eax
	jle .Lstring_wide_char_to_multi_byte_slow
.Lstring_wide_char_to_multi_byte_65:
	xor r10d, r10d
.Lstring_wide_char_to_multi_byte_68:
	cmp r10d, eax
	jae .Lstring_wide_char_to_multi_byte_7b
	cmp word ptr [r8 + r10 * 2], 0x80
	jae .Lstring_wide_char_to_multi_byte_slow
	inc r10d
	jmp .Lstring_wide_char_to_multi_byte_68
.Lstring_wide_char_to_multi_byte_7b:
	mov r11, qword ptr [rsp + 0x28]
	mov r10d, dword ptr [rsp + 0x30]
	test r10d, r10d
	je .Lstring_wide_char_to_multi_byte_aa
	test r11, r11
	je .Lstring_wide_char_to_multi_byte_slow
	cmp r10d, eax
	jl .Lstring_wide_char_to_multi_byte_slow
	xor r10d, r10d
.Lstring_wide_char_to_multi_byte_97:
	cmp r10d, eax
	jae .Lstring_wide_char_to_multi_byte_aa
	movzx edx, word ptr [r8 + r10 * 2]
	mov byte ptr [r11 + r10 * 1], dl
	inc r10d
	jmp .Lstring_wide_char_to_multi_byte_97
.Lstring_wide_char_to_multi_byte_aa:
	mov r11, qword ptr [rsp + 0x40]
	test r11, r11
	je .Lstring_wide_char_to_multi_byte_bb
	mov dword ptr [r11], 0x0
.Lstring_wide_char_to_multi_byte_bb:
	ret
.Lstring_wide_char_to_multi_byte_slow:
	.byte 0xe9
	.long 0
raven_prelude_string_wide_char_to_multi_byte_end:

	.global raven_prelude_string_get_thread_locale, raven_prelude_string_get_thread_locale_end
raven_prelude_string_get_thread_locale:
	mov eax, 0x409
	ret
	.byte 0xe9
	.long 0
raven_prelude_string_get_thread_locale_end:

	.global raven_prelude_string_get_a_c_p, raven_prelude_string_get_a_c_p_end
raven_prelude_string_get_a_c_p:
	mov eax, 0x4e4
	ret
	.byte 0xe9
	.long 0
raven_prelude_string_get_a_c_p_end:

	.global raven_prelude_string_set_thread_locale, raven_prelude_string_set_thread_locale_end
raven_prelude_string_set_thread_locale:
	mov eax, 0x1
	ret
	.byte 0xe9
	.long 0
raven_prelude_string_set_thread_locale_end:

/* QueryPerformanceCounter and the unbiased interrupt times, once the runtime has published how the time stamp counter relates to the
 * performance counter (see guest_clock_api): mov r10,[pointer]; test r10,r10; jz served; mov r11,[r10+calibration]; test r11,r11;
 * jz served; the count from rdtsc as 32.32 fixed point from the base of the slot at that offset; mov [rcx],rax; mov eax,1; ret. Both
 * jumps go to the served stub, whose rel32 the runtime fills in where served and served_again end. */
	.global raven_prelude_clock, raven_prelude_clock_end, raven_prelude_clock_served, raven_prelude_clock_served_again
raven_prelude_clock:
	mov r10, qword ptr [rip + 0]
	test r10, r10
	.byte 0x0f, 0x84
	.long 0
raven_prelude_clock_served:
	mov r11, qword ptr [r10 + 0x800]
	test r11, r11
	.byte 0x0f, 0x84
	.long 0
raven_prelude_clock_served_again:
	rdtsc
	shl rdx, 0x20
	or rax, rdx
	add r11, r10
	sub rax, qword ptr [r11]
	mul qword ptr [r11 + 0x10]
	shrd rax, rdx, 0x20
	add rax, qword ptr [r11 + 0x8]
	mov qword ptr [rcx], rax
	mov eax, 0x1
	ret
raven_prelude_clock_end:

/* VirtualAlloc of a read-write commit that falls in a range open in the guest (see guest_memory_api): mov r10,[pointer]; test r10,r10;
 * jz slow; test rcx,rcx; jz slow; test rdx,rdx; jz slow; cmp r8d,MEM_COMMIT; jne slow; cmp r9d,PAGE_READWRITE; je main; slow: jmp trap;
 * restore: mov r8d,MEM_COMMIT; mov r9d,PAGE_READWRITE; jmp slow; main: rax = first page, r11 = end page; jbe slow; look through the open
 * ranges at commit_windows for one holding [rax, r11), take a log slot with lock xadd at commit_log, store the range there and return
 * rax; a full log or no range goes back to restore. The runtime fills in the rel32 that ends at trap. */
	.global raven_prelude_commit, raven_prelude_commit_end, raven_prelude_commit_trap
raven_prelude_commit:
	mov r10, qword ptr [rip + 0]
	test r10, r10
	je .Lcommit_slow
	test rcx, rcx
	je .Lcommit_slow
	test rdx, rdx
	je .Lcommit_slow
	cmp r8d, 0x1000
	jne .Lcommit_slow
	cmp r9d, 0x4
	je .Lcommit_38
.Lcommit_slow:
	.byte 0xe9
	.long 0
raven_prelude_commit_trap:
.Lcommit_restore:
	mov r8d, 0x1000
	mov r9d, 0x4
	jmp .Lcommit_slow
.Lcommit_38:
	mov rax, rcx
	and rax, 0xfffffffffffff000
	lea r11, [rcx + rdx * 1 + 0xfff]
	and r11, 0xfffffffffffff000
	cmp r11, rax
	jbe .Lcommit_slow
	mov r8d, dword ptr [r10 + 0xa00]
	lea r9, [r10 + 0xa08]
.Lcommit_63:
	test r8d, r8d
	je .Lcommit_restore
	cmp rax, qword ptr [r9]
	jb .Lcommit_9d
	cmp r11, qword ptr [r9 + 0x8]
	ja .Lcommit_9d
	mov r8d, 0x1
	lock xadd dword ptr [r10 + 0xd10], r8d
	cmp r8d, 0x20
	jae .Lcommit_restore
	shl r8, 0x4
	mov qword ptr [r10 + r8 * 1 + 0xd18], rax
	mov qword ptr [r10 + r8 * 1 + 0xd20], r11
	ret
.Lcommit_9d:
	add r9, 0x10
	dec r8d
	jmp .Lcommit_63
raven_prelude_commit_end:

/* VirtualFree of a small decommit in an open range: mov r10,[pointer]; test r10,r10; jz slow; test rcx,rcx; jz slow; test rdx,rdx;
 * jz slow; cmp r8d,MEM_DECOMMIT; je main; slow: jmp trap; restore: mov r8d,MEM_DECOMMIT; jmp slow; main: cmp rdx,256 KiB; ja slow;
 * rax = first page, r11 = end page; look through the open ranges for one holding [rax, r11), take a log slot with lock xadd, zero the
 * pages with rep stosb (keeping rdi), store the range with its low bit set and return TRUE; a full log or no range goes back to
 * restore. The runtime fills in the rel32 that ends at trap. */
	.global raven_prelude_decommit, raven_prelude_decommit_end, raven_prelude_decommit_trap
raven_prelude_decommit:
	mov r10, qword ptr [rip + 0]
	test r10, r10
	je .Ldecommit_slow
	test rcx, rcx
	je .Ldecommit_slow
	test rdx, rdx
	je .Ldecommit_slow
	cmp r8d, 0x4000
	je .Ldecommit_2c
.Ldecommit_slow:
	.byte 0xe9
	.long 0
raven_prelude_decommit_trap:
.Ldecommit_restore:
	mov r8d, 0x4000
	jmp .Ldecommit_slow
.Ldecommit_2c:
	cmp rdx, 0x40000
	ja .Ldecommit_slow
	mov rax, rcx
	and rax, 0xfffffffffffff000
	lea r11, [rcx + rdx * 1 + 0xfff]
	and r11, 0xfffffffffffff000
	mov r8d, dword ptr [r10 + 0xa00]
	lea r9, [r10 + 0xa08]
.Ldecommit_5b:
	test r8d, r8d
	je .Ldecommit_restore
	cmp rax, qword ptr [r9]
	jb .Ldecommit_b0
	cmp r11, qword ptr [r9 + 0x8]
	ja .Ldecommit_b0
	mov r8d, 0x1
	lock xadd dword ptr [r10 + 0xd10], r8d
	cmp r8d, 0x20
	jae .Ldecommit_restore
	shl r8, 0x4
	push rdi
	mov rdi, rax
	mov r9, rax
	mov rcx, r11
	sub rcx, rax
	xor eax, eax
	rep stos byte ptr [rdi], al
	pop rdi
	or r9, 0x1
	mov qword ptr [r10 + r8 * 1 + 0xd18], r9
	mov qword ptr [r10 + r8 * 1 + 0xd20], r11
	mov eax, 0x1
	ret
.Ldecommit_b0:
	add r9, 0x10
	dec r8d
	jmp .Ldecommit_5b
raven_prelude_decommit_end:

/* The preludes of the commands a renderer records by the thousand (see queued_arguments): each puts the call in the thread's ring of
 * queued calls in the served block, which the runtime drains in order, instead of stopping. The block comes through GS as for the served
 * stubs; the ring's head, tail and whether it is on sit at 0x80, 0x88 and 0x90, its sixteen records of sixteen words at 0x100. A record
 * holds the stub the call stands for, then its arguments: the four registers, those on the stack, and for OpenGL the low halves of
 * XMM0 to XMM3; vkCmdBindDescriptorSets and vkCmdBindVertexBuffers copy their arrays into the record and point at the copies. A ring
 * that is off or full, or arrays too long for a record, take the served stub instead (`slow`, a jmp whose rel32 the runtime fills in,
 * as it does the stub's address the lea that ends at `stub` loads). Every prelude fills a slot of 384 bytes. */
	.macro queued name, arguments, kind=plain
	.global raven_prelude_queued_\name, raven_prelude_queued_\name\()_stub, raven_prelude_queued_\name\()_end
raven_prelude_queued_\name:
	xchg ax, ax
	mov r10, qword ptr gs:[0x1ff0]
	test r10, r10
	{disp32} je .Lqueued_\name\()_slow
	cmp qword ptr [r10 + 0x90], 1
	{disp32} jne .Lqueued_\name\()_slow
	mov rax, qword ptr [r10 + 0x80]
	mov r11, rax
	sub r11, qword ptr [r10 + 0x88]
	cmp r11, 16
	{disp32} jae .Lqueued_\name\()_slow
	mov r11, rax
	and r11d, 15
	shl r11, 7
	add r11, r10
	mov qword ptr [r11 + 0x108], rcx
	mov qword ptr [r11 + 0x110], rdx
	mov qword ptr [r11 + 0x118], r8
	mov qword ptr [r11 + 0x120], r9
	.ifc \kind, gl_draw_elements
	/* Only while the element buffer the runtime last saw bound is published. */
	cmp qword ptr [r10 + 0x98], 1
	{disp32} jne .Lqueued_\name\()_slow
	.endif
	.ifc \kind, gl_bind_buffer
	/* A change of the element buffer is served, so that the capability is published before another draw enters the ring. */
	cmp ecx, 0x8893
	{disp32} je .Lqueued_\name\()_slow
	.endif
	.ifc \kind, descriptors
	cmp dword ptr [rsp + 40], 4
	{disp32} ja .Lqueued_\name\()_slow
	cmp dword ptr [rsp + 56], 4
	{disp32} ja .Lqueued_\name\()_slow
	.endif
	.ifc \kind, vertices
	cmp r8d, 4
	{disp32} ja .Lqueued_\name\()_slow
	.endif
	.if \arguments > 4
	.set .Lqueued_index, 4
	.rept \arguments - 4
	mov rcx, qword ptr [rsp + 40 + (.Lqueued_index - 4) * 8]
	mov qword ptr [r11 + 0x108 + .Lqueued_index * 8], rcx
	.set .Lqueued_index, .Lqueued_index + 1
	.endr
	.endif
	.ifc \kind, plain
	.else
	.ifnc \kind, descriptors
	.ifnc \kind, vertices
	/* The low XMM words, before the caller can reuse them; Vulkan's arrays use these words instead. */
	movq qword ptr [r11 + 0x148], xmm0
	movq qword ptr [r11 + 0x150], xmm1
	movq qword ptr [r11 + 0x158], xmm2
	movq qword ptr [r11 + 0x160], xmm3
	.endif
	.endif
	.endif
	.ifc \kind, descriptors
	/* The descriptor sets and the dynamic offsets, into the record; the published pointers name the copies. */
	push rsi
	push rdi
	queued_copy 5, 4, 80, movsq
	queued_copy 7, 6, 112, movsd
	pop rdi
	pop rsi
	.endif
	.ifc \kind, vertices
	/* The buffers and their offsets, likewise. */
	push rsi
	push rdi
	queued_copy 3, 2, 64, movsq
	queued_copy 4, 2, 96, movsq
	pop rdi
	pop rsi
	.endif
	lea rcx, [rip + 0]
raven_prelude_queued_\name\()_stub:
	mov qword ptr [r11 + 0x100], rcx
	inc qword ptr [r10 + 0x80]
	ret
	.skip 379 - (. - raven_prelude_queued_\name), 0x90
.Lqueued_\name\()_slow:
	.byte 0xe9
	.long 0
raven_prelude_queued_\name\()_end:
	.endm

/* Copies the array whose pointer is argument `pointer` and whose count is argument `count` to `payload` bytes into the record, and
 * points the argument at the copy. */
	.macro queued_copy pointer, count, payload, move
	mov rsi, qword ptr [r11 + 0x100 + (\pointer + 1) * 8]
	lea rdi, [r11 + 0x100 + \payload]
	mov qword ptr [r11 + 0x100 + (\pointer + 1) * 8], rdi
	mov ecx, dword ptr [r11 + 0x100 + (\count + 1) * 8]
	rep \move
	.endm

	queued vk3, 3
	queued vk4, 4
	queued vk5, 5
	queued vk6, 6
	queued vk_descriptors, 8, descriptors
	queued vk_vertices, 5, vertices
	queued gl1, 1, gl
	queued gl2, 2, gl
	queued gl3, 3, gl
	queued gl4, 4, gl
	queued gl_draw_elements, 4, gl_draw_elements
	queued gl_bind_buffer, 2, gl_bind_buffer

/* An export of ntdll that is a system service, in the 24 bytes of Windows 10's: programs that hook ntdll check them, copy them
 * elsewhere and run the copy (Chromium's sandbox/win/src/service_resolver_64.cc shows which they check). The runtime writes the number
 * where `service` ends and the address of its flag where `flag` ends. With the flag clear the export makes the system call. With it set
 * it jumps to the number, which is the address of the service's entry on the service page, so a copy works from any address; RCX and
 * the stack are then still as the caller left them. The runtime recognises these bytes when it looks for the stub behind an export
 * (see served_stub_rva). */
	.global raven_nt_syscall, raven_nt_syscall_service, raven_nt_syscall_flag, raven_nt_syscall_return, raven_nt_syscall_end
raven_nt_syscall:
	{load} mov r10, rcx
	mov eax, 0
raven_nt_syscall_service:
	.byte 0xf6, 0x04, 0x25		/* test byte ptr [flag], 1 */
	.long 0
raven_nt_syscall_flag:
	.byte 1
	jnz short 1f
	syscall
raven_nt_syscall_return:
	ret
1:	jmp rax
	ret
raven_nt_syscall_end:

/* An entry of the service page: on to the stub whose address the runtime writes after it, or, for a service that has none, the system
 * call with the number still in RAX and argument one in R10. */
	.global raven_service_stubbed, raven_service_stubbed_end, raven_service_trapped, raven_service_trapped_end
raven_service_stubbed:
	jmp qword ptr [rip + 0]
raven_service_stubbed_end:
raven_service_trapped:
	syscall
	ret
raven_service_trapped_end:

/* With RAVEN_CYCLES, the entries without a stub lead here instead: the time stamp counter as the thread asks for the service, in word
 * 496 of its block (see served_stub.h), then the system call as from the entry. The landing is where a thread goes on after the runtime
 * answered a system call it made: it leaves the counter in word 497 and goes to the address the runtime left in word 498, with every
 * register and flag as the runtime left them. */
	.global raven_service_timed, raven_service_timed_end, raven_service_landing, raven_service_landing_end
raven_service_timed:
	push rax
	push rdx
	push rcx
	rdtsc
	mov rcx, qword ptr gs:[0x1ff0]
	jrcxz 1f			/* a thread without a block of its own makes the call untimed */
	mov dword ptr [rcx + 496 * 8], eax
	mov dword ptr [rcx + 496 * 8 + 4], edx
1:	pop rcx
	pop rdx
	pop rax
	syscall
	ret
raven_service_timed_end:
raven_service_landing:
	push rax
	push rdx
	push rcx
	rdtsc
	mov rcx, qword ptr gs:[0x1ff0]
	mov dword ptr [rcx + 497 * 8], eax
	mov dword ptr [rcx + 497 * 8 + 4], edx
	push qword ptr [rcx + 498 * 8]
	mov rcx, qword ptr [rsp + 8]
	mov rdx, qword ptr [rsp + 16]
	mov rax, qword ptr [rsp + 24]
	ret 24
raven_service_landing_end:

	.section .note.GNU-stack, "", @progbits
