/* tinrom - (C) 2026 TinWhisker - MIT licensed
 *
 * The SPU, for CD audio. See spu.h.
 */

#include <stdbool.h>
#include <stdint.h>
#include "hal/spu.h"
#include "ps1/registers.h"

/*
 * The SPU's bus timing, as the BIOS programs it at boot (psx-spx,
 * 1F801014h) - the same value ps1-bare-metal's initSPU() builds. Written
 * because a pre-boot start may come before the BIOS has; the assert keeps it
 * the BIOS's own, so after a BIOS boot the write changes nothing.
 */
#define SPU_BUS_DELAY (0 \
	| BIU_DEV_DELAY_WRITE_CYCLES(1) | BIU_DEV_DELAY_READ_CYCLES(14) \
	| BIU_DEV_DELAY_RECOVERY | BIU_DEV_DELAY_WIDTH_16 \
	| BIU_DEV_DELAY_AUTO_INCR | BIU_DEV_DELAY_ADDR_BITS(9) \
	| BIU_DEV_DELAY_DMA_CYCLES(0) | BIU_DEV_DELAY_DMA_CYCLES_ENABLE)

_Static_assert(SPU_BUS_DELAY == 0x200931e1, "SPU bus timing must be the BIOS's boot value");

/* SPUSTAT's low six bits follow SPUCNT's, a little late (psx-spx). */
#define SPU_MODE_BITS  0x3f
#define SPU_SETTLE_SPINS 10000

/*
 * What SPUCNT should hold, apart from the transfer mode: the CD input on or
 * off, and the reverb with it.
 */
static bool            cdInput;
static TinromSpuReverb reverb;

static uint16_t control(uint16_t transfer) {
	uint16_t value = (uint16_t) (SPU_ATTR_ENABLE | SPU_ATTR_DAC_ENABLE
		| transfer);

	if (cdInput)
		value |= SPU_ATTR_I2SA_ENABLE;

	/*
	 * Bit 7 lets the reverb write its work area, bit 2 feeds it the CD. With
	 * both off its output is whatever the area already holds, so EVOL is
	 * kept at 0 whenever they are.
	 */
	if (reverb != TINROM_SPU_REVERB_OFF)
		value |= SPU_ATTR_REVERB_ENABLE | SPU_ATTR_I2SA_REVERB;

	return value;
}

/*
 * Writes SPUCNT and waits for SPUSTAT to show it took. Bounded: on a console
 * whose SPU never answers, sound is the least of its problems, and the menu
 * should not hang over it.
 */
static void setControl(uint16_t value) {
	SPU_ATTR = value;

	for (unsigned int i = 0; i < SPU_SETTLE_SPINS; i++) {
		if ((SPU_STATX & SPU_MODE_BITS) == (value & SPU_MODE_BITS))
			break;
	}
}

/* --- reverb --------------------------------------------------------------- */

/*
 * The work area runs from ESA to the top of sound RAM (psx-spx). 24 KB holds
 * the largest setting below with room to spare, and leaves the bottom of RAM
 * - the capture buffers at 0..FFFh, and anything a later program uploads -
 * alone. Addresses are in 8-byte units throughout.
 */
#define SPU_RAM_BYTES      0x80000
#define REVERB_AREA_BYTES  0x6000
#define REVERB_AREA_UNITS  (REVERB_AREA_BYTES / 8)
#define REVERB_BASE        ((SPU_RAM_BYTES - REVERB_AREA_BYTES) / 8)

/* Manual writes go through a 32-halfword FIFO (psx-spx). */
#define FIFO_HALFWORDS 32

/* The reverb registers, 1F801DC0h on, in address order. */
enum {
	R_dAPF1, R_dAPF2, R_vIIR, R_vCOMB1, R_vCOMB2, R_vCOMB3, R_vCOMB4,
	R_vWALL, R_vAPF1, R_vAPF2, R_mLSAME, R_mRSAME, R_mLCOMB1, R_mRCOMB1,
	R_mLCOMB2, R_mRCOMB2, R_dLSAME, R_dRSAME, R_mLDIFF, R_mRDIFF,
	R_mLCOMB3, R_mRCOMB3, R_mLCOMB4, R_mRCOMB4, R_dLDIFF, R_dRDIFF,
	R_mLAPF1, R_mRAPF1, R_mLAPF2, R_mRAPF2, R_vLIN, R_vRIN,
	REVERB_REGISTERS
};

/*
 * A setting, as a room rather than as registers; layoutReverb() turns it
 * into the 32 registers. Times are in 8-byte units of the work area, which
 * the reverb steps through at 22050 halfwords a second: one unit is four
 * samples, about 0.18 ms. Volumes are signed fractions of 8000h.
 *
 *   same    how long a sound takes to come back off a near wall, and so
 *           how far apart the echoes are; `wall` is how much comes back
 *   cross   the same, bouncing between the left and right sides
 *   tap     four taps behind the reflections, two on each, that thicken
 *           the first echoes; their volumes alternate in sign so that they
 *           do not all reinforce the same frequency
 *   apf     two all-pass delays, which smear the echoes into a tail
 *   iir     a one-pole low pass on every reflection: lower is a room of
 *           softer, darker surfaces
 *   input   how much of the CD goes in - kept low, because a reflection
 *           that feeds itself piles up at its own frequency and the
 *           reverb's 16-bit work area would clip
 *   output  the reverb's share of what comes out, through EVOL
 *
 * The right side's delays are a sixteenth longer than the left's, so the
 * two sides do not echo in step and the tail sounds wide.
 *
 * `wall` sets how long the tail lasts: each trip round a reflection keeps
 * `wall` of the sound, so it is 60 dB down after log(0.001) / log(wall)
 * trips. The tunings below were checked on a host model of psx-spx's
 * formula for that decay, and for clipping with full-scale noise.
 */
typedef struct {
	uint16_t same, cross, tap[4], apf[2];
	int16_t  iir, wall, tapVolume[4], apfVolume[2], input, output;
} ReverbRoom;

static const ReverbRoom rooms[TINROM_SPU_REVERB_COUNT] = {
	[TINROM_SPU_REVERB_STUDIO] = {
		/* 12 and 15 ms walls, a 0.6 s tail, bright */
		66, 83, { 39, 52, 61, 74 }, { 22, 10 },
		0x68f6, 0x718b, { 0x5b85, -0x4ae1, 0x45e3, -0x3f3b },
		{ 0x4f5c, 0x4666 }, 0x1439, 0x4ccd
	},
	[TINROM_SPU_REVERB_HALL] = {
		/* 34 and 41 ms, 1.6 s */
		187, 226, { 127, 160, 182, 209 }, { 41, 17 },
		0x570a, 0x72fc, { 0x5333, -0x45e3, 0x428f, -0x3a3d },
		{ 0x547b, 0x4ccd }, 0x15e3, 0x799a
	},
	[TINROM_SPU_REVERB_CHURCH] = {
		/* 58 and 67 ms, 2.9 s, stone: dark and dense */
		320, 369, { 215, 259, 303, 347 }, { 61, 25 },
		0x4000, 0x7386, { 0x4fdf, -0x45e3, 0x428f, -0x3be7 },
		{ 0x570a, 0x4f5c }, 0x2db2, 0x799a
	},
	[TINROM_SPU_REVERB_DOME] = {
		/*
		 * 96 and 110 ms, 3.7 s, and short, weak all-passes, so the echoes
		 * stay apart: a big round space that answers back.
		 */
		529, 606, { 287, 391, 485, 579 }, { 23, 9 },
		0x5eb8, 0x6dfc, { 0x5b85, -0x428f, 0x4ae1, -0x3a3d },
		{ 0x399a, 0x3333 }, 0x24dd, 0x799a
	}
};

static uint16_t rightOf(uint16_t units) {
	return (uint16_t) (units + (units >> 4));
}

/*
 * Every register that is an address is an offset ahead of where the reverb
 * is in its area: it writes a reflection at its m register and, as it moves
 * on, reads it back at the d register once (m - d) units have gone by. So
 * each line gets a stretch of the area to itself, as long as its longest
 * delay, laid end to end from the bottom; a line's taps read inside its
 * own stretch.
 *
 * The cross lines are swapped on purpose: the left one is fed by dRDIFF,
 * which lies in the right one's stretch, and the other way round.
 */
static unsigned int layoutReverb(
	const ReverbRoom *room, uint16_t registers[REVERB_REGISTERS]
) {
	unsigned int at = 1;

	for (unsigned int right = 0; right < 2; right++) {
		uint16_t same = right ? rightOf(room->same)   : room->same;
		uint16_t tap1 = right ? rightOf(room->tap[0]) : room->tap[0];
		uint16_t tap2 = right ? rightOf(room->tap[1]) : room->tap[1];
		uint16_t deep = same;

		if (tap1 > deep) deep = tap1;
		if (tap2 > deep) deep = tap2;

		uint16_t top = (uint16_t) (at + deep);

		registers[R_mLSAME  + right] = top;
		registers[R_dLSAME  + right] = (uint16_t) (top - same);
		registers[R_mLCOMB1 + right] = (uint16_t) (top - tap1);
		registers[R_mLCOMB2 + right] = (uint16_t) (top - tap2);
		at = top + 1u;
	}

	for (unsigned int right = 0; right < 2; right++) {
		uint16_t cross = right ? rightOf(room->cross)  : room->cross;
		uint16_t tap3  = right ? rightOf(room->tap[2]) : room->tap[2];
		uint16_t tap4  = right ? rightOf(room->tap[3]) : room->tap[3];
		uint16_t deep  = cross;

		if (tap3 > deep) deep = tap3;
		if (tap4 > deep) deep = tap4;

		uint16_t top = (uint16_t) (at + deep);

		registers[R_mLDIFF  + right] = top;
		registers[R_dLDIFF  + right] = (uint16_t) (top - cross);
		registers[R_mLCOMB3 + right] = (uint16_t) (top - tap3);
		registers[R_mLCOMB4 + right] = (uint16_t) (top - tap4);
		at = top + 1u;
	}

	/* One delay per all-pass, shared by both sides. */
	for (unsigned int stage = 0; stage < 2; stage++) {
		registers[R_dAPF1 + stage] = room->apf[stage];

		for (unsigned int right = 0; right < 2; right++) {
			registers[R_mLAPF1 + (stage * 2) + right] =
				(uint16_t) (at + room->apf[stage]);
			at += room->apf[stage] + 1u;
		}
	}

	registers[R_vIIR]  = (uint16_t) room->iir;
	registers[R_vWALL] = (uint16_t) room->wall;
	registers[R_vAPF1] = (uint16_t) room->apfVolume[0];
	registers[R_vAPF2] = (uint16_t) room->apfVolume[1];
	registers[R_vLIN]  = (uint16_t) room->input;
	registers[R_vRIN]  = (uint16_t) room->input;

	for (unsigned int i = 0; i < 4; i++)
		registers[R_vCOMB1 + i] = (uint16_t) room->tapVolume[i];

	return at;
}

static void waitTransfer(void) {
	for (unsigned int i = 0; i < SPU_SETTLE_SPINS; i++) {
		if (!(SPU_STATX & SPU_STATX_BUSY))
			break;
	}
}

/*
 * Zeroes the work area by manual writes, 64 bytes at a time: FIFO filled in
 * stop mode, then manual-write mode to send it, then wait (psx-spx). The
 * reverb is off meanwhile, so it is not writing there too.
 */
static void clearReverbArea(void) {
	SPU_RAM_CTRL = SPU_RAM_CTRL_SIZE_512KB;

	for (unsigned int unit = 0; unit < REVERB_AREA_UNITS;
		unit += (FIFO_HALFWORDS * 2) / 8) {
		setControl(control(SPU_ATTR_XFER_NONE));
		SPU_TSA = (uint16_t) (REVERB_BASE + unit);

		for (unsigned int i = 0; i < FIFO_HALFWORDS; i++)
			SPU_DATA = 0;

		setControl(control(SPU_ATTR_XFER_WRITE));
		waitTransfer();
	}

	setControl(control(SPU_ATTR_XFER_NONE));
}

static void setReverbVolume(void) {
	int16_t volume = rooms[reverb].output; /* 0 for "off" */

	SPU_EVOLL = (uint16_t) volume;
	SPU_EVOLR = (uint16_t) volume;
}

void tinromSpuSetCdReverb(TinromSpuReverb setting) {
	if (setting >= TINROM_SPU_REVERB_COUNT)
		setting = TINROM_SPU_REVERB_OFF;

	/* Silence and stop the old one before touching its area. */
	reverb = TINROM_SPU_REVERB_OFF;
	setReverbVolume();
	setControl(control(SPU_ATTR_XFER_NONE));

	if (setting == TINROM_SPU_REVERB_OFF)
		return;

	uint16_t     registers[REVERB_REGISTERS];
	unsigned int used = layoutReverb(&rooms[setting], registers);

	/* Cannot happen with the rooms above; guards against a bad edit. */
	if (used > REVERB_AREA_UNITS)
		return;

	/* Writing ESA also moves the reverb's position to it (psx-spx). */
	SPU_ESA = REVERB_BASE;

	volatile uint16_t *port = &SPU_REVERB_dAPF1;

	for (unsigned int i = 0; i < REVERB_REGISTERS; i++)
		port[i] = registers[i];

	clearReverbArea();

	reverb = setting;
	setControl(control(SPU_ATTR_XFER_NONE));
	setReverbVolume();
}

/* --- the rest ------------------------------------------------------------- */

void tinromSpuInit(void) {
	BIU_DEV4_DELAY = SPU_BUS_DELAY;

	/* Volumes first, so nothing is heard while the rest is set up. */
	SPU_MVOLL = 0;
	SPU_MVOLR = 0;
	SPU_EVOLL = 0;
	SPU_EVOLR = 0;

	/* Every voice to release, and none feeding the reverb. */
	SPU_KOF0  = 0xffff;
	SPU_KOF1  = 0x00ff;
	SPU_EON0  = 0;
	SPU_EON1  = 0;

	/*
	 * On, unmuted, no transfers, and the CD input and its reverb as they
	 * were: tinromSpuSetCdVolume() and tinromSpuSetCdReverb() decide those.
	 */
	setControl(control(SPU_ATTR_XFER_NONE));

	/* Fixed volumes: bit 15 clear means no slide (psx-spx). */
	SPU_MVOLL = TINROM_SPU_MASTER_VOLUME & ~SPU_VOL_SLIDE_ENABLE;
	SPU_MVOLR = TINROM_SPU_MASTER_VOLUME & ~SPU_VOL_SLIDE_ENABLE;

	setReverbVolume();
}

void tinromSpuSetCdVolume(int16_t left, int16_t right) {
	/* Unlike the master volume, these are plain signed values - no slide. */
	SPU_AVOLL = (uint16_t) left;
	SPU_AVOLR = (uint16_t) right;

	cdInput = (left || right);

	setControl(control(SPU_ATTR_XFER_NONE));
}

/* --- the CD capture buffers ----------------------------------------------- */

/*
 * The SPU keeps writing what it plays from the CD input into the bottom of
 * sound RAM - left at 0..3FFh, right at 400h..7FFh, 512 samples each, round
 * and round at 44.1 kHz - and SPUSTAT bit 11 says which half of each it is
 * writing now (psx-spx). The other half is the last 256 samples, whole.
 *
 * Sound RAM can only be read by DMA (channel 4, transfer mode 3). Reads
 * need the bus to allow two extra DMA cycles, which the BIOS's timing does
 * not, so it is raised for the read and put back after - the same
 * adjustment ps1-bare-metal's receiveSPURAMData() makes.
 */
#define CAPTURE_LEFT       0x000
#define CAPTURE_RIGHT      0x400
#define CAPTURE_HALF_BYTES (TINROM_SPU_CAPTURE_SAMPLES * 2)
#define DMA_BLOCK_WORDS    16
#define DMA_SPINS          100000

static bool readSoundRam(void *out, unsigned int offset, unsigned int bytes) {
	unsigned int words = bytes / 4;

	setControl(control(SPU_ATTR_XFER_NONE));
	SPU_RAM_CTRL = SPU_RAM_CTRL_SIZE_512KB;
	SPU_TSA      = (uint16_t) (offset / 8);
	setControl(control(SPU_ATTR_XFER_DMA_READ));

	BIU_DEV4_DELAY = (SPU_BUS_DELAY & ~BIU_DEV_DELAY_DMA_CYCLES_BITMASK)
		| BIU_DEV_DELAY_DMA_CYCLES(2);

	DMA_DPCR         |= DMA_DPCR_CH_ENABLE(DMA_SPU);
	DMA_MADR(DMA_SPU) = (uintptr_t) out;
	DMA_BCR(DMA_SPU)  = DMA_BLOCK_WORDS | ((words / DMA_BLOCK_WORDS) << 16);
	DMA_CHCR(DMA_SPU) = DMA_CHCR_READ | DMA_CHCR_MODE_SLICE | DMA_CHCR_ENABLE;

	bool done = false;

	for (unsigned int i = 0; i < DMA_SPINS; i++) {
		if (!(DMA_CHCR(DMA_SPU) & DMA_CHCR_ENABLE)) {
			done = true;
			break;
		}
	}

	/* Given up on: stop the channel rather than let it write later. */
	if (!done)
		DMA_CHCR(DMA_SPU) = 0;

	BIU_DEV4_DELAY = SPU_BUS_DELAY;
	setControl(control(SPU_ATTR_XFER_NONE));

	return done;
}

bool tinromSpuReadCdCapture(
	int16_t left[TINROM_SPU_CAPTURE_SAMPLES],
	int16_t right[TINROM_SPU_CAPTURE_SAMPLES]
) {
	/* Bit 11 set: the second half is being written, so read the first. */
	unsigned int half = (SPU_STATX & SPU_STATX_CAPTURE_BUF)
		? 0 : CAPTURE_HALF_BYTES;

	return readSoundRam(left, CAPTURE_LEFT + half, CAPTURE_HALF_BYTES)
		&& readSoundRam(right, CAPTURE_RIGHT + half, CAPTURE_HALF_BYTES);
}
