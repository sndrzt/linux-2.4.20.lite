#include <linux/config.h>

#undef CONFIG_VIDEO_SVGA

#define CONFIG_VIDEO_VESA
#define CONFIG_VIDEO_COMPACT
#define CONFIG_VIDEO_RETAIN
#undef CONFIG_VIDEO_LOCAL
#undef CONFIG_VIDEO_400_HACK
#undef CONFIG_VIDEO_GFX_HACK
#define VIDEO_GFX_BIOS_AX 0x4f02
#define VIDEO_GFX_BIOS_BX 0x0102
#define VIDEO_GFX_DUMMY_RESOLUTION 0x6425
#define VIDEO_FIRST_MENU 0x0000
#define VIDEO_FIRST_BIOS 0x0100
#define VIDEO_FIRST_VESA 0x0200
#define VIDEO_FIRST_V7 0x0900
#define VIDEO_FIRST_SPECIAL 0x0f00
#define VIDEO_80x25 0x0f00
#define VIDEO_8POINT 0x0f01
#define VIDEO_80x43 0x0f02
#define VIDEO_80x28 0x0f03
#define VIDEO_CURRENT_MODE 0x0f04
#define VIDEO_80x30 0x0f05
#define VIDEO_80x34 0x0f06
#define VIDEO_80x60 0x0f07
#define VIDEO_GFX_HACK 0x0f08
#define VIDEO_LAST_SPECIAL 0x0f09
#define VIDEO_FIRST_RESOLUTION 0x1000
#define VIDEO_RECALC 0x8000

#define PARAM_CURSOR_POS	0x00
#define PARAM_VIDEO_PAGE	0x04
#define PARAM_VIDEO_MODE	0x06
#define PARAM_VIDEO_COLS	0x07
#define PARAM_VIDEO_EGA_BX	0x0a
#define PARAM_VIDEO_LINES	0x0e
#define PARAM_HAVE_VGA		0x0f
#define PARAM_FONT_POINTS	0x10

#define PARAM_LFB_WIDTH		0x12
#define PARAM_LFB_HEIGHT	0x14
#define PARAM_LFB_DEPTH		0x16
#define PARAM_LFB_BASE		0x18
#define PARAM_LFB_SIZE		0x1c
#define PARAM_LFB_LINELENGTH	0x24
#define PARAM_LFB_COLORS	0x26
#define PARAM_VESAPM_SEG	0x2e
#define PARAM_VESAPM_OFF	0x30
#define PARAM_LFB_PAGES		0x32

#ifdef CONFIG_VIDEO_RETAIN
#define DO_STORE call store_screen
#else
#define DO_STORE
#endif /* CONFIG_VIDEO_RETAIN */

video:	pushw	%ds
	pushw	%ds
	popw	%fs
	pushw	%cs
	popw	%ds
	pushw	%cs
	popw	%es
	xorw	%ax, %ax
	movw	%ax, %gs
	cld
	call	basic_detect
#ifdef CONFIG_VIDEO_SELECT
	movw	%fs:(0x01fa), %ax
	cmpw	$ASK_VGA, %ax
	jz	vid2

	call	mode_set
	jc	vid1

	leaw	badmdt, %si
	call	prtstr
vid2:	call	mode_menu
vid1:
#ifdef CONFIG_VIDEO_RETAIN
	call	restore_screen
#endif
#endif
	call	mode_params
	popw	%ds
	ret

basic_detect:
	movb	$0, %fs:(PARAM_HAVE_VGA)
	movb	$0x12, %ah	# Check EGA/VGA
	movb	$0x10, %bl
	int	$0x10
	movw	%bx, %fs:(PARAM_VIDEO_EGA_BX)
	cmpb	$0x10, %bl
	je	basret

	incb	adapter
	movw	$0x1a00, %ax
	int	$0x10
	cmpb	$0x1a, %al
	jne	basret
	
	incb	%fs:(PARAM_HAVE_VGA)
	incb	adapter
basret:	ret

mode_params:
#ifdef CONFIG_VIDEO_SELECT
	cmpb	$0, graphic_mode
	jnz	mopar_gr
#endif
	movb	$0x03, %ah
	xorb	%bh, %bh
	int	$0x10
	movw	%dx, %fs:(PARAM_CURSOR_POS)
	movb	$0x0f, %ah
	int	$0x10
	movw	%bx, %fs:(PARAM_VIDEO_PAGE)
	movw	%ax, %fs:(PARAM_VIDEO_MODE)
	cmpb	$0x7, %al
	jnz	mopar0

	movw	$0xb000, video_segment
mopar0: movw	%gs:(0x485), %ax
	movw	%ax, %fs:(PARAM_FONT_POINTS)
	movw	force_size, %ax
	orw	%ax, %ax
	jz	mopar1

	movb	%ah, %fs:(PARAM_VIDEO_COLS)
	movb	%al, %fs:(PARAM_VIDEO_LINES)
	ret

mopar1:	movb	$25, %al
	cmpb	$0, adapter
	jz	mopar2

	movb	%gs:(0x484), %al
	incb	%al
mopar2: movb	%al, %fs:(PARAM_VIDEO_LINES)
	ret

#ifdef CONFIG_VIDEO_SELECT
mopar_gr:
	leaw	modelist+1024, %di
	movb	$0x23, %fs:(PARAM_HAVE_VGA)
	movw	16(%di), %ax
	movw	%ax, %fs:(PARAM_LFB_LINELENGTH)
	movw	18(%di), %ax
	movw	%ax, %fs:(PARAM_LFB_WIDTH)
	movw	20(%di), %ax
	movw	%ax, %fs:(PARAM_LFB_HEIGHT)
	movb	25(%di), %al
	movb	$0, %ah
	movw	%ax, %fs:(PARAM_LFB_DEPTH)
	movb	29(%di), %al	
	movb	$0, %ah
	movw	%ax, %fs:(PARAM_LFB_PAGES)
	movl	40(%di), %eax
	movl	%eax, %fs:(PARAM_LFB_BASE)
	movl	31(%di), %eax
	movl	%eax, %fs:(PARAM_LFB_COLORS)
	movl	35(%di), %eax
	movl	%eax, %fs:(PARAM_LFB_COLORS+4)

	leaw	modelist+1024, %di
	movw	$0x4f00, %ax
	int	$0x10
	xorl	%eax, %eax
	movw	18(%di), %ax
	movl	%eax, %fs:(PARAM_LFB_SIZE)

	movw	$0x4f0a, %ax
	xorw	%bx, %bx
	xorw	%di, %di
	int	$0x10
	cmp	$0x004f, %ax
	jnz	no_pm

	movw	%es, %fs:(PARAM_VESAPM_SEG)
	movw	%di, %fs:(PARAM_VESAPM_OFF)
no_pm:	ret

mode_menu:
	leaw	keymsg, %si
	call	prtstr
	call	flush
nokey:	call	getkt

	cmpb	$0x0d, %al
	je	listm

	cmpb	$0x20, %al
	je	defmd1

	call 	beep
	jmp	nokey

defmd1:	ret

listm:	call	mode_table
listm0:	leaw	name_bann, %si
	call	prtstr
	movw	card_name, %si
	orw	%si, %si
	jnz	an2

	movb	adapter, %al
	leaw	old_name, %si
	orb	%al, %al
	jz	an1

	leaw	ega_name, %si
	decb	%al
	jz	an1

	leaw	vga_name, %si
	jmp	an1

an2:	call	prtstr
	leaw	svga_name, %si
an1:	call	prtstr
	leaw	listhdr, %si
	call	prtstr
	movb	$0x30, %dl
	leaw	modelist, %si
lm1:	cmpw	$ASK_VGA, (%si)
	jz	lm2

	movb	%dl, %al
	call	prtchr
	call	prtsp2
	lodsw
	call	prthw
	call	prtsp2
	movb	0x1(%si), %al
	call	prtdec
	movb	$0x78, %al
	call	prtchr
	lodsw
	call	prtdec
	movb	$0x0d, %al
	call	prtchr
	movb	$0x0a, %al
	call	prtchr
	incb	%dl
	cmpb	$0x3a, %dl
	jnz	lm1

	movb	$0x61, %dl
	jmp	lm1

lm2:	leaw	prompt, %si
	call	prtstr
	leaw	edit_buf, %di
lm3:	call	getkey
	cmpb	$0x0d, %al
	jz	lment

	cmpb	$0x08, %al
	jz	lmbs

	cmpb	$0x20, %al
	jc	lm3

	cmpw	$edit_buf+4, %di
	jz	lm3

	stosb
	call	prtchr
	jmp	lm3

lmbs:	cmpw	$edit_buf, %di
	jz	lm3

	decw	%di
	movb	$0x08, %al
	call	prtchr
	call	prtspc
	movb	$0x08, %al
	call	prtchr
	jmp	lm3
	
lment:	movb	$0, (%di)
	leaw	crlft, %si
	call	prtstr
	leaw	edit_buf, %si
	cmpb	$0, (%si)
	jz	lmdef

	cmpb	$0, 1(%si)
	jz	mnusel

	cmpw	$0x6373, (%si)
	jnz	lmhx

	cmpw	$0x6e61, 2(%si)
	jz	lmscan

lmhx:	xorw	%bx, %bx
lmhex:	lodsb
	orb	%al, %al
	jz	lmuse1

	subb	$0x30, %al
	jc	lmbad

	cmpb	$10, %al
	jc	lmhx1

	subb	$7, %al
	andb	$0xdf, %al
	cmpb	$10, %al
	jc	lmbad

	cmpb	$16, %al
	jnc	lmbad

lmhx1:	shlw	$4, %bx
	orb	%al, %bl
	jmp	lmhex

lmuse1:	movw	%bx, %ax
	jmp	lmuse

mnusel:	lodsb
	xorb	%ah, %ah
	subb	$0x30, %al
	jc	lmbad

	cmpb	$10, %al
	jc	lmuse
	
	cmpb	$0x61-0x30, %al
	jc	lmbad
	
	subb	$0x61-0x30-10, %al
	cmpb	$36, %al
	jnc	lmbad

lmuse:	call	mode_set
	jc	lmdef

lmbad:	leaw	unknt, %si
	call	prtstr
	jmp	lm2
lmscan:	cmpb	$0, adapter
	jz	lmbad

	movw	$0, mt_end
	movb	$1, scanning
	call	mode_table
	jmp	listm0
lmdef:	ret

setv7:
	DO_STORE
	subb	$VIDEO_FIRST_V7>>8, %bh
	movw	$0x6f05, %ax
	int	$0x10
	stc
	ret

_setrec:	jmp	setrec
_set_80x25:	jmp	set_80x25

setalias:
	movw	$VIDEO_80x25, %ax
	incw	%bx
	jz	mode_set

	movb	$VIDEO_8POINT-VIDEO_FIRST_SPECIAL, %al
	incw	%bx
	jnz	setbad				# Fall-through!

mode_set:
	movw	%ax, %bx
	cmpb	$0xff, %ah
	jz	setalias

	testb	$VIDEO_RECALC>>8, %ah
	jnz	_setrec

	cmpb	$VIDEO_FIRST_RESOLUTION>>8, %ah
	jnc	setres
	
	cmpb	$VIDEO_FIRST_SPECIAL>>8, %ah
	jz	setspc
	
	cmpb	$VIDEO_FIRST_V7>>8, %ah
	jz	setv7
	
	cmpb	$VIDEO_FIRST_VESA>>8, %ah
	jnc	check_vesa
	
	orb	%ah, %ah
	jz	setmenu
	
	decb	%ah
	jz	setbios

setbad:	clc
	movb	$0, do_restore
	ret

setvesa:
	DO_STORE
	subb	$VIDEO_FIRST_VESA>>8, %bh
	movw	$0x4f02, %ax
	int	$0x10
	cmpw	$0x004f, %ax
	jnz	setbad

	stc
	ret

setbios:
	DO_STORE
	int	$0x10
	pushw	%bx
	movb	$0x0f, %ah
	int	$0x10
	popw	%bx
	cmpb	%bl, %al
	jnz	setbad
	
	stc
	ret

setspc:	xorb	%bh, %bh
	cmpb	$VIDEO_LAST_SPECIAL-VIDEO_FIRST_SPECIAL, %bl
	jnc	setbad
	
	addw	%bx, %bx
	jmp	*spec_inits(%bx)

setmenu:
	orb	%al, %al
	jz	_set_80x25
	
	pushw	%bx
	call	mode_table
	popw	%ax
	shlw	$2, %ax
	addw	%ax, %si
	cmpw	%di, %si
	jnc	setbad
	
	movw	(%si), %ax
_m_s:	jmp	mode_set

setres:	pushw	%bx
	call	mode_table
	popw	%bx
	xchgb	%bl, %bh
setr1:	lodsw
	cmpw	$ASK_VGA, %ax
	jz	setbad
	
	lodsw
	cmpw	%bx, %ax
	jnz	setr1
	
	movw	-4(%si), %ax
	jmp	_m_s

check_vesa:
	leaw	modelist+1024, %di
	subb	$VIDEO_FIRST_VESA>>8, %bh
	movw	%bx, %cx
	movw	$0x4f01, %ax
	int	$0x10
	addb	$VIDEO_FIRST_VESA>>8, %bh
	cmpw	$0x004f, %ax
	jnz	setbad

	movb	(%di), %al
	andb	$0x19, %al
	cmpb	$0x09, %al
	jz	setvesa

	movb	(%di), %al
	andb	$0x99, %al
	cmpb	$0x99, %al
	jnz	_setbad

	subb	$VIDEO_FIRST_VESA>>8, %bh
	orw	$0x4000, %bx
	movw	$0x4f02, %ax
	int	$0x10
	cmpw	$0x004f, %ax
	jnz	_setbad

	movb	$1, graphic_mode
	movb	$0, do_restore
	stc
	ret

_setbad:	jmp	setbad

setrec:	subb	$VIDEO_RECALC>>8, %ah
	call	mode_set
	jnc	rct3

	movw	%gs:(0x485), %ax
	movb	%gs:(0x484), %bl
	incb	%bl
	mulb	%bl
	decw	%ax
	movw	$0x3d4, %dx
	movw	%ax, %bx
	movb	$0x12, %al
	movb	%bl, %ah
	outw	%ax, %dx
	movb	$0x07, %al
	call	inidx
	xchgb	%al, %ah
	andb	$0xbd, %ah
	shrb	%bh
	jnc	rct1
	orb	$0x02, %ah
rct1:	shrb	%bh
	jnc	rct2
	orb	$0x40, %ah
rct2:	movb	$0x07, %al
	outw	%ax, %dx
	stc
rct3:	ret

spec_inits:
	.word	set_80x25
	.word	set_8pixel
	.word	set_80x43
	.word	set_80x28
	.word	set_current
	.word	set_80x30
	.word	set_80x34
	.word	set_80x60
	.word	set_gfx

set_80x25:
	movw	$0x5019, force_size
use_80x25:
#ifdef CONFIG_VIDEO_400_HACK
	movw	$0x1202, %ax
	movb	$0x30, %bl
	int	$0x10
#else
	movb	$0x0f, %ah
	int	$0x10
	cmpw	$0x5007, %ax
	jz	st80

	cmpw	$0x5003, %ax
	jnz	force3

st80:	cmpb	$0, adapter
	jz	set80

	movb	%gs:(0x0484), %al
	orb	%al, %al
	jz	set80
	
	cmpb	$24, %al
	jz	set80
#endif
force3:	DO_STORE
	movw	$0x0003, %ax
	int	$0x10
set80:	stc
	ret

set_8pixel:
	DO_STORE
	call	use_80x25
set_8pt:
	movw	$0x1112, %ax
	xorb	%bl, %bl
	int	$0x10
	movw	$0x1200, %ax
	movb	$0x20, %bl
	int	$0x10
	movw	$0x1201, %ax
	movb	$0x34, %bl
	int	$0x10
	movb	$0x01, %ah
	movw	$0x0607, %cx
	int	$0x10
set_current:
	stc
	ret

set_80x28:
	DO_STORE
	call	use_80x25
set14:	movw	$0x1111, %ax
	xorb	%bl, %bl
	int	$0x10
	movb	$0x01, %ah
	movw	$0x0b0c, %cx
	int	$0x10
	stc
	ret

set_80x43:
	DO_STORE
	movw	$0x1201, %ax
	movb	$0x30, %bl
	int	$0x10
	movw	$0x0003, %ax
	int	$0x10
	jmp	set_8pt

set_80x30:
	call	use_80x25
	DO_STORE
	movw	$0x3cc, %dx
	inb	%dx, %al
	movb	$0xd4, %dl
	rorb	%al
	jc	set48a

	movb	$0xb4, %dl
set48a:	movw	$0x0c11, %ax
 	call	outidx
	movw	$0x0b06, %ax
 	call	outidx
	movw	$0x3e07, %ax
 	call	outidx
	movw	$0xea10, %ax
 	call	outidx
	movw	$0xdf12, %ax
	call	outidx
	movw	$0xe715, %ax
 	call	outidx
	movw	$0x0416, %ax
 	call	outidx
	pushw	%dx
	movb	$0xcc, %dl
 	inb	%dx, %al
 	movb	$0xc2, %dl
 	andb	$0x0d, %al
 	orb	$0xe2, %al
 	outb	%al, %dx
	popw	%dx
	movw	$0x501e, force_size
	stc
	ret

set_80x34:
	call	set_80x30
	call	set14
	movw	$0xdb12, %ax
	movw	$0x5022, force_size
setvde:	call	outidx
	stc
	ret

set_80x60:
	call	set_80x30
	call	set_8pt	
	movw	$0xdf12, %ax
	movw	$0x503c, force_size
	jmp	setvde

set_gfx:
#ifdef CONFIG_VIDEO_GFX_HACK
	movw	$VIDEO_GFX_BIOS_AX, %ax
	movw	$VIDEO_GFX_BIOS_BX, %bx
	int	$0x10
	movw	$VIDEO_GFX_DUMMY_RESOLUTION, force_size
	stc
#endif
	ret

#ifdef CONFIG_VIDEO_RETAIN

store_screen:
	cmpb	$0, do_restore
	jnz	stsr

	testb	$CAN_USE_HEAP, loadflags
	jz	stsr
	
	pushw	%ax
	pushw	%bx
	pushw	force_size
	movw	$0, force_size
	call	mode_params
	popw	force_size
	movb	%fs:(PARAM_VIDEO_LINES), %ah
	movb	%fs:(PARAM_VIDEO_COLS), %al
	movw	%ax, %bx
	mulb	%ah
	movw	%ax, %cx
	addw	%ax, %ax
	addw	$modelist+1024+4, %ax
	cmpw	heap_end_ptr, %ax
	jnc	sts1

	movw	%fs:(PARAM_CURSOR_POS), %ax
	leaw	modelist+1024, %di
	stosw
	movw	%bx, %ax
	stosw
	pushw	%ds
	movw	video_segment, %ds
	xorw	%si, %si
	rep
	movsw
	popw	%ds
	incb	do_restore
sts1:	popw	%bx
	popw	%ax
stsr:	ret

restore_screen:
	cmpb	$0, do_restore
	jz	res1

	call	mode_params
	movb	%fs:(PARAM_VIDEO_LINES), %cl
	movb	%fs:(PARAM_VIDEO_COLS), %ch
	leaw	modelist+1024, %si
	lodsw
	movw	%ax, %dx
	cmpb	%cl, %dh
	jc	res2
	
	movb	%cl, %dh
	decb	%dh
res2:	cmpb	%ch, %dl
	jc	res3
	
	movb	%ch, %dl
	decb	%dl
res3:	movb	$0x02, %ah
	movb	$0x00, %bh
	int	$0x10
	lodsw
	movb	%ah, %dl
	movb	$0, %ah
	movw	%ax, %bx
	cmpb	%cl, %dl
	jc	res4

	pushw	%ax
	movb	%dl, %al
	subb	%cl, %al
	mulb	%bl
	addw	%ax, %si
	addw	%ax, %si
	popw	%ax
	movb	%cl, %dl
res4:	cmpb	%ch, %al
	jc	res5
	
	movb	%ch, %al
res5:	movb	$0, %cl
	xchgb	%ch, %cl
	movw	%cx, %bp
	pushw	%es
	movw	video_segment, %es
	xorw	%di, %di
	addw	%bx, %bx
	addw	%bp, %bp
res6:	pushw	%si
	pushw	%di
	movw	%ax, %cx
	rep
	movsw
	popw	%di
	popw	%si
	addw	%bp, %di
	addw	%bx, %si
	decb	%dl
	jnz	res6
	
	popw	%es				# Done
res1:	ret
#endif /* CONFIG_VIDEO_RETAIN */

outidx:	outb	%al, %dx
	pushw	%ax
	movb	%ah, %al
	incw	%dx
	outb	%al, %dx
	decw	%dx
	popw	%ax
	ret

mode_table:
	movw	mt_end, %di
	orw	%di, %di
	jnz	mtab1x
	
	leaw	modelist, %di
	movl	$VIDEO_80x25 + 0x50190000, %eax
	stosl
	movb	adapter, %al
	orb	%al, %al
	jz	mtabe
	
	decb	%al
	jnz	mtabv
	
	movl	$VIDEO_8POINT + 0x502b0000, %eax
	stosl
	jmp	mtabe

mtab1x:	jmp	mtab1

mtabv:	leaw	vga_modes, %si
	movw	$vga_modes_end-vga_modes, %cx
	rep
	movsb

	cmpb	$0, scanning
	jz	mscan1
	
	call	mode_scan
mscan1:

#ifdef CONFIG_VIDEO_LOCAL
	call	local_modes
#endif

#ifdef CONFIG_VIDEO_VESA
	call	vesa_modes
#endif

#ifdef CONFIG_VIDEO_SVGA
	cmpb	$0, scanning
	jnz	mscan2
	
	call	svga_modes
mscan2:
#endif

mtabe:

#ifdef CONFIG_VIDEO_COMPACT
	leaw	modelist, %si
	movw	%di, %dx
	movw	%si, %di
cmt1:	cmpw	%dx, %si
	jz	cmt2

	leaw	modelist, %bx
	movw	2(%si), %cx
cmt3:	cmpw	%bx, %si
	jz	cmt4

	cmpw	2(%bx), %cx
	jz	cmt5

	addw	$4, %bx
	jmp	cmt3

cmt4:	movsl
	jmp	cmt1

cmt5:	addw	$4, %si
	jmp	cmt1

cmt2:
#endif

	movw	$ASK_VGA, (%di)
	movw	%di, mt_end
mtab1:	leaw	modelist, %si
ret0:	ret

vga_modes:
	.word	VIDEO_8POINT
	.word	0x5032
	.word	VIDEO_80x43
	.word	0x502b
	.word	VIDEO_80x28
	.word	0x501c
	.word	VIDEO_80x30
	.word	0x501e
	.word	VIDEO_80x34
	.word	0x5022
	.word	VIDEO_80x60
	.word	0x503c
#ifdef CONFIG_VIDEO_GFX_HACK
	.word	VIDEO_GFX_HACK
	.word	VIDEO_GFX_DUMMY_RESOLUTION
#endif

vga_modes_end:

#ifdef CONFIG_VIDEO_VESA
vesa_modes:
	cmpb	$2, adapter
	jnz	ret0

	movw	%di, %bp
	addw	$0x200, %di
	movw	$0x4f00, %ax
	int	$0x10
	movw	%bp, %di
	cmpw	$0x004f, %ax
	jnz	ret0
	
	cmpw	$0x4556, 0x200(%di)
	jnz	ret0
	
	cmpw	$0x4153, 0x202(%di)
	jnz	ret0
	
	movw	$vesa_name, card_name
	pushw	%gs
	lgsw	0x20e(%di), %si
	movw	$128, %cx
vesa1:
	gs; lodsw
	cmpw	$0xffff, %ax
	jz	vesar
	
	cmpw	$0x0080, %ax
	jc	vesa2
	
	orb	%ah, %ah
	jz	vesan

	cmpw	$0x0800, %ax
	jnc	vesae

vesa2:	pushw	%cx
	movw	%ax, %cx
	movw	$0x4f01, %ax
	int	$0x10
	movw	%cx, %bx
	addb	$VIDEO_FIRST_VESA>>8, %bh
	popw	%cx
	cmpw	$0x004f, %ax
	jnz	vesan

	movb	(%di), %al
	andb	$0x19, %al
	cmpb	$0x09, %al
	jnz	vesan
	
	cmpw	$0xb800, 8(%di)
	jnz	vesan

	testb	$2, (%di)
	movw	%bx, (%di)
	jz	vesa3
	
	xorw	%dx, %dx
	movw	0x12(%di), %bx
	orb	%bh, %bh
	jnz	vesan
	
	movb	%bl, 0x3(%di)
	movw	0x14(%di), %ax
	orb	%ah, %ah
	jnz	vesan
	
	movb	%al, 2(%di)
	mulb	%bl
	cmpw	$8193, %ax
	jnc	vesan

	jmp	vesaok

vesa3:	subw	$0x8108, %bx
	jc	vesan

	cmpw	$5, %bx
	jnc	vesan

	movw	vesa_text_mode_table(%bx), %ax
	movw	%ax, 2(%di)
vesaok:	addw	$4, %di
vesan:	loop	vesa1
vesae:	leaw	vesaer, %si
	call	prtstr
	movw	%bp, %di
vesar:	popw	%gs
	ret

vesa_text_mode_table:
	.byte	60, 80				# 0108
	.byte	25, 132				# 0109
	.byte	43, 132				# 010A
	.byte	50, 132				# 010B
	.byte	60, 132				# 010C
#endif

mode_scan:
	movw	$0x0100, %cx
scm1:	movb	$0, %ah
	movb	%cl, %al
	int	$0x10
	movb	$0x0f, %ah
	int	$0x10
	cmpb	%cl, %al
	jnz	scm2

	movw	$0x3c0, %dx
	movb	$0x10, %al
	call	inidx
	andb	$0x03, %al
	jnz	scm2
	
	movb	$0xce, %dl
	movb	$0x06, %al
	call	inidx
	shrb	%al
	jc	scm2
	
	movb	$0xd4, %dl
	movb	$0x0f, %al
	call	inidx
	orb	%al, %al
	jnz	scm2
	
	movw	%cx, %ax
	stosw
	movb	%gs:(0x484), %al
	incb	%al
	stosb
	movw	%gs:(0x44a), %ax
	stosb
scm2:	incb	%cl
	jns	scm1
	
	movw	$0x0003, %ax
	int	$0x10
	ret

tstidx:	outw	%ax, %dx
inidx:	outb	%al, %dx
	incw	%dx
	inb	%dx, %al
	decw	%dx
	ret

#ifdef CONFIG_VIDEO_SVGA
svga_modes:
	leaw	svga_table, %si
dosvga:	lodsw
	movw	%ax, %bp
	orw	%ax, %ax
	jz	didsv1

	lodsw
	pushw	%si
	pushw	%di
	pushw	%es
	movw	$0xc000, %bx
	movw	%bx, %es
	call	*%ax
	popw	%es
	popw	%di
	popw	%si
	orw	%bp, %bp
	jz	dosvga
	
	movw	%bp, %si
	movb	svga_prefix, %ah
cpsvga:	lodsb
	orb	%al, %al
	jz	didsv
	
	stosw
	movsw
	jmp	cpsvga

didsv:	movw	%si, card_name
didsv1:	ret

svga_table:
	.word	ati_md, ati_test
	.word	oak_md, oak_test
	.word	paradise_md, paradise_test
	.word	realtek_md, realtek_test
	.word	s3_md, s3_test
	.word	chips_md, chips_test
	.word	video7_md, video7_test
	.word	cirrus5_md, cirrus5_test
	.word	cirrus6_md, cirrus6_test
	.word	cirrus1_md, cirrus1_test
	.word	ahead_md, ahead_test
	.word	everex_md, everex_test
	.word	genoa_md, genoa_test
	.word	trident_md, trident_test
	.word	tseng_md, tseng_test
	.word	0

s3_test:
	movw	$0x0f35, %cx
	movw	$0x03d4, %dx
	movb	$0x38, %al
	call	inidx
	movb	%al, %bh
	movw	$0x0038, %ax
	call	outidx
	movb	%cl, %al
	call	inidx
	movb	%al, %bl
	andb	$0xf0, %al
	movb	%al, %ah
	movb	%cl, %al
	call	outidx
	call	inidx
	andb	%ch, %al
	jz	s3_2

	movb	%bl, %ah
	movb	%cl, %al
	call	outidx
	jmp	s3_1

s3_2:	movw	%cx, %ax
	orb	%bl, %ah
	call	outidx
	call	inidx
	andb	%cl, %al
	pushw	%ax
	movb	%bl, %ah
	movb	%cl, %al
	call	outidx
	popw	%ax
	cmpb	%ch, %al
	je	no_s3

s3_1:	movw	$0x4838, %ax
	call	outidx
	movb	%cl, %al
	call	inidx
	movb	%al, %bl
	andb	$0xf0, %al
	movb	%al, %ah
	movb	%cl, %al
	call	outidx
	call	inidx
	andb	%ch, %al
	jnz	no_s3

	movw	%cx, %ax
	orb	%bl, %ah
	call	outidx
	call	inidx
	andb	%ch, %al
	pushw	%ax
	movb	%bl, %ah
	movb	%cl, %al
	call	outidx
	popw	%ax
	cmpb	%ch, %al
	jne	no_s31
	movb	$0x30, %al
	call	inidx
	leaw	idS3, %di
	movw	$0x10, %cx
	repne
	scasb
	je	no_s31

	movb	%bh, %ah
	movb	$0x38, %al
	jmp	s3rest

no_s3:	movb	$0x35, %al
	movb	%bl, %ah
	call	outidx
no_s31:	xorw	%bp, %bp
s3rest:	movb	%bh, %ah
	movb	$0x38, %al
	jmp	outidx

idS3:	.byte	0x81, 0x82, 0x90, 0x91, 0x92, 0x93, 0x94, 0x95
	.byte	0xa0, 0xa1, 0xa2, 0xa3, 0xa4, 0xa5, 0xa8, 0xb0

s3_md:	.byte	0x54, 0x2b, 0x84
	.byte	0x55, 0x19, 0x84
	.byte	0
	.ascii	"S3"
	.byte	0

ati_test:
	leaw 	idati, %si
	movw	$0x31, %di
	movw	$0x09, %cx
	repe
	cmpsb
	je	atiok

	xorw	%bp, %bp
atiok:	ret

idati:	.ascii	"761295520"

ati_md:	.byte	0x23, 0x19, 0x84
	.byte	0x33, 0x2c, 0x84
	.byte	0x22, 0x1e, 0x64
	.byte	0x21, 0x19, 0x64
	.byte	0x58, 0x21, 0x50
	.byte	0x5b, 0x1e, 0x50
	.byte	0
	.ascii	"ATI"
	.byte	0

ahead_test:
	movw	$0x200f, %ax
	movw	$0x3ce, %dx
	outw	%ax, %dx
	incw	%dx
	inb	%dx, %al
	cmpb	$0x20, %al
	je	isahed

	cmpb	$0x21, %al
	je	isahed
	
	xorw	%bp, %bp
isahed:	ret

ahead_md:
	.byte	0x22, 0x2c, 0x84
	.byte	0x23, 0x19, 0x84
	.byte	0x24, 0x1c, 0x84
	.byte	0x2f, 0x32, 0xa0
	.byte	0x32, 0x22, 0x50
	.byte	0x34, 0x42, 0x50
	.byte	0
	.ascii	"Ahead"
	.byte	0

chips_test:
	movw	$0x3c3, %dx
	inb	%dx, %al
	orb	$0x10, %al
	outb	%al, %dx
	movw	$0x104, %dx
	inb	%dx, %al
	movb	%al, %bl
	movw	$0x3c3, %dx
	inb	%dx, %al
	andb	$0xef, %al
	outb	%al, %dx
	cmpb	$0xa5, %bl
	je	cantok
	
	xorw	%bp, %bp
cantok:	ret

chips_md:
	.byte	0x60, 0x19, 0x84
	.byte	0x61, 0x32, 0x84
	.byte	0
	.ascii	"Chips & Technologies"
	.byte	0

cirrus1_test:
	movw	$0x3d4, %dx
	movb	$0x0c, %al
	outb	%al, %dx
	incw	%dx
	inb	%dx, %al
	movb	%al, %bl
	xorb	%al, %al
	outb	%al, %dx
	decw	%dx
	movb	$0x1f, %al
	outb	%al, %dx
	incw	%dx
	inb	%dx, %al
	movb	%al, %bh
	xorb	%ah, %ah
	shlb	$4, %al
	movw	%ax, %cx
	movb	%bh, %al
	shrb	$4, %al
	addw	%ax, %cx
	shlw	$8, %cx
	addw	$6, %cx
	movw	%cx, %ax
	movw	$0x3c4, %dx
	outw	%ax, %dx
	incw	%dx
	inb	%dx, %al
	andb	%al, %al
	jnz	nocirr
	
	movb	%bh, %al
	outb	%al, %dx
	inb	%dx, %al
	cmpb	$0x01, %al
	je	iscirr

nocirr:	xorw	%bp, %bp
iscirr: movw	$0x3d4, %dx
	movb	%bl, %al
	xorb	%ah, %ah
	shlw	$8, %ax
	addw	$0x0c, %ax
	outw	%ax, %dx
	ret

cirrus1_md:
	.byte	0x1f, 0x19, 0x84
	.byte	0x20, 0x2c, 0x84
	.byte	0x22, 0x1e, 0x84
	.byte	0x31, 0x25, 0x64
	.byte	0
	.ascii	"Cirrus Logic 5X0"
	.byte	0

cirrus5_test:
	movw	$0x3c4, %dx
	movb	$6, %al
	call	inidx
	movb	%al, %bl
	movw	$6, %ax
	call	tstidx
	cmpb	$0x0f, %al
	jne	c5fail
	
	movw	$0x1206, %ax
	call	tstidx
	cmpb	$0x12, %al
	jne	c5fail
	
	movb	$0x1e, %al
	call	inidx
	movb	%al, %bh
	movb	%bh, %ah
	andb	$0xc0, %ah
	movb	$0x1e, %al
	call	tstidx
	andb	$0x3f, %al
	jne	c5xx
	
	movb	$0x1e, %al
	movb	%bh, %ah
	orb	$0x3f, %ah
	call	tstidx
	xorb	$0x3f, %al
	andb	$0x3f, %al
c5xx:	pushf
	movb	$0x1e, %al
	movb	%bh, %ah
	outw	%ax, %dx
	popf
	je	c5done

c5fail:	xorw	%bp, %bp
c5done:	movb	$6, %al
	movb	%bl, %ah
	outw	%ax, %dx
	ret

cirrus5_md:
	.byte	0x14, 0x19, 0x84
	.byte	0x54, 0x2b, 0x84
	.byte	0
	.ascii	"Cirrus Logic 54XX"
	.byte	0

cirrus6_test:
	movw	$0x3ce, %dx
	movb	$0x0a, %al
	call	inidx
	movb	%al, %bl
	movw	$0xce0a, %ax
	call	tstidx
	orb	%al, %al
	jne	c2fail
	
	movw	$0xec0a, %ax
	call	tstidx
	cmpb	$0x01, %al
	jne	c2fail
	
	movb	$0xaa, %al
	call	inidx
	shrb	$4, %al
	subb	$4, %al
	jz	c6done
	
	decb	%al
	jz	c6done
	
	subb	$2, %al
	jz	c6done
	
	decb	%al
	jz	c6done
	
c2fail:	xorw	%bp, %bp
c6done:	movb	$0x0a, %al
	movb	%bl, %ah
	outw	%ax, %dx
	ret

cirrus6_md:
	.byte	0
	.ascii	"Cirrus Logic 64XX"
	.byte	0

everex_test:
	movw	$0x7000, %ax
	xorw	%bx, %bx
	int	$0x10
	cmpb	$0x70, %al
	jne	noevrx
	
	shrw	$4, %dx
	cmpw	$0x678, %dx
	je	evtrid
	
	cmpw	$0x236, %dx
	jne	evrxok

evtrid:	leaw	trident_md, %bp
evrxok:	ret

noevrx:	xorw	%bp, %bp
	ret

everex_md:
	.byte	0x03, 0x22, 0x50
	.byte	0x04, 0x3c, 0x50
	.byte	0x07, 0x2b, 0x64
	.byte	0x08, 0x4b, 0x64
	.byte	0x0a, 0x19, 0x84
	.byte	0x0b, 0x2c, 0x84
	.byte	0x16, 0x1e, 0x50
	.byte	0x18, 0x1b, 0x64
	.byte	0x21, 0x40, 0xa0
	.byte	0x40, 0x1e, 0x84
	.byte	0
	.ascii	"Everex/Trident"
	.byte	0

genoa_test:
	leaw	idgenoa, %si
	xorw	%ax, %ax
	movb	%es:(0x37), %al
	movw	%ax, %di
	movw	$0x04, %cx
	decw	%si
	decw	%di
l1:	incw	%si
	incw	%di
	movb	(%si), %al
	testb	%al, %al
	jz	l2

	cmpb	%es:(%di), %al
l2:	loope 	l1
	orw	%cx, %cx
	je	isgen
	
	xorw	%bp, %bp
isgen:	ret

idgenoa: .byte	0x77, 0x00, 0x99, 0x66

genoa_md:
	.byte	0x58, 0x20, 0x50
	.byte	0x5a, 0x2a, 0x64
	.byte	0x60, 0x19, 0x84
	.byte	0x61, 0x1d, 0x84
	.byte	0x62, 0x20, 0x84
	.byte	0x63, 0x2c, 0x84
	.byte	0x64, 0x3c, 0x84
	.byte	0x6b, 0x4f, 0x64
	.byte	0x72, 0x3c, 0x50
	.byte	0x74, 0x42, 0x50
	.byte	0x78, 0x4b, 0x64
	.byte	0
	.ascii	"Genoa"
	.byte	0

oak_test:
	leaw	idoakvga, %si
	movw	$0x08, %di
	movw	$0x08, %cx
	repe
	cmpsb
	je	isoak
	
	xorw	%bp, %bp
isoak:	ret

idoakvga: .ascii  "OAK VGA "

oak_md: .byte	0x4e, 0x3c, 0x50
	.byte	0x4f, 0x3c, 0x84
	.byte	0x50, 0x19, 0x84
	.byte	0x51, 0x2b, 0x84
	.byte	0
	.ascii	"OAK"
	.byte	0

paradise_test:
	leaw	idparadise, %si
	movw	$0x7d, %di
	movw	$0x04, %cx
	repe
	cmpsb
	je	ispara
	
	xorw	%bp, %bp
ispara:	ret

idparadise:	.ascii	"VGA="

paradise_md:
	.byte	0x41, 0x22, 0x50
	.byte	0x47, 0x1c, 0x84
	.byte	0x55, 0x19, 0x84
	.byte	0x54, 0x2c, 0x84
	.byte	0
	.ascii	"Paradise"
	.byte	0

trident_test:
	movw	$0x3c4, %dx
	movb	$0x0e, %al
	outb	%al, %dx
	incw	%dx
	inb	%dx, %al
	xchgb	%al, %ah
	xorb	%al, %al
	outb	%al, %dx
	inb	%dx, %al
	xchgb	%ah, %al
	movb	%al, %bl
	andb	$0x02, %bl
	jz	setb2
	andb	$0xfd, %al
	jmp	clrb2		

setb2:	orb	$0x02, %al	
clrb2:	outb	%al, %dx
	andb	$0x0f, %ah
	cmpb	$0x02, %ah
	je	istrid

	xorw	%bp, %bp
istrid:	ret

trident_md:
	.byte	0x50, 0x1e, 0x50
	.byte	0x51, 0x2b, 0x50
	.byte	0x52, 0x3c, 0x50
	.byte	0x57, 0x19, 0x84
	.byte	0x58, 0x1e, 0x84
	.byte	0x59, 0x2b, 0x84
	.byte	0x5a, 0x3c, 0x84
	.byte	0
	.ascii	"Trident"
	.byte	0

tseng_test:
	movw	$0x3cd, %dx
	inb	%dx, %al	# Could things be this simple ! :-)
	movb	%al, %bl
	movb	$0x55, %al
	outb	%al, %dx
	inb	%dx, %al
	movb	%al, %ah
	movb	%bl, %al
	outb	%al, %dx
	cmpb	$0x55, %ah
 	je	istsen

isnot:	xorw	%bp, %bp
istsen:	ret

tseng_md:
	.byte	0x26, 0x3c, 0x50
	.byte	0x2a, 0x28, 0x64
	.byte	0x23, 0x19, 0x84
	.byte	0x24, 0x1c, 0x84
	.byte	0x22, 0x2c, 0x84
	.byte	0x21, 0x3c, 0x84
	.byte	0
	.ascii	"Tseng"
	.byte	0

video7_test:
	movw	$0x3cc, %dx
	inb	%dx, %al
	movw	$0x3b4, %dx
	andb	$0x01, %al
	jz	even7

	movw	$0x3d4, %dx
even7:	movb	$0x0c, %al
	outb	%al, %dx
	incw	%dx
	inb	%dx, %al
	movb	%al, %bl
	movb	$0x55, %al
	outb	%al, %dx
	inb	%dx, %al
	decw	%dx
	movb	$0x1f, %al
	outb	%al, %dx
	incw	%dx
	inb	%dx, %al
	movb	%al, %bh
	decw	%dx
	movb	$0x0c, %al
	outb	%al, %dx
	incw	%dx
	movb	%bl, %al
	outb	%al, %dx
	movb	$0x55, %al
	xorb	$0xea, %al
	cmpb	%bh, %al
	jne	isnot
	
	movb	$VIDEO_FIRST_V7>>8, svga_prefix
	ret

video7_md:
	.byte	0x40, 0x2b, 0x50
	.byte	0x43, 0x3c, 0x50
	.byte	0x44, 0x3c, 0x64
	.byte	0x41, 0x19, 0x84
	.byte	0x42, 0x2c, 0x84
	.byte	0x45, 0x1c, 0x84
	.byte	0
	.ascii	"Video 7"
	.byte	0

realtek_test:
	leaw	idrtvga, %si
	movw	$0x45, %di
	movw	$0x0b, %cx
	repe
	cmpsb
	je	isrt
	
	xorw	%bp, %bp
isrt:	ret

idrtvga:	.ascii	"REALTEK VGA"

realtek_md:
	.byte	0x1a, 0x3c, 0x50
	.byte	0x1b, 0x19, 0x84
	.byte	0x1c, 0x1e, 0x84
	.byte	0x1d, 0x2b, 0x84
	.byte	0x1e, 0x3c, 0x84
	.byte	0
	.ascii	"REALTEK"
	.byte	0

#endif

#ifdef CONFIG_VIDEO_LOCAL
local_modes:
	leaw	local_mode_table, %si
locm1:	lodsw
	orw	%ax, %ax
	jz	locm2
	
	stosw
	movsw
	jmp	locm1

locm2:	ret

local_mode_table:
	.word	0x0100
	.byte	25,40
	.word	0
	.ascii	"Local"
	.byte	0
#endif

getkey:	xorb	%ah, %ah
	int	$0x16
	ret

getkt:	call	gettime
	addb	$30, %al
	cmpb	$60, %al
	jl	lminute

	subb	$60, %al
lminute:
	movb	%al, %cl
again:	movb	$0x01, %ah
	int	$0x16
	jnz	getkey

	call	gettime
	cmpb	%cl, %al
	jne	again

	movb	$0x20, %al
	ret

flush:	movb	$0x01, %ah
	int	$0x16
	jz	empty
	
	xorb	%ah, %ah
	int	$0x16
	jmp	flush

empty:	ret

prthw:	pushw	%ax
	movb	%ah, %al
	call	prthb
	popw	%ax
prthb:	pushw	%ax
	shrb	$4, %al
	call	prthn
	popw	%ax
	andb	$0x0f, %al
prthn:	cmpb	$0x0a, %al
	jc	prth1

	addb	$0x07, %al
prth1:	addb	$0x30, %al
	jmp	prtchr

prtdec:	pushw	%ax
	pushw	%cx
	xorb	%ah, %ah
	movb	$0x0a, %cl
	idivb	%cl
	cmpb	$0x09, %al
	jbe	lt100

	call	prtdec
	jmp	skip10

lt100:	addb	$0x30, %al
	call	prtchr
skip10:	movb	%ah, %al
	addb	$0x30, %al
	call	prtchr	
	popw	%cx
	popw	%ax
	ret

mt_end:		.word	0
edit_buf:	.space	6
card_name:	.word	0
scanning:	.byte	0
do_restore:	.byte	0
svga_prefix:	.byte	VIDEO_FIRST_BIOS>>8
graphic_mode:	.byte	0

keymsg:		.ascii	"Press <RETURN> to see video modes available, "
		.ascii	"<SPACE> to continue or wait 30 secs"
		.byte	0x0d, 0x0a, 0

listhdr:	.byte	0x0d, 0x0a
		.ascii	"Mode:    COLSxROWS:"

crlft:		.byte	0x0d, 0x0a, 0

prompt:		.byte	0x0d, 0x0a
		.asciz	"Enter mode number or `scan': "

unknt:		.asciz	"Unknown mode ID. Try again."

badmdt:		.ascii	"You passed an undefined mode number."
		.byte	0x0d, 0x0a, 0

vesaer:		.ascii	"Error: Scanning of VESA modes failed. Please "
		.ascii	"report to <mj@ucw.cz>."
		.byte	0x0d, 0x0a, 0

old_name:	.asciz	"CGA/MDA/HGA"

ega_name:	.asciz	"EGA"

svga_name:	.ascii	" "

vga_name:	.asciz	"VGA"

vesa_name:	.asciz	"VESA"

name_bann:	.asciz	"Video adapter: "
#endif

adapter:	.byte	0	# Video adapter: 0=CGA/MDA/HGA,1=EGA,2=VGA
video_segment:	.word	0xb800
force_size:	.word	0
