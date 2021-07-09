#include <kernel.h>
#include <kdata.h>
#include <printf.h>
#include <stdbool.h>
#include <vt.h>
#include <tty.h>
#include <graphics.h>
#include <vdp1.h>
#include <devtty.h>

#undef  DEBUG			/* UNdefine to delete debug code sequences */

__sfr __at 0x2F tty_debug2;
__sfr __at 0xAA kbd_row_set;
__sfr __at 0xA9 kbd_row_read;

uint8_t keyboard[11][8];
uint8_t shiftkeyboard[11][8];
uint8_t keymap[11];

int8_t vt_twidth = 40;
int8_t vt_tright = 39;

struct vt_repeat keyrepeat;
uint8_t vtattr_cap;

uint8_t inputtty = 1;
uint8_t outputtty = 1;		/* Fixed but needed by the VDP layer */
uint8_t vidmode = 0;		/* Will be needed when we add the mode setting */
uint8_t vswitch;

static char tbuf1[TTYSIZ];
static char tbuf2[TTYSIZ];

struct s_queue ttyinq[NUM_DEV_TTY + 1] = {	/* ttyinq[0] is never used */
	{NULL, NULL, NULL, 0, 0, 0},
	{tbuf1, tbuf1, tbuf1, TTYSIZ, 0, TTYSIZ / 2},
	{tbuf2, tbuf2, tbuf2, TTYSIZ, 0, TTYSIZ / 2}
};

tcflag_t termios_mask[NUM_DEV_TTY + 1] = {
	0,
	_CSYS,
	_CSYS
};


uint8_t vtattr_cap = 0;		/* For now */

/* tty1 is the screen tty2 is the debug port */

/* Output for the system console (kprintf etc) */
void kputchar(char c)
{
	/* Debug port for bringup */
	if (c == '\n')
		tty_putc(2, '\r');
	tty_putc(2, c);
}

/* Both console and debug port are always ready */
ttyready_t tty_writeready(uint8_t minor)
{
	minor;
	return TTY_READY_NOW;
}

void tty_putc(uint8_t minor, unsigned char c)
{
	minor;
	tty_debug2 = c;	
//	if (minor == 1) {
		if (!vswitch)
			vtoutput(&c, 1);
//		return;
//	}
}

int tty_carrier(uint8_t minor)
{
	minor;
	return 1;
}

void tty_setup(uint8_t minor, uint8_t flags)
{
	minor;
}

void tty_sleeping(uint8_t minor)
{
	minor;
}

void tty_data_consumed(uint8_t minor)
{
}

static uint8_t kbd_timer;
static uint8_t keyin[11];
static uint8_t keybyte, keybit;
static uint8_t newkey;
static int keysdown = 0;
static uint8_t shiftmask[11] = {
	0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0
};

static void keyproc(void)
{
	int i;
	uint8_t key;

	for (i = 0; i < 11; i++) {
		key = keyin[i] ^ keymap[i];
		if (key) {
			int n;
			int m = 1;
			for (n = 0; n < 8; n++) {
				if ((key & m) && (keymap[i] & m)) {
					if (!(shiftmask[i] & m))
						keysdown--;
				}
				if ((key & m) && !(keymap[i] & m)) {
					if (!(shiftmask[i] & m)) {
						keysdown++;
						newkey = 1;
						keybyte = i;
						keybit = n;
					}
				}
				m += m;
			}
		}
		keymap[i] = keyin[i];
	}
}

static uint8_t capslock = 0;

static void keydecode(void)
{
	uint8_t c;

	if (keybyte == 6 && keybit == 3) {
		capslock = 1 - capslock;
		return;
	}

	if (keymap[6] & 3 ) {	/* shift or control */
		c = shiftkeyboard[keybyte][keybit];
		/* VT switcher */
#if 0		
		if (c == KEY_F1 || c == KEY_F2 || c == KEY_F3 || c == KEY_F4) {
			if (inputtty != c - KEY_F1) {
				inputtty = c - KEY_F1;
				vtexchange();	/* Exchange the video and backing buffer */
			}
			return;
		}
#endif			
	} else
		c = keyboard[keybyte][keybit];

	if (keymap[6] & 2) {	/* control */
		if (c > 31 && c < 127)
			c &= 31;
	}

	if (capslock && c >= 'a' && c <= 'z')
		c -= 'a' - 'A';

	/* TODO: function keys (F1-F10), graph, code */

	vt_inproc(/*inputtty +*/1, c);
}

void update_keyboard(void)
{
	int n;
	uint8_t r;

	/* encode keyboard row in bits 0-3 0xAA, then read status from 0xA9 */
	for (n =0; n < 11; n++) {
		r = kbd_row_set & 0xf0 | n;
		kbd_row_set = r;
		keyin[n] = ~kbd_row_read;
	}
}


void kbd_interrupt(void)
{
	newkey = 0;
	update_keyboard();
	keyproc();

	if (keysdown && keysdown < 3) {
		if (newkey) {
			keydecode();
			kbd_timer = keyrepeat.first * ticks_per_dsecond;
		} else if (! --kbd_timer) {
			keydecode();
			kbd_timer = keyrepeat.continual * ticks_per_dsecond;
		}
	}
}

/* This is used by the vt asm code, but needs to live in the kernel */
uint16_t cursorpos;

/*
 *	TTY glue
 */

void vdp_reload(void)
{
	irqflags_t irq = di();

	if (vidmode == 0) {
		vdp_setup40();
		vt_twidth = 40;
		vt_tright = 39;
	} else {
		vdp_setup32();
		vt_twidth = 32;
		vt_tright = 31;
	}
	vdp_restore_font();
	vt_cursor_off();
	vt_cursor_on();
	irqrestore(irq);
}

/*
 *	Graphics layer interface (for TMS9918A and friends)
 */

static struct videomap tms_map = {
	0,
	0x98,		/* FIXME: update at boot ? */
	0, 0,
	0, 0,
	1,
	MAP_PIO
};

/*
 *	We always report a TMS9918A. Now it is possible that someone
 *	is using a 9938 or 9958 so we might want to add some VDP autodetect
 *	code and report accordingly but that's a minor TODO
 */
static struct display tms_mode[2] = {
	{
		0,
		256, 192,
		256, 192,
		255, 255,
		FMT_VDP,
		HW_VDP_9918A,
		GFX_MULTIMODE|GFX_MAPPABLE|GFX_TEXT|GFX_VBLANK,
		16,
		0,
		40, 24
	},
	{
		1,
		256, 192,
		256, 192,
		255, 255,
		FMT_VDP,
		HW_VDP_9918A,
		GFX_MULTIMODE|GFX_MAPPABLE|GFX_TEXT|GFX_VBLANK,
		16,
		0,
		32, 24
	}
};

int vdptty_ioctl(uint8_t minor, uarg_t arg, char *ptr)
{
  uint8_t map[8];
  if (minor == 1) {
	switch(arg) {
	case GFXIOC_GETINFO:
                return uput(&tms_mode[vidmode], ptr, sizeof(struct display));
	case GFXIOC_MAP:
		if (vswitch) {
			udata.u_error = EBUSY;
			return -1;
		}
		vswitch = minor;
		return uput(&tms_map, ptr, sizeof(struct display));
	case GFXIOC_UNMAP:
		if (vswitch == minor) {
			vdp_reload();
			vswitch = 0;
		}
		return 0;
	case GFXIOC_GETMODE:
	case GFXIOC_SETMODE: {
		uint8_t m = ugetc(ptr);
		if (m > 1) {
			udata.u_error = EINVAL;
			return -1;
		}
		if (arg == GFXIOC_GETMODE)
			return uput(&tms_mode[m], ptr, sizeof(struct display));
		vidmode = m;
		vdp_reload();
		return 0;
		}
	case GFXIOC_WAITVB:
		psleep(&vdpport);
		return 0;
	case VDPIOC_SETUP:
		/* Must be locked to issue */
		if (vswitch == 0) {
			udata.u_error = EINVAL;
			return -1;
		}
		if (uget(ptr, map, 8) == -1)
			return -1;
		map[1] |= 0xA0;
		vdp_setup(map);
		return 0;
	case VDPIOC_READ:
	case VDPIOC_WRITE:
	{
		struct vdp_rw rw;
		uint16_t size;
		uint8_t *addr = (uint8_t *)rw.data;
		if (vswitch == 0) {
			udata.u_error = EINVAL;
			return -1;
		}
		if (uget(ptr, &rw, sizeof(struct vdp_rw)) != sizeof(struct vdp_rw)) {
			udata.u_error = EFAULT;
			return -1;
		}
		size = rw.lines * rw.cols;
		if (valaddr(addr, size) != size) {
			udata.u_error = EFAULT;
			return -1;
		}
		if (arg == VDPIOC_READ)
			udata.u_error = vdp_rop(&rw);
		else
			udata.u_error = vdp_wop(&rw);
		if (udata.u_error)
			return -1;
		return 0;
	}
	}
	return vt_ioctl(minor, arg, ptr);
  }
  /* Not a VT port so don't go via generic VT */
  return tty_ioctl(minor, arg, ptr);
}

int vdptty_close(uint_fast8_t minor)
{
	if (vswitch == minor) {
		vdp_reload();
		vswitch = 0;
	}
	return tty_close(minor);
}
