/* tinrom - (C) 2026 TinWhisker - MIT licensed
 *
 * The SPU, as far as tinrom needs it: on, unmuted, passing the CD drive's
 * audio through, and optionally putting it through the reverb unit. No
 * voices; the only sound RAM touched is the reverb's own work area.
 *
 * Booted by the BIOS, the SPU is already on - the BIOS played its own
 * startup sound with it. Booted from the cart's pre-boot hook it may never
 * have been touched. So this sets everything it relies on rather than
 * assuming, and leaves alone what it does not use.
 *
 * Written from psx-spx "Sound Processing Unit (SPU)" (see NOTICE.md), with
 * ps1-bare-metal's register names; its own initSPU() is not used, because it
 * also rewrites the bus's common delay register, which the EXP1 cart tinrom
 * may be running from depends on.
 *
 * The reverb settings are tinrom's own, worked out from psx-spx's
 * description of the reverb unit and tuned on a host model of it
 * (docs/cd-audio.md). They are not Sony's library presets and not the
 * BIOS CD player's, though they borrow that player's names for its effects.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

/* The CD input's full volume, and the master volume tinrom sets. */
#define TINROM_SPU_CD_VOLUME_MAX 0x7fff
#define TINROM_SPU_MASTER_VOLUME 0x3fff

/* Reverb on the CD input, smallest room first. */
typedef enum {
	TINROM_SPU_REVERB_OFF = 0, /* "direct" */
	TINROM_SPU_REVERB_STUDIO,
	TINROM_SPU_REVERB_HALL,
	TINROM_SPU_REVERB_CHURCH,
	TINROM_SPU_REVERB_DOME,
	TINROM_SPU_REVERB_COUNT
} TinromSpuReverb;

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Turns the SPU on and unmuted at a fixed master volume, with every voice
 * keyed off and reverb out of the mix - which also cuts short the tail of
 * the BIOS's startup sound. Safe to call again.
 */
void tinromSpuInit(void);

/**
 * The CD input's volume, each -8000h..7FFFh (negative inverts the phase).
 * Zero for both also takes the input out of the mix.
 */
void tinromSpuSetCdVolume(int16_t left, int16_t right);

/**
 * Puts the CD input through one of the reverb settings, or none. Clears the
 * reverb's work area at the top of sound RAM first, so nothing left over
 * from another setting - or from whatever ran before tinrom - is heard;
 * that is 24 KB pushed through the transfer FIFO, so it blocks briefly.
 * The setting survives tinromSpuInit().
 */
void tinromSpuSetCdReverb(TinromSpuReverb reverb);

/* Samples per channel tinromSpuReadCdCapture() hands back: about 5.8 ms. */
#define TINROM_SPU_CAPTURE_SAMPLES 256

/**
 * The last TINROM_SPU_CAPTURE_SAMPLES of CD audio the SPU played, each side,
 * from its capture buffers - for a level meter or a visualiser. Both arrays
 * must be 4-byte aligned: they are DMA'd into. Blocks for two 512-byte DMA
 * reads. False if the DMA never finished, and the arrays then hold nothing
 * useful.
 */
bool tinromSpuReadCdCapture(
	int16_t left[TINROM_SPU_CAPTURE_SAMPLES],
	int16_t right[TINROM_SPU_CAPTURE_SAMPLES]
);

#ifdef __cplusplus
}
#endif
