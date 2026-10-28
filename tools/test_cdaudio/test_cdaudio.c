/* tinrom - (C) 2026 TinWhisker - MIT licensed
 *
 * Host-side tests of the CD audio half of src/hal/cdrom.c and of
 * src/hal/spu.c, against a fake drive behind the registers (registers.h in
 * this directory). The fake answers the way psx-spx documents: INT3 with the
 * status byte, a second INT2 for Pause and Init, INT5 when it refuses, BCD
 * throughout, and GetlocP with no status byte in front.
 *
 * It keeps time, too, in the microseconds tinromDelayMicroseconds() waits:
 * a data read delivers a sector every 1/300 s, double speed, until it is
 * paused, and Pause's second answer comes only once the drive has stopped -
 * with sectors still landing in between, as they do on a console and in
 * pcsx-redux. The data
 * reads are tested here as well as the audio, since they share the drive.
 *
 * Run by scripts/test-cdaudio.sh.
 */
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "hal/cdrom.h"
#include "hal/spu.h"
#include "ps1/cdromdef.h"
#include "ps1/registers.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("  FAIL %d: %s\n", __LINE__, #c); fails++; } } while (0)

void tinromLogWrite(const char *f, ...) { (void) f; }
/* The fake drive's clock: only waiting moves it. */
static unsigned long long now;

void tinromDelayMicroseconds(int us) { now += (unsigned long long) us; }
const char *tinromResultString(TinromResult r) { (void) r; return "?"; }

volatile uint16_t fakeIrqStat, fakeIrqMask;
volatile uint32_t fakeDev4, fakeDev5;
volatile uint16_t fakeSpu[32];
volatile uint16_t fakeReverb[32];
volatile uint16_t fakeSpuEsa, fakeSpuTsa, fakeSpuRamCtrl;

volatile uintptr_t fakeDmaMadr;
volatile uint32_t  fakeDmaBcr, fakeDmaDpcr;

/* Which half of the capture buffers the SPU is writing: SPUSTAT bit 11. */
static bool         captureSecondHalf;
static unsigned int dmaReads, dmaBadSetups;

/* Sound RAM, in halfwords, and the transfer FIFO in front of it. */
static uint16_t     spuRam[0x80000 / 2];
static uint16_t     spuFifo[32];
static unsigned int spuFifoUsed, spuFifoOverruns, spuSends;

/* --- the fake drive ----------------------------------------------------- */

typedef struct {
	uint8_t            type;
	uint8_t            data[16];
	unsigned int       length, read;
	unsigned long long due;   /* not visible before this */
	uint32_t           lba;   /* an INT1's sector */
} Response;

/*
 * Double speed, as tinrom reads, and how long Pause takes to stop a read:
 * the tests try a quick drive and a slow one.
 */
#define SECTOR_US 3333

static unsigned int pauseUs = 35000;

static struct {
	unsigned int bank;
	uint8_t      params[16];
	unsigned int paramCount;

	Response     queue[16];
	unsigned int queued;

	/* A data read in progress, and the sector buffer being copied out. */
	bool               reading;
	uint32_t           seekLba, readLba;
	unsigned long long nextSector, pauseAt;
	uint32_t           bufferLba;
	unsigned int       bufferPos, bufferLeft;

	/* What was sent, for the checks: command, then its parameters. */
	uint8_t      log[256];
	unsigned int logged;

	uint8_t      atv[4], atvBank[4], adpctl, adpctlBank;
	uint8_t      hintmsk, hintmskBank;

	/* The disc. */
	bool         lidOpen, playing;
	uint8_t      mode, first, last;
	uint16_t     start[100], leadOut;     /* seconds */
	uint8_t      position[8];             /* GetlocP's answer, already BCD */
} drive;

static volatile uint8_t scratch;
static int              pendingWrite = -1;

static uint8_t bcd(unsigned int value) {
	return (uint8_t) (((value / 10) << 4) | (value % 10));
}

static unsigned int unbcd(uint8_t value) {
	return (value >> 4) * 10 + (value & 15);
}

static uint8_t stat(void) {
	return (uint8_t) (CDROM_CMDSTAT_SPINDLE_ON
		| (drive.playing ? CDROM_CMDSTAT_PLAYING : 0)
		| (drive.lidOpen ? CDROM_CMDSTAT_LID_OPEN : 0));
}

/* Into the queue in the order they become due. */
static Response *answerAt(unsigned long long due, uint8_t type,
	const uint8_t *data, unsigned int length) {
	unsigned int at = drive.queued;

	while (at && (drive.queue[at - 1].due > due))
		at--;

	memmove(&drive.queue[at + 1], &drive.queue[at],
		(drive.queued - at) * sizeof(Response));
	drive.queued++;

	Response *r = &drive.queue[at];

	r->type   = type;
	r->length = length;
	r->read   = 0;
	r->due    = due;
	r->lba    = 0;
	memcpy(r->data, data, length);

	return r;
}

static void answer(uint8_t type, const uint8_t *data, unsigned int length) {
	answerAt(now, type, data, length);
}

/* A data read's sectors, each as it comes off the disc, until paused. */
static void tick(void) {
	while (drive.reading && (drive.nextSector <= now) && (drive.queued < 15)) {
		if (drive.pauseAt && (drive.nextSector >= drive.pauseAt)) {
			drive.reading = false;
			break;
		}

		uint8_t   s = (uint8_t) (CDROM_CMDSTAT_SPINDLE_ON | CDROM_CMDSTAT_READING);
		Response *r = answerAt(drive.nextSector, CDROM_IRQ_DATA_READY, &s, 1);

		r->lba = drive.readLba++;
		drive.nextSector += SECTOR_US;
	}

	if (drive.reading && drive.pauseAt && (now >= drive.pauseAt))
		drive.reading = false;
}

static Response *visible(void) {
	tick();

	return (drive.queued && (drive.queue[0].due <= now)) ? &drive.queue[0]
		: NULL;
}

/* What sector `lba` holds: its number, then a pattern from it. */
static uint8_t sectorByte(uint32_t lba, unsigned int i) {
	return (i < 4) ? (uint8_t) (lba >> (i * 8)) : (uint8_t) (lba * 7 + i);
}

static void ack(void) {
	uint8_t s = stat();

	answer(CDROM_IRQ_ACKNOWLEDGE, &s, 1);
}

static void refuse(uint8_t why) {
	uint8_t data[2] = { (uint8_t) (stat() | CDROM_CMDSTAT_ERROR), why };

	answer(CDROM_IRQ_ERROR, data, 2);
}

static void execute(uint8_t code) {
	uint8_t *p = drive.params;

	drive.log[drive.logged++] = code;

	for (unsigned int i = 0; i < drive.paramCount; i++)
		drive.log[drive.logged++] = p[i];

	bool needsDisc = (code != CDROM_CMD_NOP) && (code != CDROM_CMD_INIT)
		&& (code != CDROM_CMD_SETMODE);

	if (needsDisc && drive.lidOpen) {
		refuse(0x80);
	} else {
		switch (code) {
			case CDROM_CMD_READN:
				ack();
				drive.reading    = true;
				drive.pauseAt    = 0;
				drive.readLba    = drive.seekLba;
				drive.nextSector = now + SECTOR_US;
				break;

			case CDROM_CMD_SETLOC:
				drive.seekLba = (unbcd(p[0]) * 60 + unbcd(p[1])) * 75
					+ unbcd(p[2]) - 150;
				ack();
				break;

			case CDROM_CMD_TEST:
				/* 20h: the firmware's date and version, as a vC3 drive. */
				if (p[0] == 0x20) {
					static const uint8_t version[4] = { 0x98, 0x06, 0x10, 0xc3 };

					answer(CDROM_IRQ_ACKNOWLEDGE, version, 4);
				} else {
					refuse(0x10);
				}
				break;

			case CDROM_CMD_NOP:
			case CDROM_CMD_SETMODE:
			case CDROM_CMD_MUTE:
			case CDROM_CMD_DEMUTE:
			case CDROM_CMD_FORWARD:
			case CDROM_CMD_BACKWARD:
				if (code == CDROM_CMD_SETMODE)
					drive.mode = p[0];
				ack();
				break;

			case CDROM_CMD_INIT:
			case CDROM_CMD_STOP:
				ack();
				drive.playing = false;
				ack();
				drive.queue[drive.queued - 1].type = CDROM_IRQ_COMPLETE;
				break;

			case CDROM_CMD_PLAY:
				if (drive.paramCount && p[0]
					&& ((unbcd(p[0]) < drive.first)
					|| (unbcd(p[0]) > drive.last))) {
					refuse(0x10);
					break;
				}
				ack();
				drive.playing = true;
				break;

			case CDROM_CMD_PAUSE: {
				ack();
				drive.playing = false;

				/* A read runs on until the drive has actually stopped. */
				unsigned long long done = now;

				if (drive.reading) {
					drive.pauseAt = now + pauseUs;
					done          = drive.pauseAt;
				}

				uint8_t s = stat();

				answerAt(done, CDROM_IRQ_COMPLETE, &s, 1);
				break;
			}

			case CDROM_CMD_GETTN: {
				uint8_t data[3] = { stat(), bcd(drive.first), bcd(drive.last) };

				answer(CDROM_IRQ_ACKNOWLEDGE, data, 3);
				break;
			}

			case CDROM_CMD_GETTD: {
				unsigned int track = unbcd(p[0]);

				if (track && ((track < drive.first) || (track > drive.last))) {
					refuse(0x10);
					break;
				}

				unsigned int s = track ? drive.start[track] : drive.leadOut;
				uint8_t data[3] = { stat(), bcd(s / 60), bcd(s % 60) };

				answer(CDROM_IRQ_ACKNOWLEDGE, data, 3);
				break;
			}

			case CDROM_CMD_GETLOCP:
				answer(CDROM_IRQ_ACKNOWLEDGE, drive.position, 8);
				break;

			default:
				refuse(0x40);
				break;
		}
	}

	drive.paramCount = 0;
}

static void applyWrite(int which, uint8_t value) {
	switch (which) {
		case FAKE_ADDRESS:   drive.bank = value & 3; break;
		case FAKE_PARAMETER: drive.params[drive.paramCount++] = value; break;

		case FAKE_COMMAND:
			CHECK(drive.bank == 0);
			execute(value);
			break;

		case FAKE_HCHPCTL:
			if ((value & CDROM_HCHPCTL_BFRD) && visible()
				&& (drive.queue[0].type == CDROM_IRQ_DATA_READY)) {
				drive.bufferLba  = drive.queue[0].lba;
				drive.bufferPos  = 0;
				drive.bufferLeft = 2048;
			} else if (!value) {
				drive.bufferLeft = 0;
			}
			break;

		case FAKE_HCLRCTL:
			CHECK(drive.bank == 1);

			if ((value & 7) && visible()) {
				memmove(&drive.queue[0], &drive.queue[1],
					(drive.queued - 1) * sizeof(Response));
				drive.queued--;
			}

			if (value & CDROM_HCLRCTL_CLRPRM)
				drive.paramCount = 0;
			break;

		case FAKE_ATV0: case FAKE_ATV1: case FAKE_ATV2: case FAKE_ATV3:
			drive.atv[which - FAKE_ATV0]     = value;
			drive.atvBank[which - FAKE_ATV0] = (uint8_t) drive.bank;
			break;

		case FAKE_ADPCTL:
			drive.adpctl     = value;
			drive.adpctlBank = (uint8_t) drive.bank;
			break;

		case FAKE_HINTMSK:
			drive.hintmsk     = value;
			drive.hintmskBank = (uint8_t) drive.bank;
			break;

		default:
			break;
	}
}

static void flushWrite(void) {
	if (pendingWrite >= 0)
		applyWrite(pendingWrite, scratch);

	pendingWrite = -1;
}

volatile uint8_t *fakeCdRegister(FakeRegister which) {
	flushWrite();

	Response *head = visible();

	switch (which) {
		case FAKE_HSTS:
			scratch = (uint8_t) (((head && (head->read < head->length))
				? CDROM_HSTS_RSLRRDY : 0)
				| (drive.bufferLeft ? CDROM_HSTS_DRQSTS : 0));
			break;

		case FAKE_HINTSTS:
			CHECK(drive.bank == 1);
			scratch = (uint8_t) (0xe0 | (head ? head->type : 0));
			break;

		case FAKE_RESULT:
			scratch = (head && (head->read < head->length))
				? head->data[head->read++] : 0;
			break;

		case FAKE_RDDATA:
			scratch = drive.bufferLeft
				? sectorByte(drive.bufferLba, drive.bufferPos++) : 0;

			if (drive.bufferLeft)
				drive.bufferLeft--;
			break;

		default:
			pendingWrite = which;
			break;
	}

	return &scratch;
}

volatile uint16_t *fakeSpuStat(void) {
	static volatile uint16_t value;

	/* A manual write goes out at once; the busy bit is never seen set. */
	if (((fakeSpu[FAKE_SPU_ATTR] & SPU_ATTR_XFER_BITMASK)
		== SPU_ATTR_XFER_WRITE) && spuFifoUsed) {
		unsigned int at = (unsigned int) fakeSpuTsa * 4;

		for (unsigned int i = 0; i < spuFifoUsed; i++)
			spuRam[(at + i) % (sizeof(spuRam) / 2)] = spuFifo[i];

		spuFifoUsed = 0;
		spuSends++;
	}

	value = (uint16_t) ((fakeSpu[FAKE_SPU_ATTR] & 0x3f)
		| (captureSecondHalf ? SPU_STATX_CAPTURE_BUF : 0));

	return &value;
}

/*
 * A DMA read from sound RAM: blocks times block size words from the
 * transfer address, if SPUCNT is in DMA-read mode, the bus allows the extra
 * cycles reads need, and the channel is enabled in DPCR.
 */
volatile uint32_t *fakeDmaChcr(void) {
	static volatile uint32_t chcr;

	if (chcr & DMA_CHCR_ENABLE) {
		bool good = ((fakeSpu[FAKE_SPU_ATTR] & SPU_ATTR_XFER_BITMASK)
				== SPU_ATTR_XFER_DMA_READ)
			&& ((fakeDev4 & BIU_DEV_DELAY_DMA_CYCLES_BITMASK)
				== BIU_DEV_DELAY_DMA_CYCLES(2))
			&& (fakeDmaDpcr & DMA_DPCR_CH_ENABLE(DMA_SPU))
			&& !(chcr & DMA_CHCR_WRITE);

		if (good) {
			unsigned int words = (fakeDmaBcr & 0xffff) * (fakeDmaBcr >> 16);
			uint16_t    *out   = (uint16_t *) fakeDmaMadr;

			for (unsigned int i = 0; i < words * 2; i++)
				out[i] = spuRam[(fakeSpuTsa * 4 + i) % (sizeof(spuRam) / 2)];

			dmaReads++;
		} else {
			dmaBadSetups++;
		}

		chcr &= ~DMA_CHCR_ENABLE;
	}

	return &chcr;
}

volatile uint16_t *fakeSpuData(void) {
	static volatile uint16_t discard;

	if (spuFifoUsed >= 32) {
		spuFifoOverruns++;
		return &discard;
	}

	return (volatile uint16_t *) &spuFifo[spuFifoUsed++];
}

/* --- helpers -------------------------------------------------------------- */

static void insertDisc(unsigned int first, unsigned int last) {
	/* A write still in flight belongs to the drive being taken out. */
	flushWrite();
	memset(&drive, 0, sizeof(drive));
	drive.first = (uint8_t) first;
	drive.last  = (uint8_t) last;

	/* Track 1 at 0:02, then each 3:20 longer than the last. */
	for (unsigned int t = first; t <= last; t++)
		drive.start[t] = (uint16_t) (2 + (t - first) * 200);

	drive.leadOut = (uint16_t) (drive.start[last] + 245);
}

static void clearLog(void) {
	flushWrite();
	drive.logged = 0;
}

static bool logIs(const uint8_t *expected, unsigned int length) {
	flushWrite();

	if (drive.logged != length) {
		printf("    log has %u bytes:", drive.logged);
		for (unsigned int i = 0; i < drive.logged; i++)
			printf(" %02x", drive.log[i]);
		printf("\n");
		return false;
	}

	return !memcmp(drive.log, expected, length);
}

/* Nothing queued, and nothing still to come. */
static bool nothingPending(void) {
	flushWrite();
	now += 1000000;
	tick();

	return (drive.queued == 0) && !drive.reading;
}

static bool holds(const uint8_t *sector, uint32_t lba) {
	for (unsigned int i = 0; i < 2048; i++)
		if (sector[i] != sectorByte(lba, i))
			return false;

	return true;
}

/* --- tests ------------------------------------------------------------------- */

int main(void) {
	insertDisc(1, 12);

	puts("reset");
	CHECK(tinromCdReset() == TINROM_OK);
	CHECK(nothingPending());

	puts("table of contents");
	TinromCdToc toc;

	clearLog();
	CHECK(tinromCdReadToc(&toc) == TINROM_OK);
	CHECK(toc.first == 1 && toc.last == 12);
	CHECK(toc.start[1] == 2 && toc.start[12] == 2 + 11 * 200);
	CHECK(toc.leadOut == drive.start[12] + 245);
	CHECK(tinromCdTrackLength(&toc, 1) == 200);
	CHECK(tinromCdTrackLength(&toc, 12) == 245);
	CHECK(tinromCdTrackLength(&toc, 13) == 0 && tinromCdTrackLength(&toc, 0) == 0);
	CHECK(drive.logged == 1 + 13 * 2);
	CHECK(drive.log[0] == CDROM_CMD_GETTN);
	CHECK(drive.log[1] == CDROM_CMD_GETTD && drive.log[2] == 0x01);
	CHECK(drive.log[23] == CDROM_CMD_GETTD && drive.log[24] == 0x12); /* BCD */
	CHECK(drive.log[25] == CDROM_CMD_GETTD && drive.log[26] == 0x00); /* the end */
	CHECK(nothingPending());

	puts("play");
	clearLog();
	CHECK(tinromCdPlayTrack(&toc, 5) == TINROM_OK);
	{
		/* Track 5 starts at 2 + 4 * 200 = 802 s = 13:22. */
		static const uint8_t expect[] = {
			CDROM_CMD_SETMODE, CDROM_MODE_CDDA | CDROM_MODE_AUTO_PAUSE,
			CDROM_CMD_SETLOC, 0x13, 0x22, 0x00,
			CDROM_CMD_PLAY, 0x05
		};
		CHECK(logIs(expect, sizeof(expect)));
	}
	CHECK(drive.playing && !(drive.mode & CDROM_MODE_SPEED_2X));
	CHECK(nothingPending());

	clearLog();
	CHECK(tinromCdPlayTrack(NULL, 12) == TINROM_OK);
	{
		static const uint8_t expect[] = {
			CDROM_CMD_SETMODE, CDROM_MODE_CDDA | CDROM_MODE_AUTO_PAUSE,
			CDROM_CMD_PLAY, 0x12
		};
		CHECK(logIs(expect, sizeof(expect)));
	}

	clearLog();
	CHECK(tinromCdPlayTrack(&toc, 13) == TINROM_ERR_BAD_ARG);
	CHECK(tinromCdPlayTrack(&toc, 0) == TINROM_ERR_BAD_ARG);
	CHECK(drive.logged == 0);

	/* A track the drive does not have: refused, and reported as I/O. */
	CHECK(tinromCdPlayTrack(NULL, 40) == TINROM_ERR_IO);
	CHECK(nothingPending());

	puts("play at");
	clearLog();
	CHECK(tinromCdPlayAt(61 * 60 + 5) == TINROM_OK);
	{
		static const uint8_t expect[] = {
			CDROM_CMD_SETMODE, CDROM_MODE_CDDA | CDROM_MODE_AUTO_PAUSE,
			CDROM_CMD_SETLOC, 0x61, 0x05, 0x00,
			CDROM_CMD_PLAY
		};
		CHECK(logIs(expect, sizeof(expect)));
	}
	CHECK(tinromCdPlayAt(100 * 60) == TINROM_ERR_BAD_ARG);

	puts("pause and resume");
	CHECK(tinromCdPlayTrack(&toc, 2) == TINROM_OK);
	CHECK(tinromCdPause() == TINROM_OK);
	CHECK(!drive.playing && nothingPending());

	TinromCdStatus status;

	CHECK(tinromCdGetStatus(&status) == TINROM_OK);
	CHECK(status.spinning && !status.playing && !status.lidOpen);
	CHECK(tinromCdResume() == TINROM_OK && drive.playing);
	CHECK(tinromCdGetStatus(&status) == TINROM_OK && status.playing);
	CHECK(nothingPending());

	puts("end of track");
	CHECK(!tinromCdAudioEnded());
	{
		/* AutoPause: an INT4 nobody is waiting for. */
		drive.playing = false;
		uint8_t s     = stat();
		answer(CDROM_IRQ_DATA_END, &s, 1);
	}
	CHECK(tinromCdGetStatus(&status) == TINROM_OK && !status.playing);
	CHECK(tinromCdAudioEnded());
	CHECK(!tinromCdAudioEnded());
	CHECK(nothingPending());
	CHECK(tinromCdPlayTrack(&toc, 3) == TINROM_OK && !tinromCdAudioEnded());

	puts("position");
	{
		static const uint8_t q[8] = { 0x03, 0x01, 0x01, 0x23, 0x45,
			0x08, 0x10, 0x74 };
		memcpy(drive.position, q, 8);
	}
	TinromCdPosition position;

	CHECK(tinromCdGetPosition(&position) == TINROM_OK);
	CHECK(position.track == 3 && position.index == 1 && !position.leadOut);
	CHECK(position.minute == 1 && position.second == 23 && position.frame == 45);
	CHECK(position.absMinute == 8 && position.absSecond == 10
		&& position.absFrame == 74);
	drive.position[0] = 0xaa;
	CHECK(tinromCdGetPosition(&position) == TINROM_OK);
	CHECK(position.leadOut && position.track == 0);
	CHECK(nothingPending());

	puts("scan and mute");
	clearLog();
	CHECK(tinromCdScan(1) == TINROM_OK && tinromCdScan(-1) == TINROM_OK);
	CHECK(tinromCdScan(0) == TINROM_OK);
	CHECK(tinromCdMute(true) == TINROM_OK && tinromCdMute(false) == TINROM_OK);
	{
		static const uint8_t expect[] = {
			CDROM_CMD_FORWARD, CDROM_CMD_BACKWARD, CDROM_CMD_PLAY,
			CDROM_CMD_MUTE, CDROM_CMD_DEMUTE
		};
		CHECK(logIs(expect, sizeof(expect)));
	}

	puts("no disc");
	drive.lidOpen = true;
	CHECK(tinromCdReadToc(&toc) == TINROM_ERR_NOT_PRESENT);
	CHECK(toc.first == 0 && toc.last == 0);
	CHECK(tinromCdPlayTrack(NULL, 1) == TINROM_ERR_NOT_PRESENT);
	CHECK(tinromCdPause() == TINROM_ERR_NOT_PRESENT);
	CHECK(tinromCdGetStatus(&status) == TINROM_OK && status.lidOpen);
	CHECK(nothingPending());

	/*
	 * The boot path's pattern: the volume descriptor, then straight away the
	 * root directory and SYSTEM.CNF. Each read is paused when it has its
	 * sectors, and the drive goes on reading until the pause takes: none of
	 * that may spill into the next read.
	 */
	for (unsigned int slow = 0; slow < 2; slow++) {
		static uint8_t sectors[3][2048];

		printf("data reads, back to back, %s pause\n",
			slow ? "a slow" : "a quick");
		insertDisc(1, 1);
		pauseUs = slow ? 70000 : 35000;
		CHECK(tinromCdReset() == TINROM_OK);

		CHECK(tinromCdReadSectors(16, sectors[0], 1) == TINROM_OK);
		CHECK(holds(sectors[0], 16));
		CHECK(tinromCdReadSectors(22, sectors[0], 1) == TINROM_OK);
		CHECK(holds(sectors[0], 22));
		CHECK(tinromCdReadSectors(23, sectors[1], 2) == TINROM_OK);
		CHECK(holds(sectors[1], 23) && holds(sectors[2], 24));
		CHECK(tinromCdReadSectors(16, sectors[0], 1) == TINROM_OK);
		CHECK(holds(sectors[0], 16));

		/*
		 * The fake answers commands the instant they are sent, so a
		 * sector left coming would not beat the next command's answer
		 * here as it does on a drive. That it is left coming at all is
		 * what fails.
		 */
		CHECK(nothingPending());
	}

	puts("odd discs");
	insertDisc(1, 99);
	CHECK(tinromCdReset() == TINROM_OK);
	CHECK(tinromCdReadToc(&toc) == TINROM_OK && toc.last == 99);
	CHECK(tinromCdTrackLength(&toc, 99) == 245);
	insertDisc(5, 3);
	CHECK(tinromCdReset() == TINROM_OK);
	CHECK(tinromCdReadToc(&toc) == TINROM_ERR_CORRUPT);
	CHECK(nothingPending());

	puts("audio setup");
	insertDisc(1, 2);
	CHECK(tinromCdReset() == TINROM_OK);
	memset((void *) fakeSpu, 0, sizeof(fakeSpu));
	fakeSpu[FAKE_SPU_EVOLL] = 0x1234;
	CHECK(tinromCdAudioInit(false) == TINROM_OK);
	flushWrite();
	CHECK(drive.atv[0] == 0x80 && drive.atv[1] == 0 && drive.atv[2] == 0x80
		&& drive.atv[3] == 0);
	CHECK(drive.atvBank[0] == 2 && drive.atvBank[1] == 2
		&& drive.atvBank[2] == 3 && drive.atvBank[3] == 3);
	CHECK(drive.adpctl == CDROM_ADPCTL_CHNGATV && drive.adpctlBank == 3);
	CHECK(fakeDev4 == 0x200931e1);
	CHECK(fakeSpu[FAKE_SPU_ATTR] == (SPU_ATTR_ENABLE | SPU_ATTR_DAC_ENABLE
		| SPU_ATTR_I2SA_ENABLE));
	CHECK(fakeSpu[FAKE_SPU_MVOLL] == 0x3fff && fakeSpu[FAKE_SPU_MVOLR] == 0x3fff);
	CHECK(fakeSpu[FAKE_SPU_AVOLL] == 0x7fff && fakeSpu[FAKE_SPU_AVOLR] == 0x7fff);
	CHECK(fakeSpu[FAKE_SPU_EVOLL] == 0 && fakeSpu[FAKE_SPU_KOF0] == 0xffff
		&& fakeSpu[FAKE_SPU_KOF1] == 0x00ff);

	CHECK(tinromCdAudioInit(true) == TINROM_OK);
	flushWrite();
	CHECK(drive.atv[0] == 0x40 && drive.atv[1] == 0x40 && drive.atv[2] == 0x40
		&& drive.atv[3] == 0x40);

	tinromSpuSetCdVolume(0, 0);
	CHECK(!(fakeSpu[FAKE_SPU_ATTR] & SPU_ATTR_I2SA_ENABLE));
	CHECK(fakeSpu[FAKE_SPU_ATTR] & SPU_ATTR_DAC_ENABLE);

	/* Init again keeps the CD input as it was left. */
	tinromSpuSetCdVolume(0x4000, 0x4000);
	tinromSpuInit();
	CHECK(fakeSpu[FAKE_SPU_ATTR] & SPU_ATTR_I2SA_ENABLE);

	puts("reverb");
	/* Whatever ran before left sound RAM full of something. */
	for (unsigned int i = 0; i < sizeof(spuRam) / 2; i++)
		spuRam[i] = 0xa5a5;

	CHECK(fakeSpu[FAKE_SPU_EVOLL] == 0);
	CHECK(!(fakeSpu[FAKE_SPU_ATTR] & SPU_ATTR_REVERB_ENABLE));

	for (unsigned int setting = TINROM_SPU_REVERB_STUDIO;
		setting < TINROM_SPU_REVERB_COUNT; setting++) {
		spuSends = 0;
		tinromSpuSetCdReverb((TinromSpuReverb) setting);

		uint16_t attr = fakeSpu[FAKE_SPU_ATTR];

		CHECK(attr & SPU_ATTR_REVERB_ENABLE);
		CHECK(attr & SPU_ATTR_I2SA_REVERB);
		CHECK((attr & SPU_ATTR_XFER_BITMASK) == SPU_ATTR_XFER_NONE);
		CHECK(fakeSpu[FAKE_SPU_EVOLL] && fakeSpu[FAKE_SPU_EVOLL]
			== fakeSpu[FAKE_SPU_EVOLR]);
		CHECK((int16_t) fakeSpu[FAKE_SPU_EVOLL] > 0);

		/* The area is the top 24 KB, cleared in 64-byte sends. */
		CHECK(fakeSpuEsa == (0x80000 - 0x6000) / 8);
		CHECK(spuSends == 0x6000 / 64 && !spuFifoOverruns);

		bool cleared = true, spared = true;

		for (unsigned int i = 0; i < sizeof(spuRam) / 2; i++) {
			if (i >= (0x80000 - 0x6000) / 2)
				cleared &= (spuRam[i] == 0);
			else
				spared &= (spuRam[i] == 0xa5a5);
		}
		CHECK(cleared && spared);

		/*
		 * Every address register inside the area. And each reflection reads
		 * behind where it writes, each tap inside its own line.
		 */
		for (unsigned int r = 10; r <= 29; r++)
			CHECK(fakeReverb[r] > 0 && fakeReverb[r] < 0x6000 / 8);

		CHECK(fakeReverb[16] < fakeReverb[10] && fakeReverb[17] < fakeReverb[11]);
		CHECK(fakeReverb[24] < fakeReverb[18] && fakeReverb[25] < fakeReverb[19]);
		CHECK(fakeReverb[12] < fakeReverb[10] && fakeReverb[14] < fakeReverb[10]);
		CHECK(fakeReverb[21] < fakeReverb[19] && fakeReverb[23] < fakeReverb[19]);
		CHECK(fakeReverb[26] > fakeReverb[0] && fakeReverb[29] > fakeReverb[1]);
		CHECK(fakeReverb[30] && fakeReverb[30] == fakeReverb[31]);
		CHECK((int16_t) fakeReverb[7] > 0); /* vWALL */

		/* Leave a mark to see the next setting clear it. */
		spuRam[0x7fffe / 2] = 0x1234;
	}

	/*
	 * The hall, worked out by hand from its room in spu.c: the left line
	 * starts at 1 and is as long as its 187-unit wall, the right one a
	 * sixteenth longer. Then a mono toggle, which must keep it.
	 */
	tinromSpuSetCdReverb(TINROM_SPU_REVERB_HALL);
	CHECK(fakeReverb[10] == 188 && fakeReverb[16] == 1);  /* mLSAME, dLSAME */
	CHECK(fakeReverb[11] == 189 + 198 && fakeReverb[17] == 189); /* right +1/16 */
	CHECK(fakeReverb[12] == 188 - 127 && fakeReverb[14] == 188 - 160);
	CHECK(tinromCdAudioInit(true) == TINROM_OK);
	CHECK(fakeSpu[FAKE_SPU_ATTR] & SPU_ATTR_REVERB_ENABLE);
	CHECK(fakeSpu[FAKE_SPU_EVOLL] == 0x799a);

	tinromSpuSetCdReverb(TINROM_SPU_REVERB_OFF);
	CHECK(fakeSpu[FAKE_SPU_EVOLL] == 0 && fakeSpu[FAKE_SPU_EVOLR] == 0);
	CHECK(!(fakeSpu[FAKE_SPU_ATTR]
		& (SPU_ATTR_REVERB_ENABLE | SPU_ATTR_I2SA_REVERB)));
	CHECK(fakeSpu[FAKE_SPU_ATTR] & SPU_ATTR_I2SA_ENABLE);

	puts("capture");
	{
		static int16_t left[TINROM_SPU_CAPTURE_SAMPLES] __attribute__((aligned(4)));
		static int16_t right[TINROM_SPU_CAPTURE_SAMPLES] __attribute__((aligned(4)));

		/* Each halfword of the capture buffers holds its own address. */
		for (unsigned int i = 0; i < 0x800 / 2; i++)
			spuRam[i] = (uint16_t) (i * 2);

		dmaReads = dmaBadSetups = 0;
		captureSecondHalf = true; /* so the first half is whole */
		CHECK(tinromSpuReadCdCapture(left, right));
		CHECK(dmaReads == 2 && !dmaBadSetups);
		CHECK(left[0] == 0x000 && left[255] == 0x1fe);
		CHECK(right[0] == 0x400 && right[255] == 0x5fe);

		captureSecondHalf = false;
		CHECK(tinromSpuReadCdCapture(left, right));
		CHECK(left[0] == 0x200 && left[255] == 0x3fe);
		CHECK(right[0] == 0x600 && right[255] == 0x7fe);

		/* The bus is back to the BIOS's timing, and no transfer is left on. */
		CHECK(fakeDev4 == 0x200931e1);
		CHECK((fakeSpu[FAKE_SPU_ATTR] & SPU_ATTR_XFER_BITMASK)
			== SPU_ATTR_XFER_NONE);

		/* And the reverb, which was off, still is. */
		CHECK(!(fakeSpu[FAKE_SPU_ATTR] & SPU_ATTR_REVERB_ENABLE));
	}

	puts("drive version");
	{
		TinromDriveInfo info;

		insertDisc(1, 2);
		CHECK(tinromCdReset() == TINROM_OK);
		CHECK(tinromCdDriveInfo(&info) == TINROM_OK);
		CHECK(!strcmp(info.version, "vC3, 1998-06-10"));
		CHECK(nothingPending());
	}

	puts("handover");
	{
		/* tinrom polls, so its reset leaves the drive's interrupt masked. */
		insertDisc(1, 2);
		CHECK(tinromCdReset() == TINROM_OK);
		flushWrite();
		CHECK(drive.hintmsk == 0 && drive.hintmskBank == 1);

		/* An answer still waiting, as a command's late one might be. */
		answer(CDROM_IRQ_COMPLETE, NULL, 0);

		/* Handed over: nothing pending, and unmasked as the BIOS leaves it. */
		tinromCdHandover();
		flushWrite();
		CHECK(drive.hintmsk == 0x1f && drive.hintmskBank == 1);
		CHECK(drive.queued == 0);
		CHECK(drive.bank == 0);
	}

	printf(fails ? "%d failure(s)\n" : "all passed (0 failures)\n", fails);

	return fails ? 1 : 0;
}
