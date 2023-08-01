#include <linux/config.h>
#include <asm/segment.h>
#include <linux/version.h>
#include <linux/compile.h>
#include <asm/boot.h>
#include <asm/e820.h>
#include <asm/page.h>
	
#define SIG1	0xAA55
#define SIG2	0x5A5A

INITSEG  = DEF_INITSEG
SYSSEG   = DEF_SYSSEG
SETUPSEG = DEF_SETUPSEG
DELTA_INITSEG = SETUPSEG - INITSEG

.code16
.globl begtext, begdata, begbss, endtext, enddata, endbss

.text
begtext:
.data
begdata:
.bss
begbss:
.text

start:
	jmp	trampoline
		.ascii	"HdrS"
		.word	0x0203
realmode_swtch:	.word	0, 0
start_sys_seg:	.word	SYSSEG
		.word	kernel_version

type_of_loader:	.byte	0
	
loadflags:
LOADED_HIGH	= 1	
CAN_USE_HEAP	= 0x80

		.byte	LOADED_HIGH
setup_move_size: .word  0x8000

code32_start:
		.long	0x100000
ramdisk_image:	.long	0
ramdisk_size:	.long	0

bootsect_kludge:
		.word  bootsect_helper, SETUPSEG
heap_end_ptr:	.word  modelist+1024
pad1:		.word  0
cmd_line_ptr:	.long  0	
ramdisk_max:	.long __MAXMEM-1

trampoline:	call	start_of_setup
		.space	1024

start_of_setup:
	movw	$0x01500, %ax
	movb	$0x81, %dl
	int	$0x13

	movw	%cs, %ax
	movw	%ax, %ds
	cmpw	$SIG1, setup_sig1
	jne	bad_sig

	cmpw	$SIG2, setup_sig2
	jne	bad_sig

	jmp	good_sig1

prtstr:
	lodsb
	andb	%al, %al
	jz	fin

	call	prtchr
	jmp	prtstr

fin:	ret

prtsp2:	call	prtspc
prtspc:	movb	$0x20, %al

prtchr:	pushw	%ax
	pushw	%cx
	xorb	%bh, %bh
	movw	$0x01, %cx
	movb	$0x0e, %ah
	int	$0x10
	popw	%cx
	popw	%ax
	ret

beep:	movb	$0x07, %al
	jmp	prtchr
	
no_sig_mess: .string	"No setup signature found ..."

good_sig1:
	jmp	good_sig

bad_sig:
	movw	%cs, %ax
	subw	$DELTA_INITSEG, %ax	
	movw	%ax, %ds
	xorb	%bh, %bh
	movb	(497), %bl
	subw	$4, %bx	
	shlw	$8, %bx	
	movw	%bx, %cx
	shrw	$3, %bx	
	addw	$SYSSEG, %bx
	movw	%bx, %cs:start_sys_seg

	movw	$2048, %di
	subw	%si, %si
	pushw	%cs
	popw	%es
	movw	$SYSSEG, %ax
	movw	%ax, %ds
	rep
	movsw
	movw	%cs, %ax
	movw	%ax, %ds
	cmpw	$SIG1, setup_sig1
	jne	no_sig

	cmpw	$SIG2, setup_sig2
	jne	no_sig

	jmp	good_sig

no_sig:
	lea	no_sig_mess, %si
	call	prtstr

no_sig_loop:
	hlt
	jmp	no_sig_loop

good_sig:
	movw	%cs, %ax
	subw	$DELTA_INITSEG, %ax 
	movw	%ax, %ds
	testb	$LOADED_HIGH, %cs:loadflags	
	jz	loader_ok

	cmpb	$0, %cs:type_of_loader 
	jnz	loader_ok

	pushw	%cs	
	popw	%ds	
	lea	loader_panic_mess, %si
	call	prtstr

	jmp	no_sig_loop

loader_panic_mess: .string "Wrong loader, giving up..."

loader_ok:
	xorl	%eax, %eax
	movl	%eax, (0x1e0)

	movb	%al, (E820NR)

#define SMAP  0x534d4150

meme820:
	xorl	%ebx, %ebx
	movw	$E820MAP, %di

jmpe820:
	movl	$0x0000e820, %eax
	movl	$SMAP, %edx	
	movl	$20, %ecx
	pushw	%ds	
	popw	%es
	int	$0x15
	jc	bail820	

	cmpl	$SMAP, %eax	
	jne	bail820	

good820:
	movb	(E820NR), %al
	cmpb	$E820MAX, %al
	jnl	bail820

	incb	(E820NR)
	movw	%di, %ax
	addw	$20, %ax
	movw	%ax, %di
again820:
	cmpl	$0, %ebx
	jne	jmpe820	
bail820:

	movw	$0x0305, %ax
	xorw	%bx, %bx
	int	$0x16

	call	video

	xorw	%ax, %ax
	movw	%ax, %ds
	ldsw	(4 * 0x41), %si
	movw	%cs, %ax
	subw	$DELTA_INITSEG, %ax	
	pushw	%ax
	movw	%ax, %es
	movw	$0x0080, %di
	movw	$0x10, %cx
	pushw	%cx
	cld
	rep
 	movsb

	xorw	%ax, %ax
	movw	%ax, %ds
	ldsw	(4 * 0x46), %si
	popw	%cx
	popw	%es
	movw	$0x0090, %di
	rep
	movsb

	movw	$0x01500, %ax
	movb	$0x81, %dl
	int	$0x13
	jc	no_disk1
	
	cmpb	$3, %ah
	je	is_disk1

no_disk1:
	movw	%cs, %ax
	subw	$DELTA_INITSEG, %ax 
	movw	%ax, %es
	movw	$0x0090, %di
	movw	$0x10, %cx
	xorw	%ax, %ax
	cld
	rep
	stosb
is_disk1:
	movw	%cs, %ax
	subw	$DELTA_INITSEG, %ax	
	movw	%ax, %ds
	xorw	%ax, %ax
	movw	%ax, (0xa0)	
	movb	$0xc0, %ah
	stc
	int	$0x15
	jc	no_mca

no_mca:
	movw	%cs, %ax
	subw	$DELTA_INITSEG, %ax	
	movw	%ax, %ds
	movw	$0, (0x1ff)	
	int	$0x11
	testb	$0x04, %al
	jz	no_psmouse

	movw	$0xAA, (0x1ff)
no_psmouse:
	movw	$0, 0x40
	movw	$0x05300, %ax
	xorw	%bx, %bx
	int	$0x15
	jc	done_apm_bios
	
	cmpw	$0x0504d, %bx
	jne	done_apm_bios

	andw	$0x02, %cx
	je	done_apm_bios

	movw	$0x05304, %ax
	xorw	%bx, %bx
	int	$0x15
	movw	$0x05303, %ax
	xorl	%ebx, %ebx
	xorw	%cx, %cx
	xorw	%dx, %dx
	xorl	%esi, %esi
	xorw	%di, %di
	int	$0x15
	jc	no_32_apm_bios

	movw	%ax,  (66)
	movl	%ebx, (68)
	movw	%cx,  (72)
	movw	%dx,  (74)
	movl	%esi, (78)
	movw	%di,  (82)
	movw	$0x05300, %ax
	xorw	%bx, %bx
	xorw	%cx, %cx
	int	$0x15
	jc	apm_disconnect

	cmpw	$0x0504d, %bx
	jne	apm_disconnect

	movw	%ax, (64)
	movw	%cx, (76)
	jmp	done_apm_bios

apm_disconnect:	
	movw	$0x05304, %ax
	xorw	%bx, %bx
	int	$0x15

	jmp	done_apm_bios

no_32_apm_bios:
	andw	$0xfffd, (76)
done_apm_bios:
	cmpw	$0, %cs:realmode_swtch
	jz	rmodeswtch_normal

	lcall	%cs:realmode_swtch

	jmp	rmodeswtch_end

rmodeswtch_normal:
        pushw	%cs
	call	default_switch

rmodeswtch_end:
	movl	%cs:code32_start, %eax
	movl	%eax, %cs:code32

	testb	$LOADED_HIGH, %cs:loadflags
	jz	do_move0
	jmp	end_move

do_move0:
	movw	$0x100, %ax	
	movw	%cs, %bp
	subw	$DELTA_INITSEG, %bp	
	movw	%cs:start_sys_seg, %bx
	cld
do_move:
	movw	%ax, %es
	incb	%ah	
	movw	%bx, %ds
	addw	$0x100, %bx
	subw	%di, %di
	subw	%si, %si
	movw 	$0x800, %cx
	rep
	movsw
	cmpw	%bp, %bx
	jb	do_move

end_move:
	movw	%cs, %ax
	movw	%ax, %ds
		
	cmpl	$0, cmd_line_ptr
	jne	end_move_self
	cmpb	$0x20, type_of_loader
	je	end_move_self

	movw	%cs, %ax
	cmpw	$SETUPSEG, %ax
	je	end_move_self

	cli	
	subw	$DELTA_INITSEG, %ax	
	movw	%ss, %dx
	cmpw	%ax, %dx
	jb	move_self_1

	addw	$INITSEG, %dx
	subw	%ax, %dx
move_self_1:
	movw	%ax, %ds
	movw	$INITSEG, %ax
	movw	%ax, %es
	movw	%cs:setup_move_size, %cx
	std	

	movw	%cx, %di
	decw	%di
	movw	%di, %si
	subw	$move_self_here+0x200, %cx
	rep
	movsb
	ljmp	$SETUPSEG, $move_self_here

move_self_here:
	movw	$move_self_here+0x200, %cx
	rep
	movsb
	movw	$SETUPSEG, %ax
	movw	%ax, %ds
	movw	%dx, %ss
end_move_self:

A20_TEST_LOOPS		=  32
A20_ENABLE_LOOPS	= 255

a20_try_loop:

a20_kbc:
	call	empty_8042
	call	a20_test
	jnz	a20_done

	movb	$0xD1, %al
	outb	%al, $0x64
	call	empty_8042

	movb	$0xDF, %al
	outb	%al, $0x60
	call	empty_8042

a20_kbc_wait:
	xorw	%cx, %cx
a20_kbc_wait_loop:
	call	a20_test
	jnz	a20_done
	loop	a20_kbc_wait_loop

a20_die:
	hlt
	jmp	a20_die

a20_tries:
	.byte	A20_ENABLE_LOOPS

a20_err_msg:
	.ascii	"linux: fatal error: A20 gate not responding!"
	.byte	13, 10, 0

a20_done:
	lidt	idt_48
	xorl	%eax, %eax
	movw	%ds, %ax
	shll	$4, %eax
	addl	$gdt, %eax
	movl	%eax, (gdt_48+2)
	lgdt	gdt_48

	xorw	%ax, %ax
	outb	%al, $0xf0
	call	delay

	outb	%al, $0xf1
	call	delay

	movb	$0xFF, %al
	outb	%al, $0xA1
	call	delay
	
	movb	$0xFB, %al
	outb	%al, $0x21

	movw	$1, %ax	
	lmsw	%ax	
	jmp	flush_instr

flush_instr:
	xorw	%bx, %bx
	xorl	%esi, %esi
	movw	%cs, %si
	subw	$DELTA_INITSEG, %si
	shll	$4, %esi

	.byte 0x66, 0xea
code32:	.long	0x1000
	.word	__KERNEL_CS

kernel_version:	.ascii	UTS_RELEASE
		.ascii	" ("
		.ascii	LINUX_COMPILE_BY
		.ascii	"@"
		.ascii	LINUX_COMPILE_HOST
		.ascii	") "
		.ascii	UTS_VERSION
		.byte	0

default_switch:
	cli	
	movb	$0x80, %al
	outb	%al, $0x70
	lret

bootsect_helper:
	cmpw	$0, %cs:bootsect_es
	jnz	bootsect_second

	movb	$0x20, %cs:type_of_loader
	movw	%es, %ax
	shrw	$4, %ax
	movb	%ah, %cs:bootsect_src_base+2
	movw	%es, %ax
	movw	%ax, %cs:bootsect_es
	subw	$SYSSEG, %ax
	lret

bootsect_second:
	pushw	%cx
	pushw	%si
	pushw	%bx
	testw	%bx, %bx
	jne	bootsect_ex

	movw	$0x8000, %cx
	pushw	%cs
	popw	%es
	movw	$bootsect_gdt, %si
	movw	$0x8700, %ax
	int	$0x15
	jc	bootsect_panic

	movw	%cs:bootsect_es, %es
	incb	%cs:bootsect_dst_base+2	
bootsect_ex:
	movb	%cs:bootsect_dst_base+2, %ah
	shlb	$4, %ah	
	xorb	%al, %al
	popw	%bx
	popw	%si
	popw	%cx
	lret

bootsect_gdt:
	.word	0, 0, 0, 0
	.word	0, 0, 0, 0

bootsect_src:
	.word	0xffff

bootsect_src_base:
	.byte	0x00, 0x00, 0x01
	.byte	0x93
	.word	0

bootsect_dst:
	.word	0xffff

bootsect_dst_base:
	.byte	0x00, 0x00, 0x10
	.byte	0x93
	.word	0
	.word	0, 0, 0, 0
	.word	0, 0, 0, 0

bootsect_es:
	.word	0

bootsect_panic:
	pushw	%cs
	popw	%ds
	cld
	leaw	bootsect_panic_mess, %si
	call	prtstr
	
bootsect_panic_loop:
	jmp	bootsect_panic_loop

bootsect_panic_mess:
	.string	"INT15 refuses to access high mem, giving up."

A20_TEST_ADDR = 4*0x80

a20_test:
	pushw	%cx
	pushw	%ax
	xorw	%cx, %cx
	movw	%cx, %fs
	decw	%cx
	movw	%cx, %gs
	movw	$A20_TEST_LOOPS, %cx
	movw	%fs:(A20_TEST_ADDR), %ax
	pushw	%ax
a20_test_wait:
	incw	%ax
	movw	%ax, %fs:(A20_TEST_ADDR)
	call	delay
	cmpw	%gs:(A20_TEST_ADDR+0x10), %ax
	loope	a20_test_wait

	popw	%fs:(A20_TEST_ADDR)
	popw	%ax
	popw	%cx
	ret	

empty_8042:
	pushl	%ecx
	movl	$100000, %ecx

empty_8042_loop:
	decl	%ecx
	jz	empty_8042_end_loop

	call	delay

	inb	$0x64, %al
	testb	$1, %al	
	jz	no_output

	call	delay
	inb	$0x60, %al
	jmp	empty_8042_loop

no_output:
	testb	$2, %al	
	jnz	empty_8042_loop	
empty_8042_end_loop:
	popl	%ecx
	ret

gettime:
	pushw	%cx
	movb	$0x02, %ah
	int	$0x1a
	movb	%dh, %al
	andb	$0x0f, %al
	movb	%dh, %ah
	movb	$0x04, %cl
	shrb	%cl, %ah
	aad
	popw	%cx
	ret

delay:
	outb	%al,$0x80
	ret

gdt:
	.word	0, 0, 0, 0
	.word	0, 0, 0, 0
	.word	0xFFFF
	.word	0
	.word	0x9A00
	.word	0x00CF
	.word	0xFFFF
	.word	0
	.word	0x9200
	.word	0x00CF
idt_48:
	.word	0
	.word	0, 0
gdt_48:
	.word	0x8000
	.word	0, 0

#include "video.S"

setup_sig1:	.word	SIG1
setup_sig2:	.word	SIG2

modelist:

.text
endtext:
.data
enddata:
.bss
endbss:
