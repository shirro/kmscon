/*
 * uterm - Linux User-Space Terminal
 *
 * Copyright (c) 2012 Ran Benita <ran234@gmail.com>
 * Copyright (c) 2012 David Herrmann <dh.herrmann@googlemail.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files
 * (the "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

/*
 * This is a very "dumb" and simple fallback backend for keycodes
 * interpretation. It uses direct mapping from kernel keycodes to X keysyms
 * according to a basic US PC keyboard. It is not configurable and does not
 * support unicode or other languages.
 *
 * The key interpretation is affected by the following modifiers: Numlock,
 * Shift, Capslock, and "Normal" (no mofifiers) in that order. If a keycode is
 * not affected by one of these depressed modifiers, the next matching one is
 * attempted.
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <linux/input.h>
#include <X11/keysym.h>
#include "imKStoUCS.h"
#include "log.h"
#include "uterm.h"
#include "uterm_internal.h"

#define LOG_SUBSYSTEM "input_dumb"

struct kbd_dev {
	unsigned long ref;
	unsigned int mods;
};

/*
 * These tables do not contain all possible keys from linux/input.h.
 * If a keycode does not appear, it is mapped to keysym 0 and regarded as not
 * found.
 */

static const uint32_t keytab_normal[] = {
	[KEY_ESC]         =  XK_Escape,
	[KEY_1]           =  XK_1,
	[KEY_2]           =  XK_2,
	[KEY_3]           =  XK_3,
	[KEY_4]           =  XK_4,
	[KEY_5]           =  XK_5,
	[KEY_6]           =  XK_6,
	[KEY_7]           =  XK_7,
	[KEY_8]           =  XK_8,
	[KEY_9]           =  XK_9,
	[KEY_0]           =  XK_0,
	[KEY_MINUS]       =  XK_minus,
	[KEY_EQUAL]       =  XK_equal,
	[KEY_BACKSPACE]   =  XK_BackSpace,
	[KEY_TAB]         =  XK_Tab,
	[KEY_Q]           =  XK_q,
	[KEY_W]           =  XK_w,
	[KEY_E]           =  XK_e,
	[KEY_R]           =  XK_r,
	[KEY_T]           =  XK_t,
	[KEY_Y]           =  XK_y,
	[KEY_U]           =  XK_u,
	[KEY_I]           =  XK_i,
	[KEY_O]           =  XK_o,
	[KEY_P]           =  XK_p,
	[KEY_LEFTBRACE]   =  XK_bracketleft,
	[KEY_RIGHTBRACE]  =  XK_bracketright,
	[KEY_ENTER]       =  XK_Return,
	[KEY_LEFTCTRL]    =  XK_Control_L,
	[KEY_A]           =  XK_a,
	[KEY_S]           =  XK_s,
	[KEY_D]           =  XK_d,
	[KEY_F]           =  XK_f,
	[KEY_G]           =  XK_g,
	[KEY_H]           =  XK_h,
	[KEY_J]           =  XK_j,
	[KEY_K]           =  XK_k,
	[KEY_L]           =  XK_l,
	[KEY_SEMICOLON]   =  XK_semicolon,
	[KEY_APOSTROPHE]  =  XK_apostrophe,
	[KEY_GRAVE]       =  XK_grave,
	[KEY_LEFTSHIFT]   =  XK_Shift_L,
	[KEY_BACKSLASH]   =  XK_backslash,
	[KEY_Z]           =  XK_z,
	[KEY_X]           =  XK_x,
	[KEY_C]           =  XK_c,
	[KEY_V]           =  XK_v,
	[KEY_B]           =  XK_b,
	[KEY_N]           =  XK_n,
	[KEY_M]           =  XK_m,
	[KEY_COMMA]       =  XK_comma,
	[KEY_DOT]         =  XK_period,
	[KEY_SLASH]       =  XK_slash,
	[KEY_RIGHTSHIFT]  =  XK_Shift_R,
	[KEY_KPASTERISK]  =  XK_KP_Multiply,
	[KEY_LEFTALT]     =  XK_Alt_L,
	[KEY_SPACE]       =  XK_space,
	[KEY_CAPSLOCK]    =  XK_Caps_Lock,
	[KEY_F1]          =  XK_F1,
	[KEY_F2]          =  XK_F2,
	[KEY_F3]          =  XK_F3,
	[KEY_F4]          =  XK_F4,
	[KEY_F5]          =  XK_F5,
	[KEY_F6]          =  XK_F6,
	[KEY_F7]          =  XK_F7,
	[KEY_F8]          =  XK_F8,
	[KEY_F9]          =  XK_F9,
	[KEY_F10]         =  XK_F10,
	[KEY_NUMLOCK]     =  XK_Num_Lock,
	[KEY_SCROLLLOCK]  =  XK_Scroll_Lock,
	[KEY_KP7]         =  XK_KP_Home,
	[KEY_KP8]         =  XK_KP_Up,
	[KEY_KP9]         =  XK_KP_Page_Up,
	[KEY_KPMINUS]     =  XK_KP_Subtract,
	[KEY_KP4]         =  XK_KP_Left,
	[KEY_KP5]         =  XK_KP_Begin,
	[KEY_KP6]         =  XK_KP_Right,
	[KEY_KPPLUS]      =  XK_KP_Add,
	[KEY_KP1]         =  XK_KP_End,
	[KEY_KP2]         =  XK_KP_Down,
	[KEY_KP3]         =  XK_KP_Page_Down,
	[KEY_KP0]         =  XK_KP_Insert,
	[KEY_KPDOT]       =  XK_KP_Delete,
	[KEY_F11]         =  XK_F11,
	[KEY_F12]         =  XK_F12,
	[KEY_KPENTER]     =  XK_KP_Enter,
	[KEY_RIGHTCTRL]   =  XK_Control_R,
	[KEY_KPSLASH]     =  XK_KP_Divide,
	[KEY_RIGHTALT]    =  XK_Alt_R,
	[KEY_LINEFEED]    =  XK_Linefeed,
	[KEY_HOME]        =  XK_Home,
	[KEY_UP]          =  XK_Up,
	[KEY_PAGEUP]      =  XK_Page_Up,
	[KEY_LEFT]        =  XK_Left,
	[KEY_RIGHT]       =  XK_Right,
	[KEY_END]         =  XK_End,
	[KEY_DOWN]        =  XK_Down,
	[KEY_PAGEDOWN]    =  XK_Page_Down,
	[KEY_INSERT]      =  XK_Insert,
	[KEY_DELETE]      =  XK_Delete,
	[KEY_KPEQUAL]     =  XK_KP_Equal,
	[KEY_LEFTMETA]    =  XK_Meta_L,
	[KEY_RIGHTMETA]   =  XK_Meta_R,
};

#define KEYTAB_SIZE (KEY_RIGHTMETA + 1)

_Static_assert(
	(KEYTAB_SIZE == sizeof(keytab_normal) / sizeof(*keytab_normal)),
	"The KEYTAB_SIZE #define is incorrect!"
);

static const uint32_t keytab_numlock[KEYTAB_SIZE] = {
	[KEY_KP7]         =  XK_KP_7,
	[KEY_KP8]         =  XK_KP_8,
	[KEY_KP9]         =  XK_KP_9,
	[KEY_KP4]         =  XK_KP_4,
	[KEY_KP5]         =  XK_KP_5,
	[KEY_KP6]         =  XK_KP_6,
	[KEY_KP1]         =  XK_KP_1,
	[KEY_KP2]         =  XK_KP_2,
	[KEY_KP3]         =  XK_KP_3,
	[KEY_KP0]         =  XK_KP_0,
};

static const uint32_t keytab_shift[KEYTAB_SIZE] = {
	[KEY_1]           =  XK_exclam,
	[KEY_2]           =  XK_at,
	[KEY_3]           =  XK_numbersign,
	[KEY_4]           =  XK_dollar,
	[KEY_5]           =  XK_percent,
	[KEY_6]           =  XK_asciicircum,
	[KEY_7]           =  XK_ampersand,
	[KEY_8]           =  XK_asterisk,
	[KEY_9]           =  XK_parenleft,
	[KEY_0]           =  XK_parenright,
	[KEY_MINUS]       =  XK_underscore,
	[KEY_EQUAL]       =  XK_plus,
	[KEY_Q]           =  XK_Q,
	[KEY_W]           =  XK_W,
	[KEY_E]           =  XK_E,
	[KEY_R]           =  XK_R,
	[KEY_T]           =  XK_T,
	[KEY_Y]           =  XK_Y,
	[KEY_U]           =  XK_U,
	[KEY_I]           =  XK_I,
	[KEY_O]           =  XK_O,
	[KEY_P]           =  XK_P,
	[KEY_LEFTBRACE]   =  XK_braceleft,
	[KEY_RIGHTBRACE]  =  XK_braceright,
	[KEY_A]           =  XK_A,
	[KEY_S]           =  XK_S,
	[KEY_D]           =  XK_D,
	[KEY_F]           =  XK_F,
	[KEY_G]           =  XK_G,
	[KEY_H]           =  XK_H,
	[KEY_J]           =  XK_J,
	[KEY_K]           =  XK_K,
	[KEY_L]           =  XK_L,
	[KEY_SEMICOLON]   =  XK_colon,
	[KEY_APOSTROPHE]  =  XK_quotedbl,
	[KEY_GRAVE]       =  XK_asciitilde,
	[KEY_BACKSLASH]   =  XK_bar,
	[KEY_Z]           =  XK_Z,
	[KEY_X]           =  XK_X,
	[KEY_C]           =  XK_C,
	[KEY_V]           =  XK_V,
	[KEY_B]           =  XK_B,
	[KEY_N]           =  XK_N,
	[KEY_M]           =  XK_M,
	[KEY_COMMA]       =  XK_less,
	[KEY_DOT]         =  XK_greater,
	[KEY_SLASH]       =  XK_question,
};

static const uint32_t keytab_capslock[KEYTAB_SIZE] = {
	[KEY_Q]           =  XK_Q,
	[KEY_W]           =  XK_W,
	[KEY_E]           =  XK_E,
	[KEY_R]           =  XK_R,
	[KEY_T]           =  XK_T,
	[KEY_Y]           =  XK_Y,
	[KEY_U]           =  XK_U,
	[KEY_I]           =  XK_I,
	[KEY_O]           =  XK_O,
	[KEY_P]           =  XK_P,
	[KEY_A]           =  XK_A,
	[KEY_S]           =  XK_S,
	[KEY_D]           =  XK_D,
	[KEY_F]           =  XK_F,
	[KEY_G]           =  XK_G,
	[KEY_H]           =  XK_H,
	[KEY_J]           =  XK_J,
	[KEY_K]           =  XK_K,
	[KEY_L]           =  XK_L,
	[KEY_Z]           =  XK_Z,
	[KEY_X]           =  XK_X,
	[KEY_C]           =  XK_C,
	[KEY_V]           =  XK_V,
	[KEY_B]           =  XK_B,
	[KEY_N]           =  XK_N,
	[KEY_M]           =  XK_M,
};

static const struct {
	unsigned int mod;
	enum {
		MOD_NORMAL = 1,
		MOD_LOCK,
	} type;
} modmap[KEYTAB_SIZE] = {
	[KEY_LEFTCTRL]    =  {  UTERM_CONTROL_MASK,  MOD_NORMAL  },
	[KEY_LEFTSHIFT]   =  {  UTERM_SHIFT_MASK,    MOD_NORMAL  },
	[KEY_RIGHTSHIFT]  =  {  UTERM_SHIFT_MASK,    MOD_NORMAL  },
	[KEY_LEFTALT]     =  {  UTERM_MOD1_MASK,     MOD_NORMAL  },
	[KEY_CAPSLOCK]    =  {  UTERM_LOCK_MASK,     MOD_LOCK    },
	[KEY_NUMLOCK]     =  {  UTERM_MOD2_MASK,     MOD_LOCK    },
	[KEY_RIGHTCTRL]   =  {  UTERM_CONTROL_MASK,  MOD_NORMAL  },
	[KEY_RIGHTALT]    =  {  UTERM_MOD1_MASK,     MOD_NORMAL  },
	[KEY_LEFTMETA]    =  {  UTERM_MOD4_MASK,     MOD_NORMAL  },
	[KEY_RIGHTMETA]   =  {  UTERM_MOD4_MASK,     MOD_NORMAL  },
};

int kbd_dev_new(struct kbd_dev **out, struct kbd_desc *desc)
{
	struct kbd_dev *kbd;

	kbd = malloc(sizeof(*kbd));
	if (!kbd)
		return -ENOMEM;
	memset(kbd, 0, sizeof(*kbd));
	kbd->ref = 1;

	*out = kbd;
	return 0;
}

void kbd_dev_ref(struct kbd_dev *kbd)
{
	if (!kbd || !kbd->ref)
		return;

	++kbd->ref;
}

void kbd_dev_unref(struct kbd_dev *kbd)
{
	if (!kbd || !kbd->ref || --kbd->ref)
		return;

	free(kbd);
}

void kbd_dev_reset(struct kbd_dev *kbd, const unsigned long *ledbits)
{
	if (!kbd)
		return;

	kbd->mods = 0;

	if (input_bit_is_set(ledbits, LED_NUML))
		kbd->mods |= UTERM_MOD2_MASK;
	if (input_bit_is_set(ledbits, LED_CAPSL))
		kbd->mods |= UTERM_LOCK_MASK;
}

int kbd_dev_process_key(struct kbd_dev *kbd,
			uint16_t key_state,
			uint16_t code,
			struct uterm_input_event *out)
{
	uint32_t keysym;
	unsigned int mod;
	int mod_type;

	if (!kbd)
		return -EINVAL;

	/* Ignore unknown keycodes. */
	if (code >= KEYTAB_SIZE)
		return -ENOKEY;

	if (modmap[code].mod) {
		mod = modmap[code].mod;
		mod_type = modmap[code].type;

		/*
		 * We release locked modifiers on key press, like the kernel,
		 * but unlike XKB.
		 */
		if (key_state == 1) {
			if (mod_type == MOD_NORMAL)
				kbd->mods |= mod;
			else if (mod_type == MOD_LOCK)
				kbd->mods ^= mod;
		} else if (key_state == 0) {
			if (mod_type == MOD_NORMAL)
				kbd->mods &= ~mod;
		}

		/* Don't deliver events purely for modifiers. */
		return -ENOKEY;
	}

	if (key_state == 0)
		return -ENOKEY;

	keysym = 0;

	if (!keysym && kbd->mods & UTERM_MOD2_MASK)
		keysym = keytab_numlock[code];
	if (!keysym && kbd->mods & UTERM_SHIFT_MASK)
		keysym = keytab_shift[code];
	if (!keysym && kbd->mods & UTERM_LOCK_MASK)
		keysym = keytab_capslock[code];
	if (!keysym)
		keysym = keytab_normal[code];

	if (!keysym)
		return -ENOKEY;

	out->keycode = code;
	out->keysym = keysym;
	out->unicode = KeysymToUcs4(keysym) ?: UTERM_INPUT_INVALID;
	out->mods = kbd->mods;

	return 0;
}

int kbd_desc_new(struct kbd_desc **out,
			const char *layout,
			const char *variant,
			const char *options)
{
	if (!out)
		return -EINVAL;

	log_debug("new keyboard description (%s, %s, %s)",
						layout, variant, options);
	*out = NULL;
	return 0;
}

void kbd_desc_ref(struct kbd_desc *desc)
{
}

void kbd_desc_unref(struct kbd_desc *desc)
{
}

void kbd_keysym_to_string(uint32_t keysym, char *str, size_t size)
{
	snprintf(str, size, "%#x", keysym);
}
