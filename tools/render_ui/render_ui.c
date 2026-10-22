/* tinrom - (C) 2026 TinWhisker - MIT licensed
 *
 * Draws tinrom's screens on the host, into PNGs, for reviewing layout
 * without a console. Run by scripts/render-ui.sh.
 *
 * The real UI code (ui.c, font.c, canvas.c, widgets.c, menu.c, grid.c and
 * the hex editor's view.c) builds its frames exactly as it does on a PS1.
 * What is faked is underneath it: the packet allocator and the DMA that
 * sends the list, and a small GPU that executes the list into a 1024x512
 * VRAM. It understands what tinrom draws - fills, flat and textured
 * rectangles at 4, 8 and 16bpp, modulation, half-and-half blending, the
 * texture page and the drawing offset - and nothing else, so anything new
 * the UI starts sending shows up as a warning rather than silently missing.
 *
 * Each screen is one call into the UI with a scripted pad: by default the
 * first frame is captured and then Circle is pressed to leave; a screen that
 * needs more - the terminal session - gives a script of its own.
 */

#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>
#include "common/gpu.h"
#include "core/app.h"
#include "core/module.h"
#include "core/result.h"
#include "core/savefile.h"
#include "hal/exp.h"
#include "core/settings.h"
#include "hal/cdrom.h"
#include "hal/spu.h"
#include "modules/cdplay/visual.h"
#include "hal/pad.h"
#include "modules/hexedit/hexedit.h"
#include "modules/irc/client.h"
#include "modules/terminal/terminal.h"
#include "ui/canvas.h"
#include "ui/grid.h"
#include "ui/menu.h"
#include "ui/ui.h"
#include "ui/widgets.h"

volatile uint32_t fakeGp0, fakeGp1;

static TinromContext context;
static const char   *outputDir = ".";
static const char   *capture;
static unsigned int  frameInScreen;

/* A screen's own script: the pad for each frame, and frames to capture. */
typedef struct {
	unsigned int frame;
	uint16_t     pad;
	const char  *capture;
} ScriptStep;

static const ScriptStep *script;
static unsigned int      scriptLength;

/* --- the GPU ------------------------------------------------------------- */

static uint16_t vram[512][1024];

static struct {
	unsigned int pageX, pageY, depth, blend;
	int          clipX0, clipY0, clipX1, clipY1;
	int          originX, originY;
} gpu;

static void plot(int x, int y, uint16_t color, bool blend) {
	x += gpu.originX;
	y += gpu.originY;

	if ((x < gpu.clipX0) || (x > gpu.clipX1) || (y < gpu.clipY0)
		|| (y > gpu.clipY1) || (x < 0) || (x >= 1024) || (y < 0) || (y >= 512))
		return;

	if (blend && (gpu.blend == 0)) {
		uint16_t back = vram[y][x];
		uint16_t out  = 0;

		for (unsigned int shift = 0; shift < 15; shift += 5)
			out |= (uint16_t) (((((back >> shift) & 31)
				+ ((color >> shift) & 31)) / 2) << shift);

		color = out;
	} else if (blend) {
		fprintf(stderr, "render: blend mode %u not emulated\n", gpu.blend);
	}

	vram[y][x] = color & 0x7fff;
}

static uint16_t texel(unsigned int u, unsigned int v, uint16_t clut) {
	unsigned int x  = gpu.pageX, y = (gpu.pageY + (v & 255)) & 511;
	unsigned int cx = (clut & 0x3f) * 16, cy = (clut >> 6) & 0x1ff;

	switch (gpu.depth) {
		case 0: {
			uint16_t word = vram[y][(x + ((u & 255) / 4)) & 1023];

			return vram[cy][cx + ((word >> ((u & 3) * 4)) & 15)];
		}

		case 1: {
			uint16_t word = vram[y][(x + ((u & 255) / 2)) & 1023];

			return vram[cy][cx + ((word >> ((u & 1) * 8)) & 255)];
		}

		default:
			return vram[y][(x + (u & 255)) & 1023];
	}
}

static uint16_t rgb15(uint32_t rgb) {
	return (uint16_t) (((rgb & 0xff) >> 3) | (((rgb >> 8) & 0xff) >> 3) << 5
		| (((rgb >> 16) & 0xff) >> 3) << 10);
}

static void rectangle(const uint32_t *cmd) {
	uint32_t op       = cmd[0];
	bool     raw      = op & (1 << 24);
	bool     blend    = op & (1 << 25);
	bool     textured = op & (1 << 26);
	unsigned size     = (op >> 27) & 3;
	int      x        = (int16_t) (cmd[1] & 0xffff);
	int      y        = (int16_t) (cmd[1] >> 16);
	unsigned word     = 2;
	unsigned u0 = 0, v0 = 0;
	uint16_t clut = 0;
	int      w, h;

	if (textured) {
		u0   = cmd[word] & 0xff;
		v0   = (cmd[word] >> 8) & 0xff;
		clut = (uint16_t) (cmd[word] >> 16);
		word++;
	}

	switch (size) {
		case 0:  w = cmd[word] & 0xffff; h = cmd[word] >> 16; break;
		case 1:  w = h = 1;  break;
		case 2:  w = h = 8;  break;
		default: w = h = 16; break;
	}

	for (int dy = 0; dy < h; dy++) {
		for (int dx = 0; dx < w; dx++) {
			if (!textured) {
				plot(x + dx, y + dy, rgb15(op & 0xffffff), blend);
				continue;
			}

			uint16_t t = texel(u0 + dx, v0 + dy, clut);

			if (!t)
				continue;

			if (!raw) {
				uint16_t out = 0;

				for (unsigned int c = 0; c < 3; c++) {
					unsigned m = (op >> (c * 8)) & 0xff;
					unsigned v = (((t >> (c * 5)) & 31) * m) >> 7;

					out |= (uint16_t) (((v > 31) ? 31 : v) << (c * 5));
				}

				t = out | (t & 0x8000);
			}

			plot(x + dx, y + dy, t, blend && (t & 0x8000));
		}
	}
}

/* Modulates a texel by a primitive's colour, as the GPU does: 80h is 1.0. */
static uint16_t modulate(uint16_t t, uint32_t op) {
	uint16_t out = 0;

	for (unsigned int c = 0; c < 3; c++) {
		unsigned m = (op >> (c * 8)) & 0xff;
		unsigned v = (((t >> (c * 5)) & 31) * m) >> 7;

		out |= (uint16_t) (((v > 31) ? 31 : v) << (c * 5));
	}

	return out | (t & 0x8000);
}

/*
 * A flat textured quad, which the UI uses to stretch a bitmap across - and
 * only as an upright rectangle, which is all it draws: corners 0 and 3 are
 * top-left and bottom-right, u and v run linearly between them, and the
 * right and bottom edges are left out, as on the GPU. A quad's second
 * texture word carries the texture page, which stays set afterwards.
 */
static void quad(const uint32_t *cmd) {
	uint32_t op    = cmd[0];
	bool     raw   = op & (1 << 24);
	bool     blend = op & (1 << 25);
	int      x0 = (int16_t) (cmd[1] & 0xffff), y0 = (int16_t) (cmd[1] >> 16);
	int      x1 = (int16_t) (cmd[7] & 0xffff), y1 = (int16_t) (cmd[7] >> 16);
	int      u0 = cmd[2] & 0xff, v0 = (cmd[2] >> 8) & 0xff;
	int      u1 = cmd[8] & 0xff, v1 = (cmd[8] >> 8) & 0xff;
	uint16_t clut = (uint16_t) (cmd[2] >> 16);
	uint16_t page = (uint16_t) (cmd[4] >> 16);

	if (((cmd[3] & 0xffff) != (cmd[7] & 0xffff))
		|| ((cmd[5] >> 16) != (cmd[7] >> 16)))
		fprintf(stderr, "render: a quad that is not upright\n");

	gpu.pageX = (page & 15) * 64;
	gpu.pageY = ((page >> 4) & 1) * 256;
	gpu.blend = (page >> 5) & 3;
	gpu.depth = (page >> 7) & 3;

	if ((x1 <= x0) || (y1 <= y0))
		return;

	for (int y = y0; y < y1; y++) {
		for (int x = x0; x < x1; x++) {
			unsigned u = (unsigned) (u0 + ((x - x0) * (u1 - u0)) / (x1 - x0));
			unsigned v = (unsigned) (v0 + ((y - y0) * (v1 - v0)) / (y1 - y0));
			uint16_t t = texel(u, v, clut);

			if (!t)
				continue;
			if (!raw)
				t = modulate(t, op);

			plot(x, y, t, blend && (t & 0x8000));
		}
	}
}

static unsigned int execute(const uint32_t *cmd, unsigned int left) {
	uint32_t op = cmd[0];

	switch (op >> 29) {
		case 0:
			if ((op >> 24) == 2) {
				int x = cmd[1] & 0xffff, y = cmd[1] >> 16;
				int w = cmd[2] & 0xffff, h = cmd[2] >> 16;

				for (int dy = 0; dy < h; dy++)
					for (int dx = 0; dx < w; dx++)
						vram[(y + dy) & 511][(x + dx) & 1023] =
							rgb15(op & 0xffffff);

				return 3;
			}

			return 1;

		case 1:
			/* Polygons: only the flat textured quad is drawn by the UI. */
			if (((op >> 24) & 0x1c) == 0x0c) {
				quad(cmd);

				return 9;
			}

			fprintf(stderr, "render: polygon %08x not emulated\n", op);

			return left;

		case 3: {
			unsigned size = (op >> 27) & 3;
			unsigned n    = 2 + ((op & (1 << 26)) ? 1 : 0) + (size ? 0 : 1);

			rectangle(cmd);

			return n;
		}

		case 7:
			switch ((op >> 24) & 0x1f) {
				case 1:
					gpu.pageX = (op & 15) * 64;
					gpu.pageY = ((op >> 4) & 1) * 256;
					gpu.blend = (op >> 5) & 3;
					gpu.depth = (op >> 7) & 3;
					break;
				case 3:
					gpu.clipX0 = op & 0x3ff;
					gpu.clipY0 = (op >> 10) & 0x3ff;
					break;
				case 4:
					gpu.clipX1 = op & 0x3ff;
					gpu.clipY1 = (op >> 10) & 0x3ff;
					break;
				case 5:
					gpu.originX = ((int32_t) (op << 21)) >> 21;
					gpu.originY = ((int32_t) ((op >> 11) << 21)) >> 21;
					break;
			}

			return 1;

		default:
			fprintf(stderr, "render: GP0 %08x not emulated\n", op);

			return left;
	}
}

/* --- the packet chain ---------------------------------------------------- */

/*
 * A tag's 24-bit link field can hold a PS1 address, not a host pointer, so
 * here it holds a word offset from the start of the UI context's chains.
 */
#define LINK_END 0xffffffu

static uint32_t *linkBase(void) {
	return (uint32_t *) &context.ui.chains[0];
}

static uint32_t linkTo(const uint32_t *target) {
	return (uint32_t) (target - linkBase());
}

void clearOrderingTable(uint32_t *table, size_t numEntries) {
	table[0] = LINK_END;

	for (size_t i = 1; i < numEntries; i++)
		table[i] = linkTo(&table[i - 1]);
}

uint32_t *allocateOrderedGP0Packet(
	GPUOrderedDMAChain *chain, unsigned int zIndex, size_t numCommands
) {
	if (numCommands > 16) {
		fprintf(stderr, "render: a %zu-word packet; the limit is 16\n",
			numCommands);
		exit(1);
	}

	uint32_t *ptr = chain->nextPacket;

	chain->nextPacket += numCommands + 1;

	if (chain->nextPacket >= &chain->data[GPU_CHAIN_BUFFER_SIZE]) {
		fprintf(stderr, "render: packet buffer overrun\n");
		exit(1);
	}

	*ptr = ((uint32_t) numCommands << 24)
		| (chain->orderingTable[zIndex] & 0xffffff);
	chain->orderingTable[zIndex] = linkTo(ptr);

	return &ptr[1];
}

static void savePng(const char *name, int x0, int y0, int w, int h);

void sendGPULinkedList(const void *data) {
	const uint32_t *node = data;

	for (;;) {
		uint32_t     tag  = *node;
		unsigned int size = tag >> 24;

		for (unsigned int i = 0; i < size;)
			i += execute(&node[1 + i], size - i);

		if ((tag & 0xffffff) == LINK_END)
			break;

		node = linkBase() + (tag & 0xffffff);
	}

	if (script) {
		for (unsigned int i = 0; i < scriptLength; i++)
			if ((script[i].frame == frameInScreen) && script[i].capture)
				savePng(script[i].capture, context.ui.bufferX,
					context.ui.bufferY, TINROM_SCREEN_DEVICE_WIDTH,
					(int) context.ui.height);
	} else if (capture && (frameInScreen == 1)) {
		savePng(capture, context.ui.bufferX, context.ui.bufferY,
			TINROM_SCREEN_DEVICE_WIDTH, (int) context.ui.height);
	}
}

void sendVRAMData(const void *data, unsigned int x, unsigned int y,
	unsigned int width, unsigned int height) {
	const uint16_t *in = data;

	for (unsigned int dy = 0; dy < height; dy++)
		for (unsigned int dx = 0; dx < width; dx++)
			vram[(y + dy) & 511][(x + dx) & 1023] = *(in++);
}

void uploadIndexedTexture(
	TextureInfo *info, const void *image, const void *palette,
	unsigned int imageX, unsigned int imageY,
	unsigned int paletteX, unsigned int paletteY,
	unsigned int width, unsigned int height, GP0ColorDepth colorDepth
) {
	unsigned int divider = (colorDepth == GP0_COLOR_8BPP) ? 1 : 2;

	sendVRAMData(image, imageX, imageY, width >> divider, height);
	sendVRAMData(palette, paletteX, paletteY,
		(colorDepth == GP0_COLOR_8BPP) ? 256 : 16, 1);

	info->page   = gp0_page(imageX / 64, imageY / 256, GP0_BLEND_SEMITRANS,
		colorDepth);
	info->clut   = gp0_clut(paletteX / 16, paletteY);
	info->u      = (uint8_t) ((imageX % 64) << divider);
	info->v      = (uint8_t) (imageY % 256);
	info->width  = (uint16_t) width;
	info->height = (uint16_t) height;
}

void waitForGPUDMADone(void) {}
void waitForGP0Ready(void) {}
void setupGTE(int width, int height) {}

/* --- PNG ----------------------------------------------------------------- */

static void chunk(FILE *file, const char *type, const uint8_t *data,
	uint32_t length) {
	uint8_t head[8] = {
		length >> 24, length >> 16, length >> 8, length,
		type[0], type[1], type[2], type[3]
	};
	uLong crc = crc32(0, head + 4, 4);

	crc = crc32(crc, data, length);
	fwrite(head, 1, 8, file);
	fwrite(data, 1, length, file);

	uint8_t tail[4] = { crc >> 24, crc >> 16, crc >> 8, crc };

	fwrite(tail, 1, 4, file);
}

/*
 * The display is 640x240 with pixels twice as tall as they are wide, so the
 * PNG doubles rows only: 640x480, square pixels, the shape a TV shows.
 */
#define SCALE_Y 2

static void savePng(const char *name, int x0, int y0, int w, int h) {
	char path[512];

	snprintf(path, sizeof(path), "%s/%s.png", outputDir, name);

	FILE *file = fopen(path, "wb");

	if (!file) {
		perror(path);
		exit(1);
	}

	int      ow = w, oh = h * SCALE_Y;
	size_t   rawSize = (size_t) oh * (1 + ow * 3);
	uint8_t *raw     = malloc(rawSize), *p = raw;

	for (int y = 0; y < oh; y++) {
		*(p++) = 0;

		for (int x = 0; x < ow; x++) {
			uint16_t c = vram[y0 + y / SCALE_Y][x0 + x];

			*(p++) = (uint8_t) ((c & 31) << 3);
			*(p++) = (uint8_t) (((c >> 5) & 31) << 3);
			*(p++) = (uint8_t) (((c >> 10) & 31) << 3);
		}
	}

	uLongf   packedSize = compressBound(rawSize);
	uint8_t *packed     = malloc(packedSize);

	compress(packed, &packedSize, raw, rawSize);

	static const uint8_t signature[8] = { 0x89, 'P', 'N', 'G', 13, 10, 26, 10 };
	uint8_t header[13] = {
		ow >> 24, ow >> 16, ow >> 8, ow, oh >> 24, oh >> 16, oh >> 8, oh,
		8, 2, 0, 0, 0
	};

	fwrite(signature, 1, 8, file);
	chunk(file, "IHDR", header, 13);
	chunk(file, "IDAT", packed, (uint32_t) packedSize);
	chunk(file, "IEND", NULL, 0);
	fclose(file);
	free(raw);
	free(packed);

	printf("render: %s\n", path);
}

/* --- the rest of tinrom, faked -------------------------------------------- */

static uint16_t padNow;

void tinromPadPoll(void) {
	frameInScreen++;

	if (script) {
		padNow = 0;

		for (unsigned int i = 0; i < scriptLength; i++)
			if (script[i].frame == frameInScreen)
				padNow = script[i].pad;

		if (frameInScreen > 5000) {
			fprintf(stderr, "render: a script ran away\n");
			exit(1);
		}

		return;
	}

	padNow = (frameInScreen >= 2) ? TINROM_PAD_CIRCLE : 0;
}
uint16_t tinromPadPressed(unsigned int port) { return port ? 0 : padNow; }
uint16_t tinromPadRepeated(unsigned int port) { return port ? 0 : padNow; }

static uint32_t frames;

unsigned int tinromVideoHeight(void) { return 240; }
bool         tinromVideoIsPAL(void) { return false; }
void         tinromWaitVSync(void) { frames++; }
uint32_t     tinromFrameCounter(void) { return frames; }
void         tinromDelayMicroseconds(int microseconds) {}

/*
 * SIO1, with a modem on the end that answers the init string, connects any
 * dial to a BBS whose first screen is below, and hangs up when asked.
 */
static uint8_t modemRx[16384];
static size_t  modemHead, modemTail;
static char    modemLine[128];
static size_t  modemLineLength;

static const char bbsScreen[] =
	"\033[2J\033[1;1H"
	"\033[1;34m\xc9\xcd\xcd\xcd\xcd\xcd\xcd\xcd\xcd\xcd\xcd\xcd\xcd\xcd\xcd\xcd\xcd\xcd\xcd"
	"\xcd\xcd\xcd\xcd\xcd\xcd\xcd\xcd\xcd\xcd\xcd\xcd\xcd\xcd\xcd\xcd\xcd\xcd\xcd\xcd\xbb\r\n"
	"\xba \033[1;33mSOME BBS\033[0;37m  -  a node of nowhere       \033[1;34m\xba\r\n"
	"\xc8\xcd\xcd\xcd\xcd\xcd\xcd\xcd\xcd\xcd\xcd\xcd\xcd\xcd\xcd\xcd\xcd\xcd\xcd"
	"\xcd\xcd\xcd\xcd\xcd\xcd\xcd\xcd\xcd\xcd\xcd\xcd\xcd\xcd\xcd\xcd\xcd\xcd\xcd\xcd\xbc\r\n\r\n"
	"\033[0;36m  [\033[1;37mM\033[0;36m] Message bases     [\033[1;37mF\033[0;36m] File areas"
	"      [\033[1;37mD\033[0;36m] Door games\r\n"
	"  [\033[1;37mW\033[0;36m] Who's online      [\033[1;37mB\033[0;36m] Bulletins"
	"       [\033[1;37mG\033[0;36m] Goodbye\r\n\r\n"
	"\033[0;32m\xb0\xb0\xb1\xb1\xb2\xb2\xdb\xdb\033[1;32m\xdb\xdb\xb2\xb2\xb1\xb1\xb0\xb0"
	"\033[0m  \033[41m red \033[42m green \033[43m brown \033[44m blue \033[45m"
	" magenta \033[46m cyan \033[47;30m grey \033[0m\r\n\r\n"
	"\033[1;37mLast callers:\033[0m\r\n"
	"  sysop        12:01  \x82t\x82 caf\x82  na\x8bve  \x9a" "\x81" "ber  \xe1  \xe0  \xe3  \xf1  \xfb  \xc9\xcd\xcb\xcd\xbb \xda\xc4\xc2\xc4\xbf\r\n"
	"  guest        11:47  The quick brown fox jumps over the lazy dog.\r\n"
	"  someone      09:13  THE QUICK BROWN FOX JUMPS OVER THE LAZY DOG. 0123456789\r\n\r\n"
	"\033[1;35mCommand\033[0;35m (\033[1mH\033[0;35m for help)\033[1;37m: \033[0m";

/* With this set, a dial reaches an IRC server instead, played below. */
static bool ircServer, ircOnline;

static const char ircWelcome[] =
	":irc.example 001 snep :Welcome to ExampleNet, snep\r\n"
	":irc.example 005 snep PREFIX=(ov)@+ CHANTYPES=# :are supported\r\n"
	":irc.example 375 snep :- irc.example message of the day -\r\n"
	":irc.example 372 snep :- Be excellent to each other.\r\n"
	":irc.example 376 snep :End of /MOTD\r\n"
	":NickServ!s@services NOTICE snep :You are now identified for snep.\r\n";

static const char ircJoin[] =
	":snep!snep@psx JOIN #tinrom\r\n"
	":irc.example 332 snep #tinrom :tinrom: a clean-room UniROM - \00303PlayStation\003 homebrew\r\n"
	":irc.example 353 snep = #tinrom :snep @rhew +lain alice bob carol dave\r\n"
	":irc.example 366 snep #tinrom :End of /NAMES list.\r\n"
	":alice!a@h PRIVMSG #tinrom :anyone tried the new memory card saves?\r\n"
	":rhew!r@h PRIVMSG #tinrom :yes - card 2 for tinrom, card 1 for games, works well\r\n"
	":bob!b@h PRIVMSG #tinrom :\001ACTION plugs in a WiFi modem\001\r\n"
	":carol!c@h PRIVMSG #tinrom :\002bold\002, \00304red\003, \00308,02yellow on blue\003 and caf\xc3\xa9 \xe2\x94\x80\xe2\x94\x80 \xe2\x96\x88\r\n"
	":lain!l@h PRIVMSG #tinrom :snep: is that you on a real PlayStation? the line wraps under the nick when it gets long enough, like this one does\r\n"
	":dave!d@h JOIN #tinrom\r\n"
	":alice!a@h PRIVMSG snep :psst - nice client\r\n";

static void modemSays(const char *text) {
	size_t length = strlen(text);

	memcpy(&modemRx[modemHead], text, length);
	modemHead += length;
}

static void modemHears(uint8_t byte) {
	if (byte == '\n')
		return;

	if ((byte != '\r') && (modemLineLength + 1 < sizeof(modemLine))) {
		modemLine[modemLineLength++] = (char) byte;
		modemLine[modemLineLength]   = 0;

		if (!strcmp(modemLine, "+++")) {
			modemSays("\r\nOK\r\n");
			modemLineLength = 0;
		}

		return;
	}

	if (ircOnline) {
		if (!strncmp(modemLine, "USER ", 5))
			modemSays(ircWelcome);
		else if (!strncmp(modemLine, "JOIN ", 5))
			modemSays(ircJoin);
		else if (!strncmp(modemLine, "QUIT", 4)) {
			modemSays("ERROR :Closing Link: psx (Quit: bye)\r\n\r\nNO CARRIER\r\n");
			ircOnline = false;
		}
	} else if (!strncmp(modemLine, "ATD", 3)) {
		modemSays("\r\nCONNECT 9600\r\n");

		if (ircServer)
			ircOnline = true;
		else
			modemSays(bbsScreen);
	} else if (!strncmp(modemLine, "AT", 2)) {
		modemSays("\r\nOK\r\n");
	}

	modemLineLength = 0;
}

uint32_t tinromTimerNoise(void) { return frames * 2654435761u; }
bool     tinromExpSwitchOn(TinromSwitchSource source) { return false; }
void     tinromSettingsApplyTier(TinromSettings *s, TinromTier tier) {}
uint32_t tinromSioBaud(void) { return 9600; }
TinromResult tinromSioSetBaud(uint32_t baud) { return TINROM_OK; }
void     tinromSioBufferStart(void) { modemHead = modemTail = 0; }
void     tinromSioBufferStop(void) {}
bool     tinromSioBuffering(void) { return true; }
void     tinromSioPump(void) {}
void     tinromSioHold(bool hold) {}
void     tinromSioSetCtsFlow(bool enabled) {}
void     tinromSioSetDtr(bool asserted) {}
bool     tinromSioDsr(void) { return true; }
uint32_t tinromSioOverruns(void) { return 0; }
void     tinromSioFlushInput(void) { modemTail = modemHead; }

/* No cards here: the terminal starts from its defaults, as with empty slots. */
const TinromSaveFile tinromSaveSettings = { "BETNRM-00001SETTINGS", "" };
const TinromSaveFile tinromSaveComms    = { "BETNRM-00002COMMS", "" };
bool tinromSaveCardPresent(unsigned port) { return false; }
TinromResult tinromSaveLoad(unsigned port, const TinromSaveFile *file) {
	return TINROM_ERR_NOT_PRESENT;
}
TinromResult tinromSaveStore(unsigned port, const TinromSaveFile *file) {
	return TINROM_ERR_NOT_PRESENT;
}
const void *tinromSaveSection(uint32_t tag, size_t *length) { return NULL; }
TinromResult tinromSaveSetSection(uint32_t tag, const void *d, size_t n) {
	return TINROM_OK;
}
int  tinromSettingsCard(void) { return -1; }
int  tinromSettingsSaveCard(void) { return -1; }
void tinromSettingsSetCard(unsigned port) {}
TinromResult tinromSettingsPutSection(const TinromSettings *settings) {
	return TINROM_OK;
}

size_t tinromSioReceive(uint8_t *out, size_t max) {
	size_t count = 0;

	while ((count < max) && (modemTail < modemHead))
		out[count++] = modemRx[modemTail++];

	return count;
}

void tinromSioWrite(const void *data, size_t length) {
	for (size_t i = 0; i < length; i++)
		modemHears(((const uint8_t *) data)[i]);
}

void tinromSioWriteString(const char *text) {
	tinromSioWrite(text, strlen(text));
}
void         tinromLogWrite(const char *format, ...) {}
TinromContext *tinromContext(void) { return &context; }

const char *tinromResultString(TinromResult result) {
	return result ? "the operation failed" : "OK";
}
const char *tinromTierName(TinromTier tier) { return "developer"; }
const char  tinromVersionFull[] = "0.4.0+2610011200";

static TinromResult fakeEnter(TinromContext *ctx) { return TINROM_OK; }

#define FAKE(n, s) { .name = n, .summary = s, .enter = fakeEnter }

static const TinromModule fakeModules[] = {
	FAKE("Boot disc", "Unlock the drive and start what is in it"),
	FAKE("Memory cards", "Copy, delete, undelete and image saves"),
	FAKE("File browser", "Browse a disc or card and run executables"),
	FAKE("Cheats", "Enter, store and apply cheat codes"),
	FAKE("Region patch", "Patch PAL/NTSC so imports run at speed"),
	FAKE("Cart flasher", "Identify the cart EEPROM and flash it"),
	FAKE("System information", "Console, BIOS, cart, disc and card details"),
	FAKE("Serial console", "nops server: upload, dump, poke, TTY"),
	FAKE("Hex editor", "View and patch RAM, ROM, cart and cards"),
	FAKE("GDB", "Debug over SIO1: GDB directly, or through nops"),
	FAKE("Settings", "Feature level, modules, video, autoboot"),
	FAKE("About", "Version, credits, licences and the boot log")
};

size_t tinromModuleCount(void) {
	return sizeof(fakeModules) / sizeof(fakeModules[0]);
}
const TinromModule *tinromModuleAt(size_t index) { return &fakeModules[index]; }
bool tinromModuleVisible(const TinromContext *ctx, size_t index) { return true; }
bool tinromModuleAvailable(size_t index) { return index != 5; }
void tinromModulesTick(TinromContext *ctx) {}

/* The hex editor's memory: a few recognisable things in fake RAM. */
static uint8_t fakeRam[0x1000];

bool hexRead(const HexRegion *region, uint32_t offset, uint32_t size,
	uint32_t *value) {
	uint32_t v = 0;

	memcpy(&v, &fakeRam[offset & 0xfff], size);
	*value = v;

	return true;
}
const char *hexRamOwner(const HexRegion *region, uint32_t offset) {
	return "tinrom";
}
bool hexCardDirty(const HexEditor *ed) { return false; }
bool hexCardByteDirty(const HexEditor *ed, uint32_t index) { return false; }

/* --- screens ------------------------------------------------------------- */

static void screen(const char *name) {
	capture       = name;
	script        = NULL;
	frameInScreen = 0;
}

static const char *paneLines[] = {
	"tinrom 0.4.0+2610011200",
	"built 26-10-01 12:00",
	"A clean-room, modular UniROM replacement.",
	"",
	"Built on:",
	"  ps1-bare-metal, by spicyjpeg (MIT)",
	"",
	"Font:",
	"  from the Superpowers asset packs (CC0)",
	"  superpowers-html5.com",
	"",
	"tinrom is not affiliated with, or endorsed by, any of the above. "
	"Thanks to all of them.",
	"",
	"!\"#$%&'()*+,-./0123456789:;<=>?@",
	"ABCDEFGHIJKLMNOPQRSTUVWXYZ[\\]^_`",
	"abcdefghijklmnopqrstuvwxyz{|}~"
};

static const char *paneLineAt(unsigned int index) {
	return paneLines[index];
}

/* Key/value tables line up on tab stops. */
static const char *tableLines[] = {
	"tinrom\t0.4.0+2610011200",
	"video\tNTSC (60Hz)",
	"serial\t115200 baud",
	"cart size\t128 KB",
	"card 1\tpresent, flag 08",
	"",
	"epc\t80012340\tra\t80010a5c",
	"cause\t00000024\tsr\t40000404",
	"badv\t00000000\tsp\t801ffef0",
	"",
	"  serviced before\t3",
	"  serviced after\t4",
	"",
	"This line is long enough that it has to wrap onto the next one, "
	"which comes out closer together than separate entries do."
};

static const char *tableLineAt(unsigned int index) {
	return tableLines[index];
}

static const char *describeCell(const UIGridPane *pane, unsigned int cell,
	void *user) {
	return "CRASH BANDICOOT  -  1 block, BASCUS-94900CRASH";
}

/* --- a single 10x10 pane: the example in src/ui/grid.h's comments ------- */

#define BOARD_SIDE 10

static UIGridCell boardCells[BOARD_SIDE * BOARD_SIDE];
static char       boardLabels[BOARD_SIDE * BOARD_SIDE][4];

static const char *describeBoard(const UIGridPane *pane, unsigned int cell,
	void *user) {
	static char line[48];

	(void) user;

	if (cell >= uiGridPaneCells(pane))
		return NULL;

	snprintf(line, sizeof(line), "Row %u, column %u - %s", cell / BOARD_SIDE + 1,
		cell % BOARD_SIDE + 1,
		(boardCells[cell].kind == UI_GRID_EMPTY) ? "free" : "taken");

	return line;
}

static void boardScreen(void) {
	for (unsigned int i = 0; i < BOARD_SIDE * BOARD_SIDE; i++) {
		bool taken = ((i * 37) % 11) < 3;

		snprintf(boardLabels[i], sizeof(boardLabels[i]), "%u", i);

		/* Labels only where there is something: see the text budget note. */
		boardCells[i] = (UIGridCell) {
			.kind  = taken ? UI_GRID_ITEM : UI_GRID_EMPTY,
			.label = taken ? boardLabels[i] : NULL,
			.icon  = UI_GRID_NO_ICON,
			.faded = false
		};
	}

	UIGridPane pane = {
		.title     = "Board",
		.cells     = boardCells,
		.available = true,
		.columns   = BOARD_SIDE,
		.rows      = BOARD_SIDE
	};
	UIGridCursor cursor = { 0, 23 };

	screen("grid-10x10");
	uiGridRun(&context, "10 x 10", &pane, 1, &cursor, describeBoard, NULL,
		NULL);
}

/*
 * A drive with a PlayStation disc in it: the game in track 1, then 22
 * tracks of music, each a little over three minutes. It plays by the frame
 * counter, at 60 a second, and never runs a track out.
 */
static uint32_t     cdPlayFrom;
static unsigned int cdTrack;
static bool         cdPaused;

TinromResult tinromCdAudioInit(bool mono) { return TINROM_OK; }
void         tinromSpuSetCdReverb(TinromSpuReverb reverb) {}

/*
 * No capture, and no GTE to draw the visualiser with: the 3D scene is
 * checked on a console, not here. The screen around it still is.
 */
bool tinromSpuReadCdCapture(int16_t *left, int16_t *right) { return false; }
void sceneDraw(UIContext *ui, unsigned int budget, const Visual *visual,
	VisualMode mode, unsigned int frame) {}
TinromResult tinromCdStop(void) { cdTrack = 0; return TINROM_OK; }
bool         tinromCdAudioEnded(void) { return false; }

TinromResult tinromCdIdentify(TinromDiscInfo *info) {
	memset(info, 0, sizeof(*info));
	info->type = TINROM_DISC_DATA;

	return TINROM_OK;
}

/* Sector 16 of the game: its ISO9660 volume descriptor, which is all the
 * player reads, to tell the game's track from music. */
TinromResult tinromCdReadSectors(uint32_t lba, void *buffer, size_t count) {
	memset(buffer, 0, count * 2048);

	if (lba == 16)
		memcpy(buffer, "\001CD001\001", 7);

	return TINROM_OK;
}

TinromResult tinromCdReadToc(TinromCdToc *toc) {
	memset(toc, 0, sizeof(*toc));
	toc->first = 1;
	toc->last  = 23;

	unsigned int at = 2;

	for (unsigned int t = 1; t <= toc->last; t++) {
		toc->start[t] = (uint16_t) at;
		at += (t == 1) ? 1500 : 180 + ((t * 37) % 90);
	}

	toc->leadOut = (uint16_t) at;

	return TINROM_OK;
}

unsigned int tinromCdTrackLength(const TinromCdToc *toc, unsigned int track) {
	unsigned int end = (track < toc->last) ? toc->start[track + 1]
		: toc->leadOut;

	return end - toc->start[track];
}

TinromResult tinromCdPlayTrack(const TinromCdToc *toc, unsigned int track) {
	cdTrack    = track;
	cdPaused   = false;
	cdPlayFrom = tinromFrameCounter() - (83 * 60); /* well into it */

	return TINROM_OK;
}

TinromResult tinromCdPause(void) { cdPaused = true; return TINROM_OK; }
TinromResult tinromCdResume(void) { cdPaused = false; return TINROM_OK; }

TinromResult tinromCdGetStatus(TinromCdStatus *status) {
	memset(status, 0, sizeof(*status));
	status->spinning = true;
	status->playing  = cdTrack && !cdPaused;

	return TINROM_OK;
}

TinromResult tinromCdGetPosition(TinromCdPosition *position) {
	unsigned int seconds = (tinromFrameCounter() - cdPlayFrom) / 60;

	memset(position, 0, sizeof(*position));
	position->track  = (uint8_t) cdTrack;
	position->index  = 1;
	position->minute = (uint8_t) (seconds / 60);
	position->second = (uint8_t) (seconds % 60);

	return TINROM_OK;
}

extern const TinromModule tinromModule_cdplay;

/*
 * The audio CD player: the disc is read on the first poll, then Cross plays
 * the track under the cursor, R1 steps the visualiser on (its name shows
 * under the grid; the 3D scene itself needs a GTE), and once that has gone
 * the "..." menu is opened over it and the effect stepped on to the hall.
 */
static void cdplayScreen(void) {
	static const ScriptStep steps[] = {
		{ 20, TINROM_PAD_RIGHT, NULL },
		{ 21, TINROM_PAD_RIGHT, NULL },
		{ 22, TINROM_PAD_CROSS, NULL },
		{ 40, 0, "cdplay" },
		{ 41, TINROM_PAD_R1, NULL },
		{ 43, 0, "cdplay-visual" },
		{ 141, TINROM_PAD_DOWN, NULL },
		{ 142, TINROM_PAD_DOWN, NULL },
		{ 143, TINROM_PAD_DOWN, NULL },
		{ 144, TINROM_PAD_CROSS, NULL },
		{ 146, TINROM_PAD_DOWN, NULL },
		{ 147, TINROM_PAD_DOWN, NULL },
		{ 148, TINROM_PAD_DOWN, NULL },
		{ 149, TINROM_PAD_DOWN, NULL },
		{ 150, TINROM_PAD_DOWN, NULL },
		{ 151, TINROM_PAD_CROSS, NULL },
		{ 153, TINROM_PAD_CROSS, NULL },
		{ 156, 0, "cdplay-menu" },
		{ 157, TINROM_PAD_CIRCLE, NULL },
		{ 159, TINROM_PAD_CIRCLE, NULL }
	};

	screen(NULL);
	script       = steps;
	scriptLength = sizeof(steps) / sizeof(steps[0]);
	tinromModule_cdplay.enter(&context);
	script       = NULL;
}

static void hexScreen(const char *name, unsigned unit, bool editing) {
	static const HexRegion region = {
		"Main RAM", "", HEX_REGION_MEMORY, 0x80010000u, 0x200000, true, 0,
		0x1000
	};
	static HexEditor ed;

	for (unsigned int i = 0; i < sizeof(fakeRam); i++)
		fakeRam[i] = (i < 64) ? (uint8_t) (i * 7) : (uint8_t) "tinrom, the hex editor  "[i % 24];

	memset(&ed, 0, sizeof(ed));
	ed.region      = &region;
	ed.view        = 0;
	ed.cursor      = 0x24;
	ed.unit        = unit;
	ed.editing     = editing;
	ed.nibble      = 1;
	ed.pending     = 0x1234abcd & ((unit == 4) ? 0xffffffffu : ((1u << (unit * 16)) - 1));
	ed.frameNumber = -1;

	hexViewInit();
	screen(name);

	UIContext *ui = &context.ui;

	uiSetModelVisible(ui, false);
	uiBeginFrame(ui);
	uiSetChrome(ui, "Hex editor", "D-pad: move  X: edit  []: size  /\\: menu");
	hexViewRefresh(&ed);
	hexViewDraw(&context, &ed);
	uiEndFrame(ui);
}

int main(int argc, char **argv) {
	if (argc > 1)
		outputDir = argv[1];

	uiInit(&context.ui);

	static const UIMenuItem popupItems[] = {
		{ "Go to address",   "Dial in an address, or an offset on a card", true, 0 },
		{ "Find",            "A byte, halfword, word or up to 16 characters", true, 1 },
		{ "Follow pointer",  "Go where the word under the cursor points", true, 2 },
		{ "Undo last write", "Nothing written yet", false, 3 }
	};

	screen("menu");
	uiMenuRunMain(&context);

	screen("message");
	uiMessageBox(&context, "Cannot unlock",
		"This console's drive does not support the unlock commands, or "
		"ignores them.\n\nThat is every Japanese and Asian console, and any "
		"drive firmware older than vC1.\n\nThe door-swap sequence is the only "
		"route on those, and it is not implemented yet.");

	screen("confirm");
	uiConfirm(&context, "Edit",
		"This address belongs to the monitor.\n\nChanging it can crash "
		"tinrom or lose the debugger. Edit it anyway?", true);

	screen("pane");
	uiTextPane(&context, "Credits", paneLineAt,
		sizeof(paneLines) / sizeof(paneLines[0]));

	screen("table");
	uiTextPane(&context, "System information", tableLineAt,
		sizeof(tableLines) / sizeof(tableLines[0]));

	screen("popup");
	uiPopupRun(&context, "Menu", popupItems, 4, 120, 60, NULL, NULL);

	static UIGridCell cells[UI_GRID_CELLS];

	for (unsigned int i = 0; i < UI_GRID_CELLS; i++)
		cells[i] = (UIGridCell) {
			(i < 5) ? UI_GRID_ITEM : UI_GRID_EMPTY,
			(i < 5) ? "CRASH BANDICOOT" : NULL, UI_GRID_NO_ICON, i == 3
		};

	/*
	 * Two saves with an icon, which the grid draws doubled across the
	 * wide pixels: a ring with a dot in it, each of three frames a
	 * different colour, in slots 0 and 5 - the second in another place on
	 * the icon sheet, to catch a wrong u.
	 */
	static uint8_t  iconSheet[UI_ICON_SIZE * UI_ICON_MAX_FRAMES
		* UI_ICON_SIZE / 2] __attribute__((aligned(4)));
	static uint16_t iconPalette[16] __attribute__((aligned(4))) = {
		0x0000, 0x8000, 0x7fff, 0x001f, 0x03e0, 0x7c00
	};

	for (unsigned int y = 0; y < UI_ICON_SIZE; y++) {
		for (unsigned int x = 0; x < UI_ICON_SIZE * UI_ICON_MAX_FRAMES; x++) {
			int dx = (int) (x % UI_ICON_SIZE) * 2 - 15;
			int dy = (int) y * 2 - 15;
			int r  = (dx * dx) + (dy * dy);
			uint8_t index = (r < 30) ? (uint8_t) (3 + (x / UI_ICON_SIZE))
				: (r < 120) ? 0 : (r < 196) ? 2 : (r < 256) ? 1 : 0;

			iconSheet[(y * UI_ICON_SIZE * UI_ICON_MAX_FRAMES + x) / 2] |=
				(uint8_t) (index << ((x & 1) * 4));
		}
	}

	uiIconUpload(&context.ui, 0, iconSheet, iconPalette, 3);
	uiIconUpload(&context.ui, 5, iconSheet, iconPalette, 3);
	cells[1].icon = 0;
	cells[2].icon = 5;

	UIGridPane panes[2] = {
		{ "Memory card 1", cells, true, NULL, "..." },
		{ "Memory card 2", NULL, false, "no card in this slot", "..." }
	};
	UIGridCursor cursor = { 0, 0 };

	screen("grid");
	uiGridRun(&context, "Memory cards", panes, 2, &cursor, describeCell,
		NULL, NULL);

	boardScreen();
	cdplayScreen();

	/*
	 * A terminal session: dial, let the BBS draw, open the keyboard, then
	 * Start, down to Leave, and Cross.
	 */
	static const ScriptStep session[] = {
		{ 10, 0, "terminal" },
		{ 11, TINROM_PAD_TRIANGLE, NULL },
		{ 13, TINROM_PAD_RIGHT, NULL },
		{ 15, TINROM_PAD_DOWN, NULL },
		{ 17, 0, "terminal-keys" },
		{ 18, TINROM_PAD_CIRCLE, NULL },
		{ 20, TINROM_PAD_START, NULL },
		{ 23, 0, "terminal-menu" },
		{ 24, TINROM_PAD_DOWN, NULL },
		{ 25, TINROM_PAD_DOWN, NULL },
		{ 26, TINROM_PAD_DOWN, NULL },
		{ 27, TINROM_PAD_DOWN, NULL },
		{ 28, TINROM_PAD_DOWN, NULL },
		{ 30, TINROM_PAD_CROSS, NULL }
	};
	static const CommsBookmark bbs = {
		"Some BBS", "somebbs.domain:23", COMMS_KIND_TERMINAL,
		COMMS_TELNET_OFF
	};

	screen(NULL);
	script       = session;
	scriptLength = sizeof(session) / sizeof(session[0]);
	terminalSession(&context, bbs.name, &bbs);
	script       = NULL;

	/*
	 * An IRC session: connect and join, type a little, the nick list, the
	 * menu, and leave - which says QUIT and waits for the server to hang up.
	 */
	static const ScriptStep ircScript[] = {
		{ 20, 0, "irc" },
		{ 21, TINROM_PAD_TRIANGLE, NULL },
		{ 23, TINROM_PAD_DOWN, NULL },
		{ 24, TINROM_PAD_CROSS, NULL },
		{ 25, TINROM_PAD_RIGHT, NULL },
		{ 26, TINROM_PAD_CROSS, NULL },
		{ 28, 0, "irc-keys" },
		{ 29, TINROM_PAD_CIRCLE, NULL },
		{ 30, TINROM_PAD_SQUARE, NULL },
		{ 33, TINROM_PAD_RIGHT, NULL },
		{ 35, 0, "irc-nicks" },
		{ 36, TINROM_PAD_CIRCLE, NULL },
		{ 38, TINROM_PAD_START, NULL },
		{ 41, 0, "irc-menu" },
		{ 42, TINROM_PAD_DOWN, NULL },
		{ 43, TINROM_PAD_DOWN, NULL },
		{ 44, TINROM_PAD_DOWN, NULL },
		{ 45, TINROM_PAD_DOWN, NULL },
		{ 46, TINROM_PAD_DOWN, NULL },
		{ 47, TINROM_PAD_DOWN, NULL },
		{ 48, TINROM_PAD_DOWN, NULL },
		{ 50, TINROM_PAD_CROSS, NULL }
	};
	static CommsBookmark server;
	static IrcSessionLogin login = {
		"snep", "snep", "Snep", "#tinrom", "", "hunter2", true
	};

	commsBookmarkDefaults(&server, COMMS_KIND_IRC);
	snprintf(server.name, sizeof(server.name), "ExampleNet");
	snprintf(server.target, sizeof(server.target), "irc.example:6667");

	screen(NULL);
	ircServer    = true;
	script       = ircScript;
	scriptLength = sizeof(ircScript) / sizeof(ircScript[0]);
	ircSession(&context, &server, &login);
	script       = NULL;
	ircServer    = false;

	static char address[COMMS_TARGET_MAX] = "somebbs.domain:23";

	screen("edit");
	uiEditText(&context, "Address", "host:port - somebbs.domain:23 - or "
		"whatever else the adapter dials.", address, sizeof(address));

	hexScreen("hex-bytes", 1, false);
	hexScreen("hex-words", 4, true);

	return 0;
}
