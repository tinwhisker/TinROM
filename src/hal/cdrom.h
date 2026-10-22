/* tinrom - (C) 2026 TinWhisker - MIT licensed
 *
 * CD-ROM drive.
 *
 * Deliberately not routed through the BIOS kernel's CD functions: this is a
 * native driver over the documented command set (psx-spx, see NOTICE.md).
 * ps1-bare-metal's src/ps1/registers.h and src/ps1/cdromdef.h carry the
 * register, command, interrupt, mode and MSF definitions it is built on, and
 * its example 13 (src/13_cdromInit) is where the drive initialisation and the
 * HINTSTS read technique come from.
 *
 * ps1-bare-metal has no sector-reading code yet - example 13 stops at
 * initialisation, and says the reading examples are still to come - so the
 * read path itself is tinrom's own.
 *
 * Polled, like the rest of tinrom's HAL. The drive's interrupt flags are read
 * from HINTSTS rather than through the interrupt controller.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "core/result.h"

#define TINROM_CD_SECTOR_SIZE 2048

/* --- CD audio (CD-DA) ---------------------------------------------------- */

/* A CD numbers its tracks 1 to 99, in BCD. */
#define TINROM_CD_TRACKS_MAX 99

/*
 * The disc's table of contents, from GetTN and GetTD. Times are seconds from
 * the start of the disc's time scale (00:00, two seconds before the first
 * sector). GetTD gives minutes and seconds only, rounded DOWN to a second
 * (psx-spx), so a start here can be up to 74 frames before the real one.
 *
 * What it does not say is which tracks are data. On a PlayStation disc
 * with music, track 1 is the data track and the rest are audio; on an audio
 * CD every track is. tinromCdIdentify() tells the two kinds of disc apart.
 */
typedef struct {
	uint8_t  first, last;                     /* 0 and 0: no tracks */
	uint16_t start[TINROM_CD_TRACKS_MAX + 1]; /* by track number */
	uint16_t leadOut;                         /* where the last track ends */
} TinromCdToc;

/* Where the drive is, from GetlocP: decoded from BCD. */
typedef struct {
	uint8_t track, index;          /* track 0 with leadOut set past the end */
	uint8_t minute, second, frame; /* into the track */
	uint8_t absMinute, absSecond, absFrame; /* on the disc */
	bool    leadOut;
} TinromCdPosition;

/* The drive's status byte, as every command returns it. */
typedef struct {
	uint8_t raw;
	bool    lidOpen;
	bool    spinning;
	bool    playing;  /* CD-DA playing (not paused, not reading data) */
	bool    seeking;
	bool    error;
} TinromCdStatus;

typedef enum {
	TINROM_DISC_NONE    = 0,
	TINROM_DISC_AUDIO   = 1,
	TINROM_DISC_DATA    = 2,
	TINROM_DISC_UNKNOWN = 3
} TinromDiscType;

typedef enum {
	TINROM_REGION_UNKNOWN = 0,
	TINROM_REGION_NTSC_J  = 1,
	TINROM_REGION_NTSC_U  = 2,
	TINROM_REGION_PAL     = 3,
	/** Yaroze and debug units, which are not tied to one region. */
	TINROM_REGION_FREE    = 4
} TinromRegion;

typedef struct {
	TinromDiscType type;
	/** Region of the DISC, from its SCEx string. Unknown for a CD-R. */
	TinromRegion   region;
	char           serial[16]; /* e.g. "SLES-01234", empty if unknown */
	char           label[33];  /* ISO9660 volume identifier */
	/**
	 * False for a CD-R or an unlicensed pressing. Such a disc reads only
	 * after tinromCdUnlock() has succeeded, or with a modchip fitted.
	 */
	bool           licenceStringPresent;
	/** The four raw SCEx bytes, for the disc information screen. */
	char           scex[5];
} TinromDiscInfo;

typedef struct {
	/** Region the console itself is built for, from test command 19h,22h. */
	TinromRegion region;
	/** The drive firmware's own answer, e.g. "for Europe". */
	char         regionString[24];
	/** Drive firmware version string from test command 19h,20h. */
	char         version[24];
	/** True once tinromCdUnlock() has reported success this session. */
	bool         unlocked;
} TinromDriveInfo;

#ifdef __cplusplus
extern "C" {
#endif

TinromResult tinromCdInit(void);
TinromResult tinromCdReset(void);

/** Which console this is, and what its drive firmware calls itself. */
TinromResult tinromCdDriveInfo(TinromDriveInfo *info);

/**
 * Tells the drive's sub-CPU to stop refusing discs whose SCEx region string
 * does not match the console's - which is every CD-R, and every import.
 *
 * This is the documented "secret unlock" sequence: commands 50h through 56h,
 * carrying the licence text the sub-CPU compares against, with the region
 * word taken from the console's own answer to test command 19h,22h. It is
 * described in full in psx-spx and has been public since 2013; tinrom sends
 * documented commands to documented ports and copies no code. See
 * docs/cd-boot.md for what it does and does not cover.
 *
 * Every one of the seven commands answers INT5 even when it works, so success
 * is judged by reading a sector afterwards, not by the responses.
 *
 * TINROM_ERR_UNSUPPORTED means this console cannot be unlocked this way:
 * Japanese and Asian consoles contain the code but do not act on the result,
 * and drive firmware older than vC1 has no such commands at all. On those,
 * the door-swap sequence is the only route.
 */
TinromResult tinromCdUnlock(void);
/** Puts the region check back (command 57h). */
TinromResult tinromCdLock(void);
bool         tinromCdUnlocked(void);

/** Shell state, for swap-trick sequencing. */
bool         tinromCdShellOpen(void);
/** Identify the disc currently in the drive. */
TinromResult tinromCdIdentify(TinromDiscInfo *info);

/** Read `count` 2048-byte sectors starting at `lba`. */
TinromResult tinromCdReadSectors(uint32_t lba, void *buffer, size_t count);

/** Spins down. Call before handing the drive to something else. */
TinromResult tinromCdStop(void);

/*
 * CD audio. The drive plays CD-DA on its own once told to, at 1x, with no
 * CPU involved; its output goes through a 2x2 volume matrix in the drive and
 * then into the SPU's CD input, which tinromCdAudioInit() switches on. The
 * drive does not region-check audio, so CD-Rs play without the unlock.
 *
 * Everything below is polled like the rest of this driver. Report mode -
 * the drive sending its position unprompted - is never turned on, because
 * those interrupts would be taken for the answer to whatever command came
 * next; ask with tinromCdGetPosition() instead, a few times a second.
 *
 * Reading data (tinromCdReadSectors) takes the drive over and ends any
 * playback, so a module that plays should stop on its way out.
 */

/**
 * Sets the SPU up (hal/spu.h) and the drive's volume matrix: stereo, or
 * both channels into both outputs at half volume for `mono`. Call before
 * playing; calling again changes just the matrix.
 */
TinromResult tinromCdAudioInit(bool mono);

/** Reads the table of contents. TINROM_ERR_NOT_PRESENT with no disc. */
TinromResult tinromCdReadToc(TinromCdToc *toc);
/** A track's length in seconds, from the TOC; 0 for no such track. */
unsigned int tinromCdTrackLength(const TinromCdToc *toc, unsigned int track);

/**
 * Plays `track` from its start, pausing at its end (AutoPause): see
 * tinromCdAudioEnded(). With `toc` it also seeks to the track's start
 * first, because psx-spx notes that drives ignore Play's track parameter
 * shortly after power-up; that start is rounded down to a second, so it is
 * only the fallback. `toc` may be NULL.
 */
TinromResult tinromCdPlayTrack(const TinromCdToc *toc, unsigned int track);
/** Plays from `seconds` into the disc (see TinromCdToc), to a track end. */
TinromResult tinromCdPlayAt(unsigned int seconds);
/** Pauses, keeping the disc spinning and the place. */
TinromResult tinromCdPause(void);
/** Carries on from where it paused. */
TinromResult tinromCdResume(void);
/**
 * Fast forward (above 0) or rewind (below 0), audibly, until called again
 * with 0 - which plays on normally from wherever it got to.
 */
TinromResult tinromCdScan(int direction);
/** Silences the drive's audio output, or not. Playback carries on. */
TinromResult tinromCdMute(bool mute);

/** Asks the drive for its status byte (command 01h). */
TinromResult tinromCdGetStatus(TinromCdStatus *status);
/** Where playback is. Fails while the drive is seeking. */
TinromResult tinromCdGetPosition(TinromCdPosition *position);
/**
 * True once since a track ran to its end and the drive paused there itself.
 * Noticed whenever any command is sent - a tinromCdGetStatus() poll is
 * enough - and cleared by asking. The disc stays at the END of the track, so
 * moving on is tinromCdPlayTrack(next), not tinromCdResume().
 */
bool         tinromCdAudioEnded(void);

#ifdef __cplusplus
}
#endif
