/* tinrom - (C) 2026 TinWhisker - MIT licensed
 *
 * The cart's crash screen: what went wrong, where, and the registers and
 * memory around it, on a screen of its own. See crash_api.h for how it is
 * reached and entry.S for the way in.
 *
 * It runs out of the cart's ROM, after a game has had the whole console, so
 * it trusts nothing the game left: it brings the GPU up from a reset, puts
 * its own font in VRAM, and reads memory only where memory is sure to be.
 * A second exception in here would be a hang with nothing on the screen,
 * which is the very thing this exists to replace. It has no writable data
 * at all - the link refuses any - and its stack is the scratchpad.
 *
 * Everything that touches hardware goes through hw.h, so tools/test_crash
 * can draw this screen on the host.
 */

#include <stdbool.h>
#include <stdint.h>
#include "crash/crash_api.h"
#include "crash/hw.h"
#include "hal/exception.h"
#include "ps1/gpucmd.h"

/* 0x20..0x7e of the terminal's 4x8 font: tools/convertbitfont.py. */
#define FIRST_GLYPH ' '
#define LAST_GLYPH  '~'
#define GLYPHS      (LAST_GLYPH - FIRST_GLYPH + 1)

extern const uint8_t crashFont[GLYPHS][8];

/*
 * 320x240, the same on PAL and NTSC: 40 columns of 8x8 cells, 30 rows. The
 * 4x8 glyphs are drawn twice as wide as they are stored, which is what makes
 * 40 columns readable on a television.
 */
#define SCREEN_W 320
#define SCREEN_H 240
#define CELL     8

/*
 * The font, sixteen glyphs to a row and each pixel doubled across, as a
 * 16-bit texture beside the screen. A texture page starts on a multiple of
 * 64 halfwords, which 320 is.
 */
#define FONT_X     320
#define FONT_Y     0
#define FONT_W     128
#define FONT_H     (((GLYPHS + 15) / 16) * CELL)
#define FONT_PAGE  (FONT_X / 64)

/* Lit and clear texels; 0000h is the one a sprite does not draw. */
#define TEXEL_ON  0x7fffu
#define TEXEL_OFF 0x0000u

/* Text is the white font times these, 80h being 1.0. */
#define INK_TITLE gp0_rgb(0x80, 0x68, 0x20)
#define INK_TEXT  gp0_rgb(0x80, 0x80, 0x80)
#define INK_LABEL gp0_rgb(0x48, 0x58, 0x78)
#define INK_MARK  gp0_rgb(0x80, 0x70, 0x30)
#define INK_FAINT gp0_rgb(0x30, 0x38, 0x50)
#define PAPER     gp0_rgb(0x10, 0x14, 0x38)

/*
 * Size is what is short in ROM, not speed: one screen is drawn once. So the
 * helpers stay functions rather than being inlined into crashMain.
 */
#define HELPER static __attribute__((noinline))

/* GPUSTAT bit 20: the picture is PAL. */
#define GPUSTAT_PAL (1u << 20)

/*
 * The kernel's bookkeeping (psx-spx, "BIOS Memory Map"): 108h points at the
 * process control block, whose first word points at the current thread's
 * control block; 110h points at the first thread's. A TCB holds the
 * registers the kernel saved on the way in - the layout docs/debugger.md
 * gives, as nops reads it too: r0-r31 from word 2, then the resume
 * address, hi, lo, SR and cause. k0 is the kernel's, not the program's.
 */
#define KERNEL_PCB_POINTER 0x80000108u
#define KERNEL_TCB_POINTER 0x80000110u
#define TCB_BYTES          0xc0u
#define TCB_R(n)           (8u + 4u * (n))
#define TCB_HI             0x8cu
#define TCB_LO             0x90u

/* --- memory it is safe to read ------------------------------------------ */

/*
 * The address to read a word at `address` through, or 0 when there might
 * not be anything there. Main RAM in any of its mirrors and segments, and
 * the BIOS ROM; nothing else, because a bus error in here hangs the console
 * with nothing on the screen. The scratchpad is out too: it is this code's
 * stack now, not what the program left.
 */
static uint32_t readable(uint32_t address) {
	if (address & 3)
		return 0;
	if ((address >= 0x20000000u) && (address < 0x80000000u))
		return 0; /* KUSEG past the first 512 MB maps nothing */
	if (address >= 0xc0000000u)
		return 0; /* KSEG2: the cache control register and nothing else */

	uint32_t physical = address & 0x1fffffffu;

	/* 2 MB, mirrored to fill 8 MB. Read through the uncached segment. */
	if (physical < 0x00800000u)
		return 0xa0000000u | (physical & 0x001fffffu);
	if ((physical >= 0x1fc00000u) && (physical < 0x1fc80000u))
		return 0xa0000000u | physical;

	return 0;
}

HELPER bool peek(uint32_t address, uint32_t *value) {
	uint32_t at = readable(address);

	if (!at)
		return false;

	*value = hwPeek(at);

	return true;
}

/*
 * Where the kernel saved the program's registers, or 0. The current thread
 * through the PCB when that looks right, else the first thread; and only a
 * block that lies wholly in RAM.
 */
static bool tcbAt(uint32_t pointerAt, uint32_t *tcb) {
	uint32_t at, last;

	return peek(pointerAt, &at) && peek(at, tcb)
		&& readable(*tcb) && peek(*tcb + TCB_BYTES - 4, &last);
}

HELPER uint32_t findTcb(void) {
	uint32_t tcb;

	if (tcbAt(KERNEL_PCB_POINTER, &tcb))
		return tcb;

	uint32_t first;

	if (peek(KERNEL_TCB_POINTER, &first) && readable(first)
		&& peek(first + TCB_BYTES - 4, &tcb))
		return first;

	return 0;
}

/* --- drawing ------------------------------------------------------------ */

HELPER void setupScreen(bool pal) {
	hwGp1(gp1_resetGPU());

	/*
	 * The standard picture, centred as ps1-bare-metal's setupGPU() does it:
	 * 2560 dot clocks across, 240 lines either side of the middle.
	 */
	int lineMiddle = pal ? 0xa3 : 0x88;

	hwGp1(gp1_fbRangeH(0x760 - 0x500, 0x760 + 0x500));
	hwGp1(gp1_fbRangeV(lineMiddle - SCREEN_H / 2, lineMiddle + SCREEN_H / 2));
	hwGp1(gp1_fbMode(GP1_HRES_320, GP1_VRES_256,
		pal ? GP1_MODE_PAL : GP1_MODE_NTSC, false, GP1_COLOR_16BPP));
	hwGp1(gp1_fbOffset(0, 0));

	hwGp0Command(gp0_setPage(
		gp0_page(FONT_PAGE, 0, GP0_BLEND_SEMITRANS, GP0_COLOR_16BPP),
		false, true));
	hwGp0Command(gp0_setWindow(0, 0, 0, 0));
	hwGp0Command(gp0_fbOffset1(0, 0));
	hwGp0Command(gp0_fbOffset2(SCREEN_W - 1, SCREEN_H - 1));
	hwGp0Command(gp0_fbOrigin(0, 0));
	hwGp0Command(gp0_fbMask(false, false));

	hwGp0Command(gp0_vramFill() | PAPER);
	hwGp0(gp0_xy(0, 0));
	hwGp0(gp0_xy(SCREEN_W, SCREEN_H));
}

/*
 * Straight from the font table into VRAM, a pair of texels per word. Each
 * glyph's pixels are doubled across, so both texels of a pair are always
 * the same font pixel.
 */
HELPER void uploadFont(void) {
	hwGp0Command(gp0_vramWrite());
	hwGp0(gp0_xy(FONT_X, FONT_Y));
	hwGp0(gp0_xy(FONT_W, FONT_H));

	for (unsigned int y = 0; y < FONT_H; y++) {
		for (unsigned int x = 0; x < FONT_W; x += 2) {
			unsigned int glyph = (y / CELL) * 16 + x / CELL;
			bool         lit   = (glyph < GLYPHS)
				&& (crashFont[glyph][y % CELL] & (8u >> ((x % CELL) / 2)));

			hwGp0(lit ? ((TEXEL_ON << 16) | TEXEL_ON) : TEXEL_OFF);
		}
	}

	/* The texture cache may hold whatever the game last drew with. */
	hwGp0Command(gp0_flushCache());
}

HELPER void put(unsigned int column, unsigned int row, uint32_t ink, char c) {
	if ((c <= FIRST_GLYPH) || (c > LAST_GLYPH))
		return;

	unsigned int glyph = (unsigned int) (c - FIRST_GLYPH);

	hwGp0Command(gp0_rectangle8x8(true, false, false) | ink);
	hwGp0(gp0_xy((int) (column * CELL), (int) (row * CELL)));
	hwGp0(gp0_uv((glyph % 16) * CELL, (glyph / 16) * CELL, 0));
}

/* Each returns the column after what it drew. */
HELPER unsigned int text(
	unsigned int column, unsigned int row, uint32_t ink, const char *string
) {
	for (; *string; string++)
		put(column++, row, ink, *string);

	return column;
}

HELPER unsigned int hex(
	unsigned int column, unsigned int row, uint32_t ink, uint32_t value,
	unsigned int digits
) {
	static const char digit[] = "0123456789abcdef";

	while (digits--)
		put(column++, row, ink, digit[(value >> (digits * 4)) & 15]);

	return column;
}

/* A word of memory, or dashes when it cannot be read. */
HELPER unsigned int word(
	unsigned int column, unsigned int row, uint32_t ink, uint32_t address
) {
	uint32_t value;

	if (peek(address, &value))
		return hex(column, row, ink, value, 8);

	return text(column, row, INK_FAINT, "--------");
}

/* --- what to say -------------------------------------------------------- */

/* The break code of the instruction at `address`, or -1 if it is not one. */
static int breakCode(uint32_t address) {
	uint32_t instruction;

	if (!peek(address, &instruction) || ((instruction & 0xfc00003fu) != 0x0d))
		return -1;

	/*
	 * The 20-bit field is two 10-bit halves, and "break n" puts n in the
	 * upper one - so GCC's division checks, break 7 and break 6, read as
	 * psx-spx's BRK(1C00h) and BRK(1800h).
	 */
	return (int) ((instruction >> 16) & 0x3ff);
}

HELPER void describe(
	unsigned int row, uint32_t cause, uint32_t epc, uint32_t badVaddr
) {
	unsigned int code   = TINROM_EXC_CODE(cause);
	uint32_t     faulty = epc + ((cause & TINROM_EXC_CAUSE_BD) ? 4 : 0);
	const char  *what;

	switch (code) {
		case TINROM_EXC_ADEL:
			what = (badVaddr == faulty)
				? "Address error fetching an instruction"
				: "Address error on a load";
			break;

		case TINROM_EXC_ADES: what = "Address error on a store";          break;
		case TINROM_EXC_IBE:  what = "Bus error fetching an instruction"; break;
		case TINROM_EXC_DBE:  what = "Bus error on a load or store";      break;
		case TINROM_EXC_SYS:  what = "A syscall nothing handled";         break;
		case TINROM_EXC_RI:   what = "Reserved instruction";              break;
		case TINROM_EXC_CPU:  what = "Coprocessor unusable";              break;
		case TINROM_EXC_OVF:  what = "Arithmetic overflow";               break;

		case TINROM_EXC_BP: {
			int number = breakCode(faulty);

			if (number == 7) {
				what = "Division by zero";
			} else if (number == 6) {
				what = "Division overflow";
			} else if (number >= 0) {
				hex(text(1, row, INK_TEXT, "Break instruction, code "), row,
					INK_TEXT, (uint32_t) number, 3);
				return;
			} else {
				what = "Breakpoint";
			}
			break;
		}

		default:
			hex(text(1, row, INK_TEXT, "Exception "), row, INK_TEXT, code, 2);
			return;
	}

	text(1, row, INK_TEXT, what);
}

/*
 * In the order they go on the screen, three to a row: everything the kernel
 * keeps but r0, which is always zero, and k0 and k1, which are the kernel's
 * own by the time it has saved them. Then hi and lo.
 */
static const uint8_t shownRegisters[] = {
	 1,  2,  3,    4,  5,  6,    7,  8,  9,   10, 11, 12,   13, 14, 15,
	16, 17, 18,   19, 20, 21,   22, 23, 24,   25, 28, 29,   30, 31
};
static const char registerNames[32][3] = {
	"r0", "at", "v0", "v1", "a0", "a1", "a2", "a3",
	"t0", "t1", "t2", "t3", "t4", "t5", "t6", "t7",
	"s0", "s1", "s2", "s3", "s4", "s5", "s6", "s7",
	"t8", "t9", "k0", "k1", "gp", "sp", "fp", "ra"
};

#define SHOWN (sizeof(shownRegisters) / sizeof(shownRegisters[0]))

HELPER void registers(unsigned int row, uint32_t tcb) {
	if (!tcb) {
		text(1, row, INK_TEXT, "The kernel's copy of the registers");
		text(1, row + 1, INK_TEXT, "could not be found.");
		return;
	}

	for (unsigned int i = 0; i < SHOWN + 2; i++) {
		const char *name;
		uint32_t    offset;

		if (i < SHOWN) {
			name   = registerNames[shownRegisters[i]];
			offset = TCB_R(shownRegisters[i]);
		} else {
			name   = (i == SHOWN) ? "hi" : "lo";
			offset = (i == SHOWN) ? TCB_HI : TCB_LO;
		}

		unsigned int line   = row + i / 3;
		unsigned int column = text(1 + (i % 3) * 13, line, INK_LABEL, name);

		word(column + 1, line, INK_TEXT, tcb + offset);
	}
}

/* Four words a row from `address`, `marked` in another colour. */
HELPER void words(
	unsigned int row, uint32_t address, unsigned int count, uint32_t marked
) {
	for (unsigned int i = 0; i < count; i++) {
		uint32_t at = address + i * 4;

		word(1 + (i % 4) * 9, row + i / 4,
			(at == marked) ? INK_MARK : INK_TEXT, at);
	}
}

/* --- the screen --------------------------------------------------------- */

void crashMain(uint32_t cause, uint32_t epc, uint32_t badVaddr, uint32_t sr) {
	/* Read before the reset clears it: show it in the program's own mode. */
	bool pal = (hwGpuStat() & GPUSTAT_PAL) != 0;

	hwQuiet();
	setupScreen(pal);
	uploadFont();

	uint32_t tcb = findTcb();

	text(1, 1, INK_TITLE, "The game crashed.");
	text(33, 1, INK_FAINT, "tinrom");

	describe(3, cause, epc, badVaddr);

	hex(text(1, 4, INK_LABEL, "pc "), 4, INK_TEXT, epc, 8);
	hex(text(14, 4, INK_LABEL, "badv "), 4, INK_TEXT, badVaddr, 8);
	hex(text(1, 5, INK_LABEL, "cause "), 5, INK_TEXT, cause, 8);
	hex(text(17, 5, INK_LABEL, "sr "), 5, INK_TEXT, sr, 8);

	if (cause & TINROM_EXC_CAUSE_BD)
		text(1, 6, INK_LABEL, "The fault is in pc's delay slot.");

	registers(8, tcb);

	text(1, 20, INK_LABEL, "code around pc");
	words(21, epc - 8, 4, epc);

	uint32_t sp;

	if (tcb && peek(tcb + TCB_R(29), &sp)) {
		hex(text(1, 23, INK_LABEL, "stack at "), 23, INK_LABEL, sp, 8);
		words(24, sp, 8, ~0u);
	}

	text(1, 27, INK_TEXT, "Reset the console to carry on.");

	hwGp1(gp1_dispBlank(false));
	hwHalt();
}
