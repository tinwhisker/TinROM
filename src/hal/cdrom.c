/* tinrom - (C) 2026 TinWhisker - MIT licensed
 *
 * CD-ROM driver. Commands, responses and sector reads, all polled.
 *
 * Everything here follows psx-spx's "CDROM Controller I/O Ports" and command
 * documentation (see NOTICE.md). No BIOS kernel calls and no borrowed code.
 *
 * The host interface is four banks of four 8-bit registers, selected by
 * writing the bank number to ADDRESS (1F801800h). Getting the bank wrong is
 * the classic way to lose an afternoon here, so every access below goes
 * through a helper that sets the bank first.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "core/log.h"
#include "hal/cdrom.h"
#include "hal/spu.h"
#include "hal/timer.h"
#include "libc/string.h"
#include "ps1/cdromdef.h"
#include "ps1/registers.h"

/*
 * Commands, test subcommands, interrupt types, status bits and mode bits all
 * come from ps1-bare-metal's ps1/cdromdef.h. They used to be defined here,
 * because the header tinrom was pinned against did not have them; each was
 * checked value-for-value against upstream's before this file switched over.
 *
 * Note CDROMIRQType in particular: HINTSTS bits 0-2 look like three flags,
 * but the mechacon uses them as one 3-bit value. Upstream now names those
 * values, which is what tinrom needed and previously had to define itself.
 */

/* GetID second-response flag bits. */
#define CD_ID_DENIED  0x80
#define CD_ID_MISSING 0x40
#define CD_ID_AUDIO   0x10

/*
 * The CD-ROM's bus timing, as ps1-bare-metal's example 13 builds it. The
 * assert is the point: this has to stay the BIOS's own boot value (psx-spx,
 * 1F801018h), because tinromCdReset writes it unconditionally.
 */
#define CD_BUS_DELAY (0 \
	| BIU_DEV_DELAY_WRITE_CYCLES(3) | BIU_DEV_DELAY_READ_CYCLES(4) \
	| BIU_DEV_DELAY_RECOVERY | BIU_DEV_DELAY_PRESTROBE \
	| BIU_DEV_DELAY_WIDTH_8 | BIU_DEV_DELAY_ADDR_BITS(2))

_Static_assert(CD_BUS_DELAY == 0x00020943, "CD-ROM bus timing must be the BIOS's boot value");

#define CD_RESULT_MAX 16
#define CD_PARAM_MAX  16

/* Timeouts, in microseconds. Seeks and spin-up are the slow ones. */
#define CD_TIMEOUT_FAST 100000  /* 0.1s: acknowledge for a simple command */
#define CD_TIMEOUT_SLOW 2000000 /* 2s: seek, spin-up, GetID's second reply */

typedef struct {
	uint8_t      type;                 /* INT1..INT5, 0 for none */
	uint8_t      data[CD_RESULT_MAX];
	unsigned int length;
} CdResponse;

static bool            driveReady;
static bool            driveUnlocked;
static TinromDriveInfo driveInfo;

/* An INT4 seen in passing: AutoPause stopping at the end of a track. */
static bool            audioEnded;

/* --- register plumbing -------------------------------------------------- */

static void setBank(unsigned int bank) {
	CDROM_ADDRESS = (uint8_t) (bank & 3);
}

static uint8_t interruptFlags(void) {
	setBank(1);

	/*
	 * Read until two reads agree. The three type bits in HINTSTS are not
	 * guaranteed to change at the same instant - on early models the
	 * mechacon and the decoder run on different clocks - so a single read can
	 * land part-way through an update and return a type that was never sent.
	 * From ps1-bare-metal's example 13 (MIT, spicyjpeg); tinrom used to read
	 * it once.
	 */
	uint8_t first, second;

	do {
		first  = (uint8_t) (CDROM_HINTSTS & CDROM_HINT_INT_BITMASK);
		second = (uint8_t) (CDROM_HINTSTS & CDROM_HINT_INT_BITMASK);
	} while (first != second);

	return first;
}

static void acknowledge(void) {
	setBank(1);
	CDROM_HCLRCTL = CDROM_HCLRCTL_CLRINT_BITMASK;

	/* With HINTMSK at 0 (see tinromCdReset) nothing should reach IRQ_STAT,
	 * but a flag left set there confuses anything that later polls it, and
	 * clearing it costs one store. */
	IRQ_STAT = ~(1 << IRQ_CDROM);
}

static void clearParameters(void) {
	setBank(1);
	CDROM_HCLRCTL = CDROM_HCLRCTL_CLRPRM;

	/* ps1-bare-metal's example 13 lets the clear settle for a microsecond
	 * before refilling the FIFO; tinrom went straight on. Cheap enough not to
	 * argue with. */
	tinromDelayMicroseconds(1);
}

/** Drains whatever a previous command left behind. */
static void drain(void) {
	for (unsigned int i = 0; i < 8; i++) {
		uint8_t type = interruptFlags();

		if (!type)
			break;

		/*
		 * Nothing waits for this one: AutoPause raises it when a track runs
		 * out, long after the Play that started it was answered. The next
		 * command clears it here, so this is where it is noticed.
		 */
		if (type == CDROM_IRQ_DATA_END)
			audioEnded = true;

		setBank(1);

		while (CDROM_HSTS & CDROM_HSTS_RSLRRDY)
			(void) CDROM_RESULT;

		acknowledge();
		tinromDelayMicroseconds(100);
	}

	clearParameters();
}

static bool waitInterrupt(unsigned int timeoutMicroseconds) {
	for (unsigned int waited = 0; waited < timeoutMicroseconds; waited += 50) {
		if (interruptFlags())
			return true;

		tinromDelayMicroseconds(50);
	}

	return false;
}

static void readResult(CdResponse *response) {
	response->length = 0;

	setBank(1);

	while ((CDROM_HSTS & CDROM_HSTS_RSLRRDY)
		&& (response->length < CD_RESULT_MAX))
		response->data[response->length++] = CDROM_RESULT;
}

/**
 * Waits for the next interrupt and reads whatever came with it. Does NOT
 * acknowledge - the caller decides, because some commands answer twice and
 * acknowledging is what releases the second one.
 */
static TinromResult collect(CdResponse *response, unsigned int timeout) {
	response->type   = 0;
	response->length = 0;

	if (!waitInterrupt(timeout))
		return TINROM_ERR_TIMEOUT;

	response->type = interruptFlags();

	readResult(response);

	return TINROM_OK;
}

/**
 * Sends a command and collects its first response. Any earlier interrupt is
 * cleared first: psx-spx is explicit that sending a command with one pending
 * makes the drive's behaviour unpredictable.
 */
static TinromResult command(
	uint8_t        code,
	const uint8_t *parameters,
	unsigned int   count,
	CdResponse    *response,
	unsigned int   timeout
) {
	if (count > CD_PARAM_MAX)
		return TINROM_ERR_BAD_ARG;

	drain();

	setBank(0);

	for (unsigned int i = 0; i < count; i++)
		CDROM_PARAMETER = parameters[i];

	CDROM_COMMAND = code;

	return collect(response, timeout);
}

/** For commands that answer twice: acknowledge the first, wait for the second. */
static TinromResult secondResponse(CdResponse *response, unsigned int timeout) {
	acknowledge();

	return collect(response, timeout);
}

/* --- basics ------------------------------------------------------------- */

static TinromResult simpleCommand(uint8_t code, CdResponse *response) {
	TinromResult result = command(code, NULL, 0, response, CD_TIMEOUT_FAST);

	if (result == TINROM_OK)
		acknowledge();

	return result;
}

TinromResult tinromCdReset(void) {
	/*
	 * The CD-ROM's own bus timing, set to the value the BIOS programs at boot
	 * (the static assert holds it there), so a drive left misconfigured by
	 * whatever ran before still reads. Normally a no-op.
	 *
	 * Only DEV5. ps1-bare-metal's example 13 also writes COM_DELAY, but that
	 * one is shared by every device on the bus - including the EXP1 cart
	 * tinrom may be executing from - and is not something a CD init routine
	 * has any business changing.
	 */
	BIU_DEV5_DELAY = CD_BUS_DELAY;

	/* tinrom polls, so the drive stays out of the CPU's interrupt controller. */
	IRQ_MASK &= ~(1 << IRQ_CDROM);

	/*
	 * Reset the decoder's side of the interface, as example 13 does: no data
	 * request pending, nothing forwarded to IRQ_STAT, and every latched flag
	 * cleared - including the two buffer flags drain() does not touch.
	 *
	 * HINTMSK gates only the line to the CPU, not what HINTSTS reports, so
	 * masking everything costs the polling nothing. An earlier comment here
	 * said the opposite and set it to 1Fh; example 13 polls successfully with
	 * it at 0.
	 */
	setBank(0);
	CDROM_HCHPCTL = 0;

	setBank(1);
	CDROM_HINTMSK_W = 0;
	CDROM_HCLRCTL   = CDROM_HCLRCTL_CLRINT_BITMASK
		| CDROM_HCLRCTL_CLRBFEMPT | CDROM_HCLRCTL_CLRBFWRDY;

	drain();

	CdResponse   response;
	TinromResult result = command(
		CDROM_CMD_INIT, NULL, 0, &response, CD_TIMEOUT_FAST
	);

	if (result != TINROM_OK)
		return result;

	if (response.type == CDROM_IRQ_ERROR)
		return TINROM_ERR_IO;

	/* Init answers INT3 and then INT2 when the drive has settled. */
	result = secondResponse(&response, CD_TIMEOUT_SLOW);

	acknowledge();

	if (result != TINROM_OK)
		return result;

	driveReady = true;

	return TINROM_OK;
}

TinromResult tinromCdInit(void) {
	driveReady    = false;
	driveUnlocked = false;

	memset(&driveInfo, 0, sizeof(driveInfo));

	TinromResult result = tinromCdReset();

	if (result != TINROM_OK)
		return result;

	/* Neither of these spins the disc, so they are cheap at boot. */
	(void) tinromCdDriveInfo(&driveInfo);

	return TINROM_OK;
}

bool tinromCdShellOpen(void) {
	CdResponse response;

	if (simpleCommand(CDROM_CMD_NOP, &response) != TINROM_OK)
		return false;
	if (!response.length)
		return false;

	return (response.data[0] & CDROM_CMDSTAT_LID_OPEN) != 0;
}

TinromResult tinromCdStop(void) {
	CdResponse response;

	return simpleCommand(CDROM_CMD_STOP, &response);
}

/* --- drive identity ----------------------------------------------------- */

static TinromResult testString(
	uint8_t subFunction, char *out, size_t size
) {
	uint8_t      parameter = subFunction;
	CdResponse   response;
	TinromResult result    = command(
		CDROM_CMD_TEST, &parameter, 1, &response, CD_TIMEOUT_FAST
	);

	if (result != TINROM_OK)
		return result;

	uint8_t type = response.type;

	acknowledge();

	/* vC0 drives answer INT5(11h,10h) to the region query. */
	if (type == CDROM_IRQ_ERROR)
		return TINROM_ERR_UNSUPPORTED;

	size_t copied = 0;

	for (unsigned int i = 0; (i < response.length) && ((copied + 1) < size); i++) {
		char ch = (char) response.data[i];

		if (!ch)
			break;
		if ((ch < 0x20) || (ch > 0x7e))
			continue;

		out[copied++] = ch;
	}

	out[copied] = '\0';

	return TINROM_OK;
}

/*
 * The drive firmware's answer to test 19h,22h, verbatim from psx-spx:
 *
 *   "for Europe"  PAL, Europe              requires SCEE discs
 *   "for U/C"     NTSC, North America      requires SCEA discs
 *   "for Japan"   NTSC, Japan / Asia       requires SCEI discs
 *   "for NETNA"   Yaroze, region free      requires SCEx discs
 *   "for US/AEP"  debug, region free       accepts unlicensed CD-Rs
 */
static TinromRegion regionFromString(const char *text) {
	if (!strncmp(text, "for Europe", 10))
		return TINROM_REGION_PAL;
	if (!strncmp(text, "for U/C", 7))
		return TINROM_REGION_NTSC_U;
	if (!strncmp(text, "for Japan", 9))
		return TINROM_REGION_NTSC_J;
	if (!strncmp(text, "for NETNA", 9) || !strncmp(text, "for US/AEP", 10))
		return TINROM_REGION_FREE;

	return TINROM_REGION_UNKNOWN;
}

TinromResult tinromCdDriveInfo(TinromDriveInfo *info) {
	if (!info)
		return TINROM_ERR_BAD_ARG;

	memset(info, 0, sizeof(*info));

	(void) testString(CDROM_TEST_GET_VERSION, info->version, sizeof(info->version));

	TinromResult result = testString(
		CDROM_TEST_GET_REGION, info->regionString, sizeof(info->regionString)
	);

	if (result == TINROM_OK)
		info->region = regionFromString(info->regionString);

	info->unlocked = driveUnlocked;

	TINROM_LOG("cd: %s, %s", info->version[0] ? info->version : "?",
		info->regionString[0] ? info->regionString : "region unknown");

	return TINROM_OK;
}

bool tinromCdUnlocked(void) {
	return driveUnlocked;
}

/* --- the unlock sequence ------------------------------------------------ */

/*
 * Commands 50h..56h carry, between them, the licence sentence the drive's
 * sub-CPU compares against, with the last word depending on the console's own
 * region. These are interface constants in the sense docs/attribution.md
 * already defines: values a chip compares against, like a file format's magic
 * number. They are documented in psx-spx and no code is taken from anywhere.
 *
 * Every one of the seven answers INT5 whether or not it worked, so there is
 * nothing to check as we go. The only honest test is to read a sector
 * afterwards, which is what tinromCdUnlock() does.
 */
static const char *const unlockParts[] = {
	NULL,            /* 50h takes no parameter */
	"Licensed by",   /* 51h */
	"Sony",          /* 52h */
	"Computer",      /* 53h */
	"Entertainment", /* 54h */
	NULL,            /* 55h: the region word, filled in below */
	NULL             /* 56h takes no parameter */
};

#define UNLOCK_STEPS (sizeof(unlockParts) / sizeof(unlockParts[0]))

static const char *unlockRegionWord(TinromRegion region) {
	switch (region) {
		case TINROM_REGION_NTSC_U: return "of America";
		case TINROM_REGION_PAL:    return "(Europe)";
		case TINROM_REGION_FREE:   return "World wide";
		/* Documented, but Japanese consoles ignore the result. */
		case TINROM_REGION_NTSC_J: return "Inc.";
		default:                   return NULL;
	}
}

static void sendUnlockStep(unsigned int step, const char *text) {
	drain();

	setBank(0);

	if (text) {
		for (const char *c = text; *c; c++)
			CDROM_PARAMETER = (uint8_t) *c;
	}

	CDROM_COMMAND = (uint8_t) (CDROM_CMD_UNLOCK0 + step);

	CdResponse response;

	/* INT5 every time, success or not. Collect it so the next step is clean. */
	(void) collect(&response, CD_TIMEOUT_FAST);

	acknowledge();
}

TinromResult tinromCdUnlock(void) {
	if (driveUnlocked)
		return TINROM_OK;

	if (!driveInfo.regionString[0])
		(void) tinromCdDriveInfo(&driveInfo);

	/*
	 * A drive that cannot answer the region query is older than vC1 and has
	 * no unlock commands either; a Japanese console answers but ignores the
	 * unlocked flag. Neither is a fault, and neither is worth pretending
	 * about - the swap sequence is the route on those consoles.
	 */
	const char *word = unlockRegionWord(driveInfo.region);

	if (!word) {
		TINROM_LOG("cd: no unlock for region '%s'", driveInfo.regionString);

		return TINROM_ERR_UNSUPPORTED;
	}

	if (driveInfo.region == TINROM_REGION_NTSC_J) {
		TINROM_LOG("cd: Japanese consoles ignore the unlock flag");

		return TINROM_ERR_UNSUPPORTED;
	}

	for (unsigned int step = 0; step < UNLOCK_STEPS; step++)
		sendUnlockStep(
			step, (step == 5) ? word : unlockParts[step]
		);

	/*
	 * The responses said nothing useful, so ask the disc. A sector that reads
	 * now and would not before is the whole proof.
	 */
	static uint8_t probe[TINROM_CD_SECTOR_SIZE];

	driveUnlocked = true;

	TinromResult result = tinromCdReadSectors(16, probe, 1);

	if (result != TINROM_OK) {
		driveUnlocked = false;

		TINROM_LOG("cd: unlock did not take (%s)", tinromResultString(result));

		return TINROM_ERR_IO;
	}

	driveInfo.unlocked = true;

	TINROM_LOG("cd: unlocked, %s", word);

	return TINROM_OK;
}

TinromResult tinromCdLock(void) {
	CdResponse response;

	(void) command(CDROM_CMD_LOCK, NULL, 0, &response, CD_TIMEOUT_FAST);

	acknowledge();

	driveUnlocked      = false;
	driveInfo.unlocked = false;

	return TINROM_OK;
}

/* --- identification ----------------------------------------------------- */

static TinromRegion regionFromScex(const char *scex) {
	if (strncmp(scex, "SCE", 3))
		return TINROM_REGION_UNKNOWN;

	switch (scex[3]) {
		case 'I': return TINROM_REGION_NTSC_J;
		case 'A': return TINROM_REGION_NTSC_U;
		case 'E': return TINROM_REGION_PAL;
		case 'W': return TINROM_REGION_FREE;
		default:  return TINROM_REGION_UNKNOWN;
	}
}

TinromResult tinromCdIdentify(TinromDiscInfo *info) {
	if (!info)
		return TINROM_ERR_BAD_ARG;

	memset(info, 0, sizeof(*info));

	info->type   = TINROM_DISC_NONE;
	info->region = TINROM_REGION_UNKNOWN;

	if (!driveReady && (tinromCdReset() != TINROM_OK))
		return TINROM_ERR_NOT_PRESENT;

	CdResponse   response;
	TinromResult result = command(
		CDROM_CMD_GETID, NULL, 0, &response, CD_TIMEOUT_SLOW
	);

	if (result != TINROM_OK)
		return result;

	/* Door open, spinning up or still working out what it has. */
	if (response.type == CDROM_IRQ_ERROR) {
		acknowledge();

		return TINROM_ERR_NOT_PRESENT;
	}

	result = secondResponse(&response, CD_TIMEOUT_SLOW);

	uint8_t type = response.type;

	acknowledge();

	if (result != TINROM_OK)
		return result;
	if (response.length < 8)
		return TINROM_ERR_IO;

	uint8_t flags = response.data[1];

	memcpy(info->scex, &response.data[4], 4);
	info->scex[4] = '\0';

	if (flags & CD_ID_MISSING) {
		info->type = TINROM_DISC_NONE;

		return TINROM_ERR_NOT_PRESENT;
	}

	if (flags & CD_ID_AUDIO)
		info->type = TINROM_DISC_AUDIO;
	else
		info->type = TINROM_DISC_DATA;

	/*
	 * INT2 means the drive accepted the disc as licensed. INT5 with a disc
	 * present means it did not - a CD-R, or a pressing from another region.
	 * Neither stops us reading it once unlocked, so this is information, not
	 * an error.
	 */
	info->licenceStringPresent =
		(type != CDROM_IRQ_ERROR) && !(flags & CD_ID_DENIED);
	info->region = regionFromScex(info->scex);

	TINROM_LOG("cd: id flags %02x scex '%s'", (unsigned int) flags, info->scex);

	return TINROM_OK;
}

/* --- sector reads ------------------------------------------------------- */

static TinromResult setMode(void) {
	uint8_t      mode = CDROM_MODE_SPEED_2X; /* 2048-byte data sectors */
	CdResponse   response;
	TinromResult result = command(
		CDROM_CMD_SETMODE, &mode, 1, &response, CD_TIMEOUT_FAST
	);

	acknowledge();

	return result;
}

/*
 * Copies one sector out of the drive's buffer.
 *
 * Byte at a time through RDDATA. DMA channel 3 would copy faster, but the
 * copy is not what sets the speed: at double speed a sector arrives every
 * 6.7 ms and copying it this way takes roughly one, so DMA would save about a
 * tenth of a second on a game's load - and a wrong DMA setup is a hang
 * rather than a slow read. Worth revisiting only for something that streams.
 */
/* Spins waiting for DRQSTS. Generous: this is a "did the drive die" limit,
 * not a timing one. */
#define COPY_READY_SPINS 10000

static bool copySector(uint8_t *out) {
	setBank(0);
	CDROM_HCHPCTL = CDROM_HCHPCTL_BFRD;

	/*
	 * The decoder needs a moment before DRQSTS comes up.
	 *
	 * Running out of patience here used to fall through and read 2048 bytes
	 * anyway, which returns whatever RDDATA happens to give when there is no
	 * sector behind it - so a drive that was not ready produced a buffer full
	 * of plausible-looking rubbish and a success code. Silent corruption is a
	 * far worse failure than a refusal: the caller cannot tell, and the
	 * garbage only announces itself much later, somewhere else.
	 */
	bool ready = false;

	for (unsigned int spins = 0; spins < COPY_READY_SPINS; spins++) {
		if (CDROM_HSTS & CDROM_HSTS_DRQSTS) {
			ready = true;

			break;
		}
	}

	if (!ready) {
		CDROM_HCHPCTL = 0;

		return false;
	}

	for (unsigned int i = 0; i < TINROM_CD_SECTOR_SIZE; i++)
		out[i] = CDROM_RDDATA;

	CDROM_HCHPCTL = 0;

	return true;
}

/*
 * Pauses a read and waits for the drive to say it has stopped.
 *
 * Pause answers twice. The first answer comes at once, but the drive goes
 * on reading until the second, INT2 - a sector or several later at double
 * speed - and those sectors raise INT1 in between. Returning after the first
 * answer left them to arrive in the middle of the next command, which took
 * the first of them as its own answer. In practice that was the very next
 * read failing with an I/O error: the volume descriptor read fine, and the
 * root directory straight after it did not, so the boot path could not find
 * SYSTEM.CNF on a disc the file browser, reading at its own pace, read
 * without trouble.
 */
/* Sectors that may land before the pause takes: most of a second's worth. */
#define PAUSE_ANSWERS_MAX 256

static void stopReading(void) {
	CdResponse response;

	if (command(CDROM_CMD_PAUSE, NULL, 0, &response, CD_TIMEOUT_SLOW)
		== TINROM_OK) {
		for (unsigned int i = 0; i < PAUSE_ANSWERS_MAX; i++) {
			uint8_t type = response.type;

			acknowledge();

			if ((type == CDROM_IRQ_COMPLETE) || (type == CDROM_IRQ_ERROR))
				break;

			/* Pause's own first answer, or a sector still landing. */
			if (collect(&response, CD_TIMEOUT_SLOW) != TINROM_OK) {
				TINROM_LOG("cd: the drive did not say it had paused");
				break;
			}
		}
	}

	drain();
}

TinromResult tinromCdReadSectors(uint32_t lba, void *buffer, size_t count) {
	if (!buffer || !count)
		return TINROM_ERR_BAD_ARG;

	if (!driveReady && (tinromCdReset() != TINROM_OK))
		return TINROM_ERR_NOT_PRESENT;

	TinromResult result = setMode();

	if (result != TINROM_OK)
		return result;

	uint8_t location[3];

	cdrom_lbaToMSF(location, lba);

	CdResponse response;

	result = command(
		CDROM_CMD_SETLOC, location, sizeof(location), &response, CD_TIMEOUT_FAST
	);

	if (result != TINROM_OK)
		return result;

	if (response.type == CDROM_IRQ_ERROR) {
		acknowledge();

		return TINROM_ERR_IO;
	}

	acknowledge();

	result = command(CDROM_CMD_READN, NULL, 0, &response, CD_TIMEOUT_SLOW);

	if (result != TINROM_OK)
		return result;

	if (response.type == CDROM_IRQ_ERROR) {
		acknowledge();

		return TINROM_ERR_IO;
	}

	acknowledge();

	uint8_t *out = (uint8_t *) buffer;

	for (size_t i = 0; i < count; i++) {
		result = collect(&response, CD_TIMEOUT_SLOW);

		if (result != TINROM_OK)
			break;

		/*
		 * INT1 is a sector. INT5 here is the drive refusing the disc, which
		 * on a locked drive is exactly what an unlicensed disc looks like.
		 */
		if (response.type == CDROM_IRQ_ERROR) {
			acknowledge();
			result = TINROM_ERR_IO;

			break;
		}

		if (response.type == CDROM_IRQ_DATA_READY) {
			bool copied = copySector(&out[i * TINROM_CD_SECTOR_SIZE]);

			acknowledge();

			if (!copied) {
				TINROM_LOG("cd: sector %u of %u never became readable",
					(unsigned int) i, (unsigned int) count);
				result = TINROM_ERR_IO;

				break;
			}

			continue;
		}

		/* Anything else - INT4 at the end of the disc, say - ends the read. */
		acknowledge();
		result = TINROM_ERR_IO;

		break;
	}

	stopReading();

	return result;
}

/* --- CD audio ----------------------------------------------------------- */

/*
 * From psx-spx's CD-ROM command documentation (see NOTICE.md): GetTN, GetTD,
 * GetlocP, Play, Forward, Backward, Pause, Mute and Demute, and Setmode's
 * CDDA and AutoPause bits; ps1-bare-metal's example 14 (src/14_cdAudio) for
 * the volume matrix and the SPU's CD input.
 */

#define AUDIO_MODE (CDROM_MODE_CDDA | CDROM_MODE_AUTO_PAUSE | CDROM_MODE_SPEED_1X)

/* GetlocP's track number past the last track. */
#define TRACK_LEAD_OUT 0xaa

/* The matrix's unity gain, and half of it for mono. */
#define ATV_FULL 0x80
#define ATV_HALF 0x40

static TinromResult ensureReady(void) {
	if (!driveReady && (tinromCdReset() != TINROM_OK))
		return TINROM_ERR_NOT_PRESENT;

	return TINROM_OK;
}

/*
 * A command that answers once, with INT3 when it worked. INT5 is the drive
 * refusing - no disc, lid open, a track that is not there - which callers
 * that care tell apart by the status byte that comes with it.
 */
static TinromResult acknowledged(
	uint8_t code, const uint8_t *parameters, unsigned int count,
	CdResponse *response
) {
	TinromResult result = command(code, parameters, count, response,
		CD_TIMEOUT_FAST);

	if (result != TINROM_OK)
		return result;

	uint8_t type = response->type;

	acknowledge();

	return (type == CDROM_IRQ_ACKNOWLEDGE) ? TINROM_OK : TINROM_ERR_IO;
}

static void decodeStatus(uint8_t raw, TinromCdStatus *status) {
	status->raw      = raw;
	status->lidOpen  = (raw & CDROM_CMDSTAT_LID_OPEN) != 0;
	status->spinning = (raw & CDROM_CMDSTAT_SPINDLE_ON) != 0;
	status->playing  = (raw & CDROM_CMDSTAT_PLAYING) != 0;
	status->seeking  = (raw & CDROM_CMDSTAT_SEEKING) != 0;
	status->error    = (raw & CDROM_CMDSTAT_ERROR) != 0;
}

/* A refusal that is really "no disc", from the status byte that came with it. */
static TinromResult refusal(const CdResponse *response) {
	if (response->length && (response->data[0] & CDROM_CMDSTAT_LID_OPEN))
		return TINROM_ERR_NOT_PRESENT;

	return TINROM_ERR_IO;
}

static TinromResult setAudioMode(void) {
	uint8_t    mode = AUDIO_MODE;
	CdResponse response;

	return acknowledged(CDROM_CMD_SETMODE, &mode, 1, &response);
}

/* SetLoc to a whole second of disc time. */
static TinromResult setLocation(unsigned int seconds) {
	if (seconds >= (100 * 60))
		return TINROM_ERR_BAD_ARG;

	uint8_t location[3] = {
		cdrom_encodeBCD((uint8_t) (seconds / 60)),
		cdrom_encodeBCD((uint8_t) (seconds % 60)),
		0
	};
	CdResponse response;

	return acknowledged(CDROM_CMD_SETLOC, location, sizeof(location),
		&response);
}

TinromResult tinromCdAudioInit(bool mono) {
	tinromSpuInit();

	/*
	 * The drive's 2x2 matrix: left to left, left to right, right to right,
	 * right to left, 80h being unity. Stereo passes straight through; mono
	 * sends each input to both outputs at half. New values take effect only
	 * when ADPCTL's CHNGATV is set (psx-spx), which also clears ADPMUTE.
	 */
	setBank(2);
	CDROM_ATV0 = mono ? ATV_HALF : ATV_FULL;
	CDROM_ATV1 = mono ? ATV_HALF : 0;

	setBank(3);
	CDROM_ATV2 = mono ? ATV_HALF : ATV_FULL;
	CDROM_ATV3 = mono ? ATV_HALF : 0;
	CDROM_ADPCTL = CDROM_ADPCTL_CHNGATV;

	tinromSpuSetCdVolume(TINROM_SPU_CD_VOLUME_MAX, TINROM_SPU_CD_VOLUME_MAX);

	return TINROM_OK;
}

TinromResult tinromCdReadToc(TinromCdToc *toc) {
	if (!toc)
		return TINROM_ERR_BAD_ARG;

	memset(toc, 0, sizeof(*toc));

	TinromResult result = ensureReady();

	if (result != TINROM_OK)
		return result;

	CdResponse response;

	result = acknowledged(CDROM_CMD_GETTN, NULL, 0, &response);

	if (result != TINROM_OK)
		return (result == TINROM_ERR_IO) ? TINROM_ERR_NOT_PRESENT : result;
	if (response.length < 3)
		return TINROM_ERR_IO;

	uint8_t first = cdrom_decodeBCD(response.data[1]);
	uint8_t last  = cdrom_decodeBCD(response.data[2]);

	if (!first || (first > last) || (last > TINROM_CD_TRACKS_MAX))
		return TINROM_ERR_CORRUPT;

	/* Each track's start, then track 0 - which GetTD takes as the end. */
	for (unsigned int track = first; ; track++) {
		bool    end       = track > last;
		uint8_t parameter = end ? 0 : cdrom_encodeBCD((uint8_t) track);

		result = acknowledged(CDROM_CMD_GETTD, &parameter, 1, &response);

		if (result != TINROM_OK)
			return result;
		if (response.length < 3)
			return TINROM_ERR_IO;

		uint16_t seconds = (uint16_t) (cdrom_decodeBCD(response.data[1]) * 60
			+ cdrom_decodeBCD(response.data[2]));

		if (end) {
			toc->leadOut = seconds;
			break;
		}

		toc->start[track] = seconds;
	}

	toc->first = first;
	toc->last  = last;

	TINROM_LOG("cd: tracks %u-%u, %u:%02u", (unsigned int) first,
		(unsigned int) last, (unsigned int) (toc->leadOut / 60),
		(unsigned int) (toc->leadOut % 60));

	return TINROM_OK;
}

unsigned int tinromCdTrackLength(const TinromCdToc *toc, unsigned int track) {
	if (!toc || !toc->first || (track < toc->first) || (track > toc->last))
		return 0;

	unsigned int start = toc->start[track];
	unsigned int end   = (track < toc->last) ? toc->start[track + 1]
		: toc->leadOut;

	return (end > start) ? (end - start) : 0;
}

TinromResult tinromCdPlayTrack(const TinromCdToc *toc, unsigned int track) {
	if (!track || (track > TINROM_CD_TRACKS_MAX))
		return TINROM_ERR_BAD_ARG;
	if (toc && ((track < toc->first) || (track > toc->last)))
		return TINROM_ERR_BAD_ARG;

	TinromResult result = ensureReady();

	if (result == TINROM_OK)
		result = setAudioMode();

	/*
	 * The seek is the fallback for a drive that ignores Play's track number:
	 * when it does honour it, the number wins over a pending SetLoc.
	 */
	if ((result == TINROM_OK) && toc)
		result = setLocation(toc->start[track]);

	if (result != TINROM_OK)
		return result;

	uint8_t    parameter = cdrom_encodeBCD((uint8_t) track);
	CdResponse response;

	result = acknowledged(CDROM_CMD_PLAY, &parameter, 1, &response);

	if (result == TINROM_OK)
		audioEnded = false;

	return (result == TINROM_ERR_IO) ? refusal(&response) : result;
}

TinromResult tinromCdPlayAt(unsigned int seconds) {
	TinromResult result = ensureReady();

	if (result == TINROM_OK)
		result = setAudioMode();
	if (result == TINROM_OK)
		result = setLocation(seconds);
	if (result != TINROM_OK)
		return result;

	CdResponse response;

	result = acknowledged(CDROM_CMD_PLAY, NULL, 0, &response);

	if (result == TINROM_OK)
		audioEnded = false;

	return (result == TINROM_ERR_IO) ? refusal(&response) : result;
}

TinromResult tinromCdPause(void) {
	TinromResult result = ensureReady();

	if (result != TINROM_OK)
		return result;

	CdResponse response;

	result = command(CDROM_CMD_PAUSE, NULL, 0, &response, CD_TIMEOUT_FAST);

	if (result != TINROM_OK)
		return result;

	if (response.type != CDROM_IRQ_ACKNOWLEDGE) {
		acknowledge();

		return refusal(&response);
	}

	/* The second answer comes once the drive has stopped. */
	result = secondResponse(&response, CD_TIMEOUT_SLOW);
	acknowledge();

	return result;
}

TinromResult tinromCdResume(void) {
	TinromResult result = ensureReady();

	/* Again: a data read in between leaves the drive in data mode. */
	if (result == TINROM_OK)
		result = setAudioMode();
	if (result != TINROM_OK)
		return result;

	CdResponse response;

	result = acknowledged(CDROM_CMD_PLAY, NULL, 0, &response);

	return (result == TINROM_ERR_IO) ? refusal(&response) : result;
}

TinromResult tinromCdScan(int direction) {
	uint8_t code = (direction > 0) ? CDROM_CMD_FORWARD
		: (direction < 0) ? CDROM_CMD_BACKWARD : CDROM_CMD_PLAY;

	TinromResult result = ensureReady();

	if (result != TINROM_OK)
		return result;

	CdResponse response;

	result = acknowledged(code, NULL, 0, &response);

	return (result == TINROM_ERR_IO) ? refusal(&response) : result;
}

TinromResult tinromCdMute(bool mute) {
	TinromResult result = ensureReady();

	if (result != TINROM_OK)
		return result;

	CdResponse response;

	return acknowledged(mute ? CDROM_CMD_MUTE : CDROM_CMD_DEMUTE, NULL, 0,
		&response);
}

TinromResult tinromCdGetStatus(TinromCdStatus *status) {
	if (!status)
		return TINROM_ERR_BAD_ARG;

	memset(status, 0, sizeof(*status));

	TinromResult result = ensureReady();

	if (result != TINROM_OK)
		return result;

	CdResponse response;

	result = command(CDROM_CMD_NOP, NULL, 0, &response, CD_TIMEOUT_FAST);

	if (result != TINROM_OK)
		return result;

	acknowledge();

	/* An INT5 here still carries the status byte; that is what was asked. */
	if (!response.length)
		return TINROM_ERR_IO;

	decodeStatus(response.data[0], status);

	return TINROM_OK;
}

TinromResult tinromCdGetPosition(TinromCdPosition *position) {
	if (!position)
		return TINROM_ERR_BAD_ARG;

	memset(position, 0, sizeof(*position));

	TinromResult result = ensureReady();

	if (result != TINROM_OK)
		return result;

	CdResponse response;

	result = acknowledged(CDROM_CMD_GETLOCP, NULL, 0, &response);

	if (result != TINROM_OK)
		return result;

	/* No status byte in front of this one: eight bytes of subchannel Q. */
	if (response.length < 8)
		return TINROM_ERR_IO;

	position->leadOut   = response.data[0] == TRACK_LEAD_OUT;
	position->track     = position->leadOut ? 0
		: cdrom_decodeBCD(response.data[0]);
	position->index     = cdrom_decodeBCD(response.data[1]);
	position->minute    = cdrom_decodeBCD(response.data[2]);
	position->second    = cdrom_decodeBCD(response.data[3]);
	position->frame     = cdrom_decodeBCD(response.data[4]);
	position->absMinute = cdrom_decodeBCD(response.data[5]);
	position->absSecond = cdrom_decodeBCD(response.data[6]);
	position->absFrame  = cdrom_decodeBCD(response.data[7]);

	return TINROM_OK;
}

bool tinromCdAudioEnded(void) {
	bool ended = audioEnded;

	audioEnded = false;

	return ended;
}
