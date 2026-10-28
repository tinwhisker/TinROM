/* tinrom - (C) 2026 TinWhisker - MIT licensed
 *
 * Memory cards over SIO0: probe, raw frame read, directory listing.
 *
 * Every command, frame layout and constant below is documented hardware
 * behaviour, taken from psx-spx "Controller and Memory Card" (see NOTICE.md).
 * The bus underneath - /CS, byte exchange, the /ACK handshake - is shared with
 * the controller driver and lives in hal/sio0.c.
 *
 * Reading, writing single frames, directory surgery (delete, undelete, copy)
 * and whole named files are real. Writing a full image, formatting and the
 * destructive capacity test are still stubs.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "core/log.h"
#include "hal/mcard.h"
#include "hal/sio0.h"
#include "hal/timer.h"
#include "libc/string.h"

#define MC_PORTS  2
#define MC_FRAMES (TINROM_MC_CARD_SIZE / TINROM_MC_FRAME_SIZE) /* 1024 */
#define MC_FRAMES_PER_BLOCK \
	(TINROM_MC_BLOCK_SIZE / TINROM_MC_FRAME_SIZE)              /* 64 */

#define MC_ADDRESS  0x81 /* memory card, as opposed to 01h for a controller */
#define MC_CMD_READ  0x52 /* 'R' */
#define MC_CMD_WRITE 0x57 /* 'W' */

#define MC_ID1      0x5a
#define MC_ID2      0x5d
#define MC_ACK1     0x5c
#define MC_ACK2     0x5d
#define MC_END_GOOD     0x47 /* 'G' */
#define MC_END_BAD_SUM  0x4e /* 'N' - the card disagreed with our checksum */
#define MC_END_BAD_SECT 0xff /* the sector number was out of range */

/*
 * Read Data Command, from psx-spx:
 *
 *   send  reply  meaning
 *   81h   N/A    address; nothing is driving the line yet
 *   52h   FLAG   command, and the card's status byte
 *   00h   5Ah    ID1
 *   00h   5Dh    ID2
 *   MSB   (00h)  sector number, high byte
 *   LSB   (pre)  sector number, low byte
 *   00h   5Ch    acknowledge 1   <- the late /ACK lands after this one
 *   00h   5Dh    acknowledge 2
 *   00h   MSB    confirmed sector, high byte
 *   00h   LSB    confirmed sector, low byte
 *   00h   ...    128 data bytes
 *   00h   CHK    MSB xor LSB xor every data byte
 *   00h   47h    end byte, "G" for good
 *
 * /ACK is a "more data" request, so the last byte of a packet does not get
 * one - waiting for it after the end byte would always time out.
 *
 * A sector number a card does not have is refused rather than wrapped:
 * genuine Sony cards answer FFFFh as the confirmed sector and send nothing
 * further; third-party cards answer with the number ANDed with 3FFh and then
 * hand over the wrong sector. Comparing the confirmed sector catches both.
 */

/* Directory frame (block 0, frames 1..15). */
#define DIR_STATE      0x00
#define DIR_SIZE       0x04
#define DIR_NEXT       0x08
#define DIR_NAME       0x0a
#define DIR_NAME_CHARS 20

#define DIR_STATE_FIRST   0x51 /* in use, first or only block */
#define DIR_STATE_MIDDLE  0x52 /* in use, middle block */
#define DIR_STATE_LAST    0x53 /* in use, last block */
#define DIR_STATE_FORMAT  0xa0 /* free, freshly formatted */
#define DIR_STATE_DEL_1ST 0xa1 /* free, deleted, was the first block */
#define DIR_STATE_DEL_MID 0xa2 /* free, deleted, was a middle block */
#define DIR_STATE_DEL_END 0xa3 /* free, deleted, was the last block */

#define DIR_NEXT_NONE 0xffff

/* Title frame (block 1..15, frame 0). */
#define TITLE_MAGIC_0    0x53 /* 'S' */
#define TITLE_MAGIC_1    0x43 /* 'C' */
#define TITLE_ICON_FLAG  0x02
#define TITLE_TEXT       0x04
#define TITLE_TEXT_BYTES 64   /* 32 wide characters */
#define TITLE_CLUT       0x60 /* 16 entries, 16 bits each */

/*
 * Icon display flag. psx-spx: 11h is one frame, 12h two (changing every 16
 * PAL frames), 13h three (every 11). Anything else is treated as a corrupt
 * file - the BIOS boot menu will not even list it.
 */
#define TITLE_ICON_MIN 0x11
#define TITLE_ICON_MAX 0x13

#define ICON_FRAME_BYTES (TINROM_MC_ICON_SIZE / 2 * TINROM_MC_ICON_SIZE)
#define ICON_ROW_BYTES   (TINROM_MC_ICON_SIZE / 2)

#define FRAME_CHECKSUM (TINROM_MC_FRAME_SIZE - 1)

static uint8_t frameBuffer[TINROM_MC_FRAME_SIZE];

/* ------------------------------------------------------------------------ */

static uint16_t read16(const uint8_t *p) {
	return (uint16_t) (p[0] | ((uint16_t) p[1] << 8));
}

static uint32_t read32(const uint8_t *p) {
	return (uint32_t) p[0] | ((uint32_t) p[1] << 8)
		| ((uint32_t) p[2] << 16) | ((uint32_t) p[3] << 24);
}

/** Every frame carries an XOR of its first 127 bytes in the last one. */
static bool frameChecksumOk(const uint8_t *frame) {
	uint8_t sum = 0;

	for (unsigned int i = 0; i < FRAME_CHECKSUM; i++)
		sum ^= frame[i];

	return sum == frame[FRAME_CHECKSUM];
}

/*
 * Why the last command header on each port failed, for the boot log - once
 * per change, because the settings screen asks whether a card is there on
 * every frame it draws. 0 once one succeeds.
 */
static uint32_t lastFault[MC_PORTS];

static TinromResult headerFault(
	unsigned int port, TinromResult result, unsigned int step,
	uint8_t status, uint8_t id1, uint8_t id2
) {
	/* step + 1, so that no fault at all (0) is never one of them. */
	uint32_t fault = ((uint32_t) (step + 1) << 24) | ((uint32_t) status << 16)
		| ((uint32_t) id1 << 8) | id2;

	if ((port < MC_PORTS) && (fault != lastFault[port])) {
		static const char *const what[] = {
			"no /ACK after the address",
			"no /ACK after the command",
			"no /ACK after ID 1",
			"no /ACK after ID 2",
			"not a card's IDs"
		};

		lastFault[port] = fault;
		TINROM_LOG("mc%u: %s (flag %02x, ids %02x %02x)", port,
			what[step], status, id1, id2);
	}

	return result;
}

/*
 * A card programs its flash after the write command has finished on the
 * wire, and does not answer properly until that is done. pcsx-redux writes
 * at once; on a console the next frame's write found "no card", and then -
 * once that was waited out - a card that answered its address but not with
 * its IDs. So after a write, a command header that fails in any way is
 * tried again for up to MC_BUSY_MAX_MS, a millisecond apart; a card that
 * was never there costs no more than before, because only a write starts
 * the wait.
 */
#define MC_BUSY_MAX_MS 100

static bool     writeSettling[MC_PORTS];
static unsigned longestBusyMs;

/* How far one attempt at a command's header got, and what came back. */
typedef struct {
	unsigned int step;
	uint8_t      status, id1, id2;
} HeaderReply;

static TinromResult tryHeader(
	unsigned int port, uint8_t command, HeaderReply *reply
) {
	reply->step   = 0;
	reply->status = 0;
	reply->id1    = 0;
	reply->id2    = 0;

	tinromSio0Select(port);

	(void) tinromSio0Exchange(MC_ADDRESS);

	/* Presence is the /ACK, never the byte alongside the address. */
	if (!tinromSio0WaitAck(TINROM_SIO0_ACK_CARD))
		return TINROM_ERR_NOT_PRESENT;

	reply->step   = 1;
	reply->status = tinromSio0Exchange(command);

	if (!tinromSio0WaitAck(TINROM_SIO0_ACK_CARD))
		return TINROM_ERR_IO;

	reply->step = 2;
	reply->id1  = tinromSio0Exchange(0x00);

	if (!tinromSio0WaitAck(TINROM_SIO0_ACK_CARD))
		return TINROM_ERR_IO;

	reply->step = 3;
	reply->id2  = tinromSio0Exchange(0x00);

	if (!tinromSio0WaitAck(TINROM_SIO0_ACK_CARD))
		return TINROM_ERR_IO;

	/* Something answered 81h but is not a card - a multitap, say. */
	reply->step = 4;

	if ((reply->id1 != MC_ID1) || (reply->id2 != MC_ID2))
		return TINROM_ERR_UNSUPPORTED;

	return TINROM_OK;
}

/**
 * Address the card and send a command, leaving /CS asserted and the card
 * ready for the command's own bytes. The caller always deselects.
 *
 * Straight after a write, a header that fails in any way is tried again: a
 * card still programming may not answer at all, or - as one on a console
 * did - answer its address and then not send its IDs, which read as "not
 * a card" and failed a save as "unsupported here".
 */
static TinromResult beginCommand(
	unsigned int port, uint8_t command, uint8_t *flag
) {
	HeaderReply  reply;
	TinromResult result = tryHeader(port, command, &reply);

	if ((result != TINROM_OK) && (port < MC_PORTS) && writeSettling[port]) {
		for (unsigned int waited = 1; waited <= MC_BUSY_MAX_MS; waited++) {
			tinromSio0Deselect();
			tinromDelayMicroseconds(1000);

			result = tryHeader(port, command, &reply);

			if (result == TINROM_OK) {
				if (waited > longestBusyMs) {
					longestBusyMs = waited;
					TINROM_LOG("mc%u: busy for about %u ms after a write",
						port, waited);
				}

				break;
			}
		}
	}

	/* Waited out or not, a card gone quiet is not waited for again. */
	if (port < MC_PORTS)
		writeSettling[port] = false;

	if (flag)
		*flag = reply.status;

	if (result != TINROM_OK)
		return headerFault(port, result, reply.step, reply.status, reply.id1,
			reply.id2);

	if (port < MC_PORTS)
		lastFault[port] = 0;

	return TINROM_OK;
}

/* Everything past the command header. The caller deselects on every path. */
static TinromResult readFrameSelected(
	unsigned int port, uint16_t frame, uint8_t *out
) {
	TinromResult result = beginCommand(port, MC_CMD_READ, NULL);

	if (result != TINROM_OK)
		return result;

	uint8_t msb = (uint8_t) (frame >> 8);
	uint8_t lsb = (uint8_t) (frame & 0xff);

	(void) tinromSio0Exchange(msb);

	if (!tinromSio0WaitAck(TINROM_SIO0_ACK_CARD))
		return TINROM_ERR_IO;

	(void) tinromSio0Exchange(lsb);

	if (!tinromSio0WaitAck(TINROM_SIO0_ACK_CARD))
		return TINROM_ERR_IO;

	uint8_t ack1 = tinromSio0Exchange(0x00);

	/* Sony cards stall for ~31000 cycles before acknowledging this byte. */
	if (!tinromSio0WaitAck(TINROM_SIO0_ACK_WRITE))
		return TINROM_ERR_IO;

	uint8_t ack2 = tinromSio0Exchange(0x00);

	if (!tinromSio0WaitAck(TINROM_SIO0_ACK_CARD))
		return TINROM_ERR_IO;

	if ((ack1 != MC_ACK1) || (ack2 != MC_ACK2))
		return TINROM_ERR_IO;

	uint8_t gotMsb = tinromSio0Exchange(0x00);

	if (!tinromSio0WaitAck(TINROM_SIO0_ACK_CARD))
		return TINROM_ERR_IO;

	uint8_t gotLsb = tinromSio0Exchange(0x00);

	if (!tinromSio0WaitAck(TINROM_SIO0_ACK_CARD))
		return TINROM_ERR_IO;

	if ((gotMsb != msb) || (gotLsb != lsb))
		return TINROM_ERR_BAD_ARG;

	uint8_t checksum = (uint8_t) (msb ^ lsb);

	for (unsigned int i = 0; i < TINROM_MC_FRAME_SIZE; i++) {
		uint8_t value = tinromSio0Exchange(0x00);

		out[i]    = value;
		checksum ^= value;

		if (!tinromSio0WaitAck(TINROM_SIO0_ACK_CARD))
			return TINROM_ERR_IO;
	}

	uint8_t expected = tinromSio0Exchange(0x00);

	if (!tinromSio0WaitAck(TINROM_SIO0_ACK_CARD))
		return TINROM_ERR_IO;

	/* Last byte of the packet: no /ACK follows it. */
	uint8_t end = tinromSio0Exchange(0x00);

	if (end != MC_END_GOOD)
		return TINROM_ERR_IO;

	if (checksum != expected)
		return TINROM_ERR_CORRUPT;

	return TINROM_OK;
}

/*
 * The write command, 57h. Same shape as the read: address, command, sector,
 * then 128 bytes - but the checksum goes UP to the card, and it is the card
 * that judges it.
 *
 * The card erases and programs flash after the last data byte, so the
 * acknowledgements around the checksum use the long timeout rather than the
 * ordinary one.
 *
 * The end byte is the whole result. 'G' is a completed write; 'N' means the
 * card computed a different checksum, so nothing was written; FFh means it
 * rejected the sector number. Nothing here reads the frame back - see
 * docs/memory-cards.md for why, and what that leaves uncovered.
 */
static TinromResult writeFrameSelected(
	unsigned int port, uint16_t frame, const uint8_t *in
) {
	TinromResult result = beginCommand(port, MC_CMD_WRITE, NULL);

	if (result != TINROM_OK)
		return result;

	uint8_t msb = (uint8_t) (frame >> 8);
	uint8_t lsb = (uint8_t) (frame & 0xff);

	(void) tinromSio0Exchange(msb);

	if (!tinromSio0WaitAck(TINROM_SIO0_ACK_CARD))
		return TINROM_ERR_IO;

	(void) tinromSio0Exchange(lsb);

	if (!tinromSio0WaitAck(TINROM_SIO0_ACK_CARD))
		return TINROM_ERR_IO;

	uint8_t checksum = (uint8_t) (msb ^ lsb);

	for (unsigned int i = 0; i < TINROM_MC_FRAME_SIZE; i++) {
		(void) tinromSio0Exchange(in[i]);

		checksum ^= in[i];

		if (!tinromSio0WaitAck(TINROM_SIO0_ACK_CARD))
			return TINROM_ERR_IO;
	}

	(void) tinromSio0Exchange(checksum);

	if (!tinromSio0WaitAck(TINROM_SIO0_ACK_WRITE))
		return TINROM_ERR_IO;

	uint8_t ack1 = tinromSio0Exchange(0x00);

	if (!tinromSio0WaitAck(TINROM_SIO0_ACK_WRITE))
		return TINROM_ERR_IO;

	uint8_t ack2 = tinromSio0Exchange(0x00);

	if (!tinromSio0WaitAck(TINROM_SIO0_ACK_WRITE))
		return TINROM_ERR_IO;

	if ((ack1 != MC_ACK1) || (ack2 != MC_ACK2))
		return TINROM_ERR_IO;

	/* Last byte of the packet: no /ACK follows it. */
	switch (tinromSio0Exchange(0x00)) {
		case MC_END_GOOD:
			return TINROM_OK;

		case MC_END_BAD_SUM:
			TINROM_LOG("mc%u: frame %u refused, checksum", port, frame);

			return TINROM_ERR_VERIFY;

		case MC_END_BAD_SECT:
			TINROM_LOG("mc%u: frame %u refused, bad sector", port, frame);

			return TINROM_ERR_BAD_ARG;

		default:
			return TINROM_ERR_IO;
	}
}

/* ------------------------------------------------------------------------ */
/* Shift-JIS titles.
 *
 * psx-spx documents the encoding used in title frames:
 *
 *   81h,40h      --> SPC
 *   81h,43h..97h --> punctuation marks
 *   82h,4Fh..58h --> "0..9"
 *   82h,60h..79h --> "A..Z"
 *   82h,81h..9Ah --> "a..z"
 *
 * The punctuation table below is the Unicode NFKC folding of each of those
 * code points, generated rather than typed, so a full-width comma becomes a
 * comma and nothing is invented. The rest are deliberate approximations,
 * marked below: the dashes, the wave dash, the curly quotes, the acute
 * accent and the primes have no ASCII equivalent, but every save manager
 * renders them this way - and games use them: Spyro 2's title has a curly
 * double quote where "Ripto's" has its apostrophe.
 *
 * Anything with no ASCII form - kana, kanji - becomes '?'. tinrom's font is
 * ASCII, so the alternative is a blank.
 */

#define SJIS_PUNCT_FIRST 0x8140
#define SJIS_PUNCT_LAST  0x8197

static const char sjisPunctuation[] = {
	 ' ',    0,    0,  ',',  '.',    0,  ':',  ';',  /* 8140 */
	 '?',  '!',    0,    0, '\'',  '`',    0,  '^',  /* 8148 */ /* 814c approximated */
	   0,  '_',    0,    0,    0,    0,    0,    0,  /* 8150 */
	   0,    0,    0,  '-',  '-',  '-',  '/', '\\',  /* 8158 */ /* 815b-815d approximated */
	 '~',    0,  '|',    0,    0, '\'', '\'',  '"',  /* 8160 */ /* 8160, 8165-8167 approximated */
	 '"',  '(',  ')',    0,    0,  '[',  ']',  '{',  /* 8168 */ /* 8168 approximated */
	 '}',    0,    0,    0,    0,    0,    0,    0,  /* 8170 */
	   0,    0,    0,  '+',  '-',    0,    0,    0,  /* 8178 */ /* 817c approximated */
	   0,  '=',    0,  '<',  '>',    0,    0,    0,  /* 8180 */
	   0,    0,    0,    0, '\'',  '"',    0,    0,  /* 8188 */ /* 818c-818d approximated */
	 '$',    0,    0,  '%',  '#',  '&',  '*',  '@'   /* 8190 */
};

static char sjisToAscii(uint16_t wide) {
	if ((wide >= SJIS_PUNCT_FIRST) && (wide <= SJIS_PUNCT_LAST))
		return sjisPunctuation[wide - SJIS_PUNCT_FIRST];

	if ((wide >= 0x824f) && (wide <= 0x8258))
		return (char) ('0' + (wide - 0x824f));

	if ((wide >= 0x8260) && (wide <= 0x8279))
		return (char) ('A' + (wide - 0x8260));

	if ((wide >= 0x8281) && (wide <= 0x829a))
		return (char) ('a' + (wide - 0x8281));

	return 0;
}

static void decodeTitle(char *out, size_t size, const uint8_t *frame) {
	size_t written = 0;

	out[0] = '\0';

	for (unsigned int i = 0; (i < TITLE_TEXT_BYTES) && ((written + 1) < size);) {
		uint8_t high = frame[TITLE_TEXT + i];

		/* Titles shorter than 32 characters are padded with zeroes. */
		if (!high)
			break;

		/*
		 * The BIOS card manager accepts plain 8-bit characters here as well
		 * as wide ones, and some games use them.
		 */
		if ((high >= 0x20) && (high <= 0x7e)) {
			out[written++] = (char) high;
			i++;

			continue;
		}

		if ((i + 1) >= TITLE_TEXT_BYTES)
			break;

		char ch = sjisToAscii(
			(uint16_t) (((uint16_t) high << 8) | frame[TITLE_TEXT + i + 1])
		);

		out[written++] = ch ? ch : '?';
		i += 2;
	}

	/* Titles are routinely padded out with spaces; they are not a name. */
	while (written && (out[written - 1] == ' '))
		written--;

	out[written] = '\0';
}

/* ------------------------------------------------------------------------ */

TinromResult tinromMcInit(void) {
	/* The SIO0 bus is brought up by tinromHalInit(). Nothing else to do. */
	return TINROM_OK;
}

TinromResult tinromMcProbe(unsigned int port, TinromMcInfo *info) {
	if (!info || (port >= MC_PORTS))
		return TINROM_ERR_BAD_ARG;

	memset(info, 0, sizeof(*info));

	uint8_t      flag   = 0;
	TinromResult result = beginCommand(port, MC_CMD_READ, &flag);

	/* Dropping /CS aborts the read cleanly; a probe needs nothing more. */
	tinromSio0Deselect();

	if (result != TINROM_OK)
		return result;

	info->present      = true;
	info->flag         = flag;
	info->detectedSize = TINROM_MC_CARD_SIZE;

	/* STUB: only tinromMcVerifyCapacity() can actually rule this out. */
	info->suspectCapacity = false;

	return TINROM_OK;
}

TinromResult tinromMcReadFrame(unsigned int port, uint16_t frame, void *data) {
	if (!data || (port >= MC_PORTS) || (frame >= MC_FRAMES))
		return TINROM_ERR_BAD_ARG;

	TinromResult result = readFrameSelected(port, frame, (uint8_t *) data);

	tinromSio0Deselect();

	return result;
}

TinromResult tinromMcReadDirectory(unsigned int port, TinromMcSlot *slots) {
	if (!slots || (port >= MC_PORTS))
		return TINROM_ERR_BAD_ARG;

	memset(slots, 0, sizeof(*slots) * TINROM_MC_SAVE_SLOTS);

	uint16_t     next[TINROM_MC_SAVE_SLOTS];
	unsigned int corrupt = 0;

	/* Pass one: the fifteen directory frames. */
	for (unsigned int i = 0; i < TINROM_MC_SAVE_SLOTS; i++) {
		TinromMcSlot *slot = &slots[i];

		next[i] = DIR_NEXT_NONE;

		TinromResult result =
			tinromMcReadFrame(port, (uint16_t) (1 + i), frameBuffer);

		if (result != TINROM_OK)
			return result;

		if (!frameChecksumOk(frameBuffer)) {
			corrupt++;

			continue;
		}

		switch (frameBuffer[DIR_STATE]) {
			case DIR_STATE_FIRST:
				slot->state = TINROM_MC_SLOT_USED;
				break;

			case DIR_STATE_DEL_1ST:
				slot->state   = TINROM_MC_SLOT_DELETED;
				slot->deleted = true;
				break;

			case DIR_STATE_MIDDLE:
			case DIR_STATE_LAST:
				slot->state = TINROM_MC_SLOT_LINK;
				break;

			case DIR_STATE_DEL_MID:
			case DIR_STATE_DEL_END:
				slot->state   = TINROM_MC_SLOT_LINK;
				slot->deleted = true;
				break;

			default: /* A0h, and anything a formatter left behind. */
				slot->state = TINROM_MC_SLOT_FREE;
				break;
		}

		next[i] = read16(&frameBuffer[DIR_NEXT]);

		/*
		 * Size and filename are only stored in a file's first entry; the
		 * continuation entries have those bytes zeroed.
		 */
		if ((slot->state != TINROM_MC_SLOT_USED)
			&& (slot->state != TINROM_MC_SLOT_DELETED))
			continue;

		slot->size = read32(&frameBuffer[DIR_SIZE]);

		memcpy(slot->filename, &frameBuffer[DIR_NAME], DIR_NAME_CHARS);
		slot->filename[DIR_NAME_CHARS] = '\0';

		for (unsigned int c = 0; c < DIR_NAME_CHARS; c++) {
			char ch = slot->filename[c];

			if (!ch)
				break;
			if ((ch < 0x20) || (ch > 0x7e))
				slot->filename[c] = '?';
		}
	}

	/*
	 * Pass two: block counts, by walking each file's chain. The chain is what
	 * decides which cells the browser joins together, so it is what we count,
	 * rather than trusting the size field to agree with it.
	 */
	for (unsigned int i = 0; i < TINROM_MC_SAVE_SLOTS; i++) {
		TinromMcSlot *slot = &slots[i];

		if ((slot->state != TINROM_MC_SLOT_USED)
			&& (slot->state != TINROM_MC_SLOT_DELETED))
			continue;

		unsigned int blocks = 1;
		uint16_t     link   = next[i];

		/* Capped so a corrupt card cannot spin here forever. */
		while ((link < TINROM_MC_SAVE_SLOTS) && (blocks < TINROM_MC_SAVE_SLOTS)) {
			blocks++;
			link = next[link];
		}

		slot->blocks = (uint8_t) blocks;

		uint32_t claimed = slot->size / TINROM_MC_BLOCK_SIZE;

		if (claimed && (claimed != blocks))
			TINROM_LOG("mc%u: slot %u chain %u, size claims %u",
				port, i + 1, blocks, (unsigned int) claimed);
	}

	/*
	 * Pass three: titles. Each file keeps one in the first frame of its first
	 * block, and deleting a file only rewrites the directory entry, so a
	 * deleted file still has a readable title.
	 */
	for (unsigned int i = 0; i < TINROM_MC_SAVE_SLOTS; i++) {
		TinromMcSlot *slot = &slots[i];

		if ((slot->state != TINROM_MC_SLOT_USED)
			&& (slot->state != TINROM_MC_SLOT_DELETED))
			continue;

		uint16_t frame = (uint16_t) ((i + 1) * MC_FRAMES_PER_BLOCK);

		if (tinromMcReadFrame(port, frame, frameBuffer) != TINROM_OK)
			continue;

		if ((frameBuffer[0] != TITLE_MAGIC_0) || (frameBuffer[1] != TITLE_MAGIC_1))
			continue;

		decodeTitle(slot->title, sizeof(slot->title), frameBuffer);
	}

	if (corrupt)
		TINROM_LOG("mc%u: %u directory frame(s) failed checksum", port, corrupt);

	return TINROM_OK;
}

/* Valid sectors are 0..3FFh, so this one is deliberately out of range. */
#define MC_ALIAS_PROBE_SECTOR 0x0400

TinromResult tinromMcCheckAliasing(unsigned int port, bool *aliases) {
	if (!aliases || (port >= MC_PORTS))
		return TINROM_ERR_BAD_ARG;

	*aliases = false;

	TinromResult result = beginCommand(port, MC_CMD_READ, NULL);

	if (result != TINROM_OK) {
		tinromSio0Deselect();

		return result;
	}

	uint8_t msb = (uint8_t) (MC_ALIAS_PROBE_SECTOR >> 8);
	uint8_t lsb = (uint8_t) (MC_ALIAS_PROBE_SECTOR & 0xff);

	(void) tinromSio0Exchange(msb);

	bool ok = tinromSio0WaitAck(TINROM_SIO0_ACK_CARD);

	(void) tinromSio0Exchange(lsb);
	ok = tinromSio0WaitAck(TINROM_SIO0_ACK_CARD) && ok;

	uint8_t ack1 = tinromSio0Exchange(0x00);

	ok = tinromSio0WaitAck(TINROM_SIO0_ACK_WRITE) && ok;

	uint8_t ack2 = tinromSio0Exchange(0x00);

	ok = tinromSio0WaitAck(TINROM_SIO0_ACK_CARD) && ok;

	/*
	 * The confirmed sector is the whole answer, and no /ACK is required after
	 * it: a card that is about to abort stops pulsing /ACK right here, which
	 * is itself part of the documented behaviour.
	 */
	uint8_t gotMsb = tinromSio0Exchange(0x00);

	(void) tinromSio0WaitAck(TINROM_SIO0_ACK_CARD);

	uint8_t gotLsb = tinromSio0Exchange(0x00);

	tinromSio0Deselect();

	if (!ok || (ack1 != MC_ACK1) || (ack2 != MC_ACK2))
		return TINROM_ERR_IO;

	uint16_t confirmed = (uint16_t) (((uint16_t) gotMsb << 8) | gotLsb);

	TINROM_LOG("mc%u: sector %04x answered %04x", port,
		(unsigned int) MC_ALIAS_PROBE_SECTOR, confirmed);

	if (confirmed == 0xffff)
		return TINROM_OK;

	if (confirmed == (MC_ALIAS_PROBE_SECTOR & 0x3ff)) {
		*aliases = true;

		return TINROM_OK;
	}

	return TINROM_ERR_UNSUPPORTED;
}

TinromResult tinromMcReadIcon(
	unsigned int port, unsigned int slot, TinromMcIcon *icon
) {
	if (!icon || (port >= MC_PORTS) || (slot >= TINROM_MC_SAVE_SLOTS))
		return TINROM_ERR_BAD_ARG;

	memset(icon, 0, sizeof(*icon));

	uint16_t     base   = (uint16_t) ((slot + 1) * MC_FRAMES_PER_BLOCK);
	TinromResult result = tinromMcReadFrame(port, base, frameBuffer);

	if (result != TINROM_OK)
		return result;

	if ((frameBuffer[0] != TITLE_MAGIC_0) || (frameBuffer[1] != TITLE_MAGIC_1))
		return TINROM_ERR_NOT_PRESENT;

	uint8_t flag = frameBuffer[TITLE_ICON_FLAG];

	if ((flag < TITLE_ICON_MIN) || (flag > TITLE_ICON_MAX))
		return TINROM_ERR_UNSUPPORTED;

	unsigned int frames = (unsigned int) (flag - TITLE_ICON_MIN) + 1;

	for (unsigned int i = 0; i < 16; i++)
		icon->palette[i] = read16(&frameBuffer[TITLE_CLUT + (i * 2)]);

	/*
	 * The frames follow the title frame, one per sector. They go into the
	 * sheet side by side, which means interleaving them a row at a time.
	 */
	for (unsigned int f = 0; f < frames; f++) {
		if (tinromMcReadFrame(port, (uint16_t) (base + 1 + f), frameBuffer)
			!= TINROM_OK) {
			if (!f)
				return TINROM_ERR_IO;

			/* Keep the frames we did get rather than losing the icon. */
			frames = f;
			break;
		}

		for (unsigned int row = 0; row < TINROM_MC_ICON_SIZE; row++)
			memcpy(
				&icon->pixels[(row * TINROM_MC_ICON_STRIDE)
					+ (f * ICON_ROW_BYTES)],
				&frameBuffer[row * ICON_ROW_BYTES],
				ICON_ROW_BYTES
			);
	}

	icon->frames = frames;

	return TINROM_OK;
}

TinromResult tinromMcReadImage(unsigned int port, void *buffer) {
	if (!buffer || (port >= MC_PORTS))
		return TINROM_ERR_BAD_ARG;

	uint8_t *out = (uint8_t *) buffer;

	for (uint16_t frame = 0; frame < MC_FRAMES; frame++) {
		TinromResult result = tinromMcReadFrame(
			port, frame, &out[(size_t) frame * TINROM_MC_FRAME_SIZE]
		);

		if (result != TINROM_OK)
			return result;
	}

	return TINROM_OK;
}

TinromResult tinromMcWriteFrame(
	unsigned int port, uint16_t frame, const void *data
) {
	if (!data || (port >= MC_PORTS) || (frame >= MC_FRAMES))
		return TINROM_ERR_BAD_ARG;

	TinromResult result =
		writeFrameSelected(port, frame, (const uint8_t *) data);

	tinromSio0Deselect();

	/* Whatever came of it, the card may be programming now. */
	writeSettling[port] = true;

	return result;
}

/* ------------------------------------------------------------------------ */
/*
 * Directory surgery: delete, undelete and copy.
 *
 * All three work the same way. A file is a linked list of blocks, and the
 * fifteen directory frames are its nodes: a state, a size, a pointer to the
 * next block and a filename. Deleting does not touch the data at all - it
 * only rewrites each node's state from the in-use values to the deleted ones,
 * which is why undelete is possible and why a deleted save still has a
 * readable title.
 *
 *   51h first block      A1h deleted, was first
 *   52h middle block     A2h deleted, was middle
 *   53h last block       A3h deleted, was last
 *   A0h free, formatted
 *
 * The one ordering rule worth stating: THE FIRST BLOCK'S ENTRY IS WRITTEN
 * LAST, in both directions. The first entry is the one the browser and the
 * BIOS look at, so a power loss part-way through leaves a file that is either
 * wholly there or wholly gone, never one whose head and tail disagree.
 */

static uint8_t sourceDir[TINROM_MC_SAVE_SLOTS][TINROM_MC_FRAME_SIZE];
static uint8_t targetDir[TINROM_MC_SAVE_SLOTS][TINROM_MC_FRAME_SIZE];

typedef uint8_t (*McDirectory)[TINROM_MC_FRAME_SIZE];

/** Recompute the frame's trailing XOR, which every writer has to do. */
static void signFrame(uint8_t *frame) {
	uint8_t sum = 0;

	for (unsigned int i = 0; i < FRAME_CHECKSUM; i++)
		sum ^= frame[i];

	frame[FRAME_CHECKSUM] = sum;
}

static void write16(uint8_t *p, uint16_t value) {
	p[0] = (uint8_t) (value & 0xff);
	p[1] = (uint8_t) (value >> 8);
}

static TinromResult loadDirectory(unsigned int port, McDirectory dir) {
	for (unsigned int i = 0; i < TINROM_MC_SAVE_SLOTS; i++) {
		TinromResult result =
			tinromMcReadFrame(port, (uint16_t) (1 + i), dir[i]);

		if (result != TINROM_OK)
			return result;

		if (!frameChecksumOk(dir[i])) {
			TINROM_LOG("mc%u: directory frame %u failed checksum", port, i + 1);

			return TINROM_ERR_CORRUPT;
		}
	}

	return TINROM_OK;
}

static TinromResult storeEntry(
	unsigned int port, McDirectory dir, unsigned int slot
) {
	signFrame(dir[slot]);

	return tinromMcWriteFrame(port, (uint16_t) (1 + slot), dir[slot]);
}

static bool stateIsDeleted(uint8_t state) {
	return (state >= DIR_STATE_DEL_1ST) && (state <= DIR_STATE_DEL_END);
}

static bool stateIsUsed(uint8_t state) {
	return (state >= DIR_STATE_FIRST) && (state <= DIR_STATE_LAST);
}

/**
 * Collect a file's block chain into `out`, starting at `first`.
 *
 * Returns the number of blocks, or 0 if the chain leaves the card, revisits a
 * block or runs longer than the card has blocks - all of which mean the
 * directory is damaged and nothing should be written to it.
 */
static unsigned int collectChain(
	McDirectory dir, unsigned int first, uint8_t *out
) {
	bool         seen[TINROM_MC_SAVE_SLOTS] = { false };
	unsigned int count = 0;
	unsigned int at    = first;

	for (;;) {
		if ((at >= TINROM_MC_SAVE_SLOTS) || seen[at])
			return 0;

		seen[at]   = true;
		out[count] = (uint8_t) at;

		if (++count > TINROM_MC_SAVE_SLOTS)
			return 0;

		uint16_t next = read16(&dir[at][DIR_NEXT]);

		if (next == DIR_NEXT_NONE)
			return count;

		at = next;
	}
}

/* Shared by delete and undelete: the two differ only by a constant. */
static TinromResult retagChain(
	unsigned int port, unsigned int slot, bool deleting
) {
	if (port >= MC_PORTS || (slot >= TINROM_MC_SAVE_SLOTS))
		return TINROM_ERR_BAD_ARG;

	TinromResult result = loadDirectory(port, sourceDir);

	if (result != TINROM_OK)
		return result;

	uint8_t head = sourceDir[slot][DIR_STATE];

	if (deleting ? (head != DIR_STATE_FIRST) : (head != DIR_STATE_DEL_1ST))
		return TINROM_ERR_BAD_ARG;

	uint8_t      chain[TINROM_MC_SAVE_SLOTS];
	unsigned int blocks = collectChain(sourceDir, slot, chain);

	if (!blocks)
		return TINROM_ERR_CORRUPT;

	/*
	 * Undelete is the one that can fail on inspection: the blocks a deleted
	 * file used to own are free, and something may have taken them since.
	 * Checking the whole chain before writing anything means a refusal
	 * changes nothing at all.
	 */
	for (unsigned int i = 0; i < blocks; i++) {
		uint8_t state = sourceDir[chain[i]][DIR_STATE];

		if (deleting ? !stateIsUsed(state) : !stateIsDeleted(state)) {
			TINROM_LOG("mc%u: block %u is %02x, chain not %s",
				port, chain[i] + 1, state,
				deleting ? "in use" : "recoverable");

			return TINROM_ERR_BUSY;
		}
	}

	/* First entry last, so an interrupted run never half-shows a file. */
	for (unsigned int i = blocks; i-- > 0;) {
		uint8_t *entry = sourceDir[chain[i]];

		entry[DIR_STATE] = (uint8_t) (deleting
			? (entry[DIR_STATE] + (DIR_STATE_DEL_1ST - DIR_STATE_FIRST))
			: (entry[DIR_STATE] - (DIR_STATE_DEL_1ST - DIR_STATE_FIRST)));

		result = storeEntry(port, sourceDir, chain[i]);

		if (result != TINROM_OK)
			return result;
	}

	return TINROM_OK;
}

TinromResult tinromMcDelete(unsigned int port, unsigned int slot) {
	return retagChain(port, slot, true);
}

TinromResult tinromMcUndelete(unsigned int port, unsigned int slot) {
	return retagChain(port, slot, false);
}

TinromResult tinromMcSaveChain(
	unsigned int port, unsigned int slot, uint8_t *blocks, unsigned int *count
) {
	if (!blocks || !count || (port >= MC_PORTS)
		|| (slot >= TINROM_MC_SAVE_SLOTS))
		return TINROM_ERR_BAD_ARG;

	TinromResult result = loadDirectory(port, sourceDir);

	if (result != TINROM_OK)
		return result;

	uint8_t head = sourceDir[slot][DIR_STATE];

	if ((head != DIR_STATE_FIRST) && (head != DIR_STATE_DEL_1ST))
		return TINROM_ERR_BAD_ARG;

	unsigned int blockCount = collectChain(sourceDir, slot, blocks);

	if (!blockCount)
		return TINROM_ERR_CORRUPT;

	*count = blockCount;

	return TINROM_OK;
}

TinromResult tinromMcCopy(
	unsigned int       sourcePort,
	unsigned int       sourceSlot,
	unsigned int       targetPort,
	unsigned int      *targetSlot,
	TinromMcProgressFn progress,
	void              *user
) {
	if ((sourcePort >= MC_PORTS) || (targetPort >= MC_PORTS)
		|| (sourceSlot >= TINROM_MC_SAVE_SLOTS))
		return TINROM_ERR_BAD_ARG;

	/* Copying within one card would need a second filename; see the docs. */
	if (sourcePort == targetPort)
		return TINROM_ERR_UNSUPPORTED;

	TinromResult result = loadDirectory(sourcePort, sourceDir);

	if (result != TINROM_OK)
		return result;

	uint8_t head = sourceDir[sourceSlot][DIR_STATE];

	if ((head != DIR_STATE_FIRST) && (head != DIR_STATE_DEL_1ST))
		return TINROM_ERR_BAD_ARG;

	uint8_t      chain[TINROM_MC_SAVE_SLOTS];
	unsigned int blocks = collectChain(sourceDir, sourceSlot, chain);

	if (!blocks)
		return TINROM_ERR_CORRUPT;

	result = loadDirectory(targetPort, targetDir);

	if (result != TINROM_OK)
		return result;

	/*
	 * Two files sharing a filename is what the BIOS treats as a corrupt
	 * card, so a name already in use is a refusal rather than a warning. A
	 * DELETED file of the same name is not in the way: restoring over one is
	 * the ordinary reason to be copying at all.
	 */
	for (unsigned int i = 0; i < TINROM_MC_SAVE_SLOTS; i++) {
		if (targetDir[i][DIR_STATE] != DIR_STATE_FIRST)
			continue;

		if (!memcmp(&targetDir[i][DIR_NAME],
			&sourceDir[sourceSlot][DIR_NAME], DIR_NAME_CHARS))
			return TINROM_ERR_BUSY;
	}

	/*
	 * Blocks do not have to be adjacent - the chain is a linked list - so any
	 * free ones will do. Formatted blocks are taken first and deleted ones
	 * only if those run out, which leaves recoverable saves recoverable for
	 * as long as the card can manage it.
	 */
	uint8_t      target[TINROM_MC_SAVE_SLOTS];
	unsigned int found = 0;

	for (unsigned int pass = 0; (pass < 2) && (found < blocks); pass++) {
		for (unsigned int i = 0;
			(i < TINROM_MC_SAVE_SLOTS) && (found < blocks); i++) {
			uint8_t state = targetDir[i][DIR_STATE];

			if (stateIsUsed(state))
				continue;

			bool deleted = stateIsDeleted(state);

			if (deleted == (pass == 0))
				continue;

			target[found++] = (uint8_t) i;
		}
	}

	if (found < blocks)
		return TINROM_ERR_NO_SPACE;

	/* Data first. A failure here leaves free blocks holding rubbish, which
	 * nothing is pointing at, rather than a directory entry with no data. */
	for (unsigned int b = 0; b < blocks; b++) {
		uint16_t from = (uint16_t) ((chain[b]  + 1) * MC_FRAMES_PER_BLOCK);
		uint16_t to   = (uint16_t) ((target[b] + 1) * MC_FRAMES_PER_BLOCK);

		for (unsigned int f = 0; f < MC_FRAMES_PER_BLOCK; f++) {
			result = tinromMcReadFrame(
				sourcePort, (uint16_t) (from + f), frameBuffer
			);

			if (result != TINROM_OK)
				return result;

			result = tinromMcWriteFrame(
				targetPort, (uint16_t) (to + f), frameBuffer
			);

			if (result != TINROM_OK)
				return result;

			if (progress)
				progress(user, (b * MC_FRAMES_PER_BLOCK) + f + 1,
					blocks * MC_FRAMES_PER_BLOCK);
		}
	}

	/*
	 * Then the directory entries, built fresh rather than copied: the states
	 * are always the in-use ones, so copying a deleted save restores it, and
	 * only the first entry carries the size and the name.
	 */
	for (unsigned int b = blocks; b-- > 0;) {
		uint8_t *entry = targetDir[target[b]];

		memset(entry, 0, TINROM_MC_FRAME_SIZE);

		entry[DIR_STATE] = (uint8_t) ((blocks == 1) ? DIR_STATE_FIRST
			: (b == 0) ? DIR_STATE_FIRST
			: (b == (blocks - 1)) ? DIR_STATE_LAST
			: DIR_STATE_MIDDLE);

		write16(&entry[DIR_NEXT], (b == (blocks - 1))
			? DIR_NEXT_NONE : target[b + 1]);

		if (!b) {
			memcpy(&entry[DIR_SIZE], &sourceDir[sourceSlot][DIR_SIZE], 4);
			memcpy(&entry[DIR_NAME], &sourceDir[sourceSlot][DIR_NAME],
				DIR_NAME_CHARS);
		}

		result = storeEntry(targetPort, targetDir, target[b]);

		if (result != TINROM_OK)
			return result;
	}

	if (targetSlot)
		*targetSlot = target[0];

	return TINROM_OK;
}

/* ------------------------------------------------------------------------ */
/* Files by name: what tinrom's own saves are read and written with. */

static bool nameMatches(const uint8_t *entry, const char *name) {
	for (unsigned int i = 0; i < DIR_NAME_CHARS; i++) {
		uint8_t want = (uint8_t) name[i];

		if (entry[DIR_NAME + i] != want)
			return false;
		if (!want)
			return true;
	}

	return true;
}

/*
 * collectChain() only proves the chain stays on the card. Before reading or
 * writing through it, check every block in it is one of this file's: first,
 * middles, last. A damaged pointer into another save's blocks must never be
 * followed by a write.
 */
static bool chainIsFile(
	McDirectory dir, const uint8_t *chain, unsigned int blocks
) {
	for (unsigned int i = 0; i < blocks; i++) {
		uint8_t want = (i == 0) ? DIR_STATE_FIRST
			: (i == (blocks - 1)) ? DIR_STATE_LAST : DIR_STATE_MIDDLE;

		if (dir[chain[i]][DIR_STATE] != want)
			return false;
	}

	return true;
}

static int findInUse(McDirectory dir, const char *name) {
	for (unsigned int i = 0; i < TINROM_MC_SAVE_SLOTS; i++) {
		if ((dir[i][DIR_STATE] == DIR_STATE_FIRST) && nameMatches(dir[i], name))
			return (int) i;
	}

	return -1;
}

TinromResult tinromMcFindFile(
	unsigned int port, const char *name, unsigned int *slot
) {
	if ((port >= MC_PORTS) || !name)
		return TINROM_ERR_BAD_ARG;

	TinromResult result = loadDirectory(port, sourceDir);

	if (result != TINROM_OK)
		return result;

	int found = findInUse(sourceDir, name);

	if (found < 0)
		return TINROM_ERR_NOT_PRESENT;

	if (slot)
		*slot = (unsigned int) found;

	return TINROM_OK;
}

TinromResult tinromMcReadFile(
	unsigned int port, const char *name, void *data, size_t size
) {
	if ((port >= MC_PORTS) || !name || !data)
		return TINROM_ERR_BAD_ARG;

	TinromResult result = loadDirectory(port, sourceDir);

	if (result != TINROM_OK)
		return result;

	int first = findInUse(sourceDir, name);

	if (first < 0)
		return TINROM_ERR_NOT_PRESENT;

	uint8_t      chain[TINROM_MC_SAVE_SLOTS];
	unsigned int blocks = collectChain(sourceDir, (unsigned int) first, chain);

	if (!blocks || !chainIsFile(sourceDir, chain, blocks))
		return TINROM_ERR_CORRUPT;
	if (size > (size_t) blocks * TINROM_MC_BLOCK_SIZE)
		size = (size_t) blocks * TINROM_MC_BLOCK_SIZE;

	uint8_t *out = (uint8_t *) data;

	for (size_t done = 0; done < size; done += TINROM_MC_FRAME_SIZE) {
		unsigned int block = (unsigned int) (done / TINROM_MC_BLOCK_SIZE);
		unsigned int frame = (unsigned int)
			((done % TINROM_MC_BLOCK_SIZE) / TINROM_MC_FRAME_SIZE);

		result = tinromMcReadFrame(port,
			(uint16_t) ((chain[block] + 1) * MC_FRAMES_PER_BLOCK + frame),
			frameBuffer);

		if (result != TINROM_OK)
			return result;

		size_t take = size - done;

		if (take > TINROM_MC_FRAME_SIZE)
			take = TINROM_MC_FRAME_SIZE;

		memcpy(out + done, frameBuffer, take);
	}

	return TINROM_OK;
}

/*
 * Writing a file over one of the same name: the new copy goes into free
 * blocks, the old one is then marked deleted, and only then do the new
 * directory entries appear - first block last, as everywhere here. A power
 * cut leaves either the old file, or the old one deleted (which the memory
 * card screen can undelete) and the new one not yet there; never two files
 * with one name, which the BIOS takes for a damaged card.
 *
 * With no room for a second copy, the old file's own blocks are written
 * over instead, which is the one case a power cut can cost the contents.
 */
TinromResult tinromMcWriteFile(
	unsigned int port, const char *name, const void *data, size_t size
) {
	if ((port >= MC_PORTS) || !name || !data || !size
		|| (size % TINROM_MC_BLOCK_SIZE))
		return TINROM_ERR_BAD_ARG;

	unsigned int blocks = (unsigned int) (size / TINROM_MC_BLOCK_SIZE);

	if (blocks > TINROM_MC_SAVE_SLOTS)
		return TINROM_ERR_NO_SPACE;

	TinromResult result = loadDirectory(port, targetDir);

	if (result != TINROM_OK)
		return result;

	int          old = findInUse(targetDir, name);
	uint8_t      oldChain[TINROM_MC_SAVE_SLOTS];
	unsigned int oldBlocks = 0;

	if (old >= 0) {
		oldBlocks = collectChain(targetDir, (unsigned int) old, oldChain);

		if (!oldBlocks || !chainIsFile(targetDir, oldChain, oldBlocks)) {
			TINROM_LOG("mc%u: %s has a damaged chain, not writing", port, name);
			return TINROM_ERR_CORRUPT;
		}
	}

	/*
	 * Free blocks, in three passes. First the blocks of earlier copies of
	 * this same file, which this write itself deleted last time: nobody
	 * wants those back, and taking them keeps a file saved over and over
	 * swapping between two blocks rather than walking across the card.
	 * Then formatted blocks, and other deleted saves' last, as
	 * tinromMcCopy() takes them, so those stay recoverable for as long as
	 * the card has room.
	 */
	bool ours[TINROM_MC_SAVE_SLOTS] = { false };

	for (unsigned int i = 0; i < TINROM_MC_SAVE_SLOTS; i++) {
		if ((targetDir[i][DIR_STATE] != DIR_STATE_DEL_1ST)
			|| !nameMatches(targetDir[i], name))
			continue;

		uint8_t      chain[TINROM_MC_SAVE_SLOTS];
		unsigned int length = collectChain(targetDir, i, chain);

		/* Only blocks still deleted: another file may own one since. */
		for (unsigned int b = 0; b < length; b++) {
			if (stateIsDeleted(targetDir[chain[b]][DIR_STATE]))
				ours[chain[b]] = true;
		}
	}

	uint8_t      target[TINROM_MC_SAVE_SLOTS];
	unsigned int found = 0;

	for (unsigned int pass = 0; (pass < 3) && (found < blocks); pass++) {
		for (unsigned int i = 0;
			(i < TINROM_MC_SAVE_SLOTS) && (found < blocks); i++) {
			uint8_t state = targetDir[i][DIR_STATE];
			bool    take;

			switch (pass) {
				case 0:  take = ours[i]; break;
				case 1:  take = !stateIsUsed(state) && !stateIsDeleted(state);
					break;
				default: take = stateIsDeleted(state) && !ours[i]; break;
			}

			if (take)
				target[found++] = (uint8_t) i;
		}
	}

	bool inPlace = false;

	if (found < blocks) {
		if (oldBlocks < blocks)
			return TINROM_ERR_NO_SPACE;

		memcpy(target, oldChain, blocks);
		inPlace = true;
	}

	const uint8_t *bytes = (const uint8_t *) data;

	for (unsigned int b = 0; b < blocks; b++) {
		uint16_t to = (uint16_t) ((target[b] + 1) * MC_FRAMES_PER_BLOCK);

		for (unsigned int f = 0; f < MC_FRAMES_PER_BLOCK; f++) {
			memcpy(frameBuffer, bytes
				+ ((size_t) b * TINROM_MC_BLOCK_SIZE)
				+ ((size_t) f * TINROM_MC_FRAME_SIZE), TINROM_MC_FRAME_SIZE);

			result = tinromMcWriteFrame(port, (uint16_t) (to + f), frameBuffer);

			if (result != TINROM_OK)
				return result;
		}
	}

	/* The old copy goes before the new one shows. */
	if ((old >= 0) && !inPlace) {
		for (unsigned int i = oldBlocks; i-- > 0;) {
			uint8_t *entry = targetDir[oldChain[i]];

			entry[DIR_STATE] = (uint8_t) (entry[DIR_STATE]
				+ (DIR_STATE_DEL_1ST - DIR_STATE_FIRST));

			result = storeEntry(port, targetDir, oldChain[i]);

			if (result != TINROM_OK)
				return result;
		}
	}

	for (unsigned int b = blocks; b-- > 0;) {
		uint8_t *entry = targetDir[target[b]];

		memset(entry, 0, TINROM_MC_FRAME_SIZE);

		entry[DIR_STATE] = (uint8_t) ((b == 0) ? DIR_STATE_FIRST
			: (b == (blocks - 1)) ? DIR_STATE_LAST : DIR_STATE_MIDDLE);
		write16(&entry[DIR_NEXT], (b == (blocks - 1))
			? DIR_NEXT_NONE : target[b + 1]);

		if (!b) {
			uint32_t bytesUsed = (uint32_t) size;

			entry[DIR_SIZE + 0] = (uint8_t) (bytesUsed);
			entry[DIR_SIZE + 1] = (uint8_t) (bytesUsed >> 8);
			entry[DIR_SIZE + 2] = (uint8_t) (bytesUsed >> 16);
			entry[DIR_SIZE + 3] = (uint8_t) (bytesUsed >> 24);

			for (unsigned int i = 0; (i < DIR_NAME_CHARS) && name[i]; i++)
				entry[DIR_NAME + i] = (uint8_t) name[i];
		}

		result = storeEntry(port, targetDir, target[b]);

		if (result != TINROM_OK)
			return result;
	}

	/*
	 * An in-place write that got shorter frees the old tail - after the new
	 * entries have cut the chain short, so that nothing in use ever points
	 * at a block marked free. A cut between the two wastes those blocks
	 * rather than cross-linking them with whatever takes them next.
	 */
	if (inPlace) {
		for (unsigned int i = blocks; i < oldBlocks; i++) {
			uint8_t *entry = targetDir[oldChain[i]];

			memset(entry, 0, TINROM_MC_FRAME_SIZE);
			entry[DIR_STATE] = DIR_STATE_FORMAT;
			write16(&entry[DIR_NEXT], DIR_NEXT_NONE);

			result = storeEntry(port, targetDir, oldChain[i]);

			if (result != TINROM_OK)
				return result;
		}
	}

	TINROM_LOG("mc%u: wrote %s, %u block%s%s", port, name, blocks,
		(blocks == 1) ? "" : "s", inPlace ? ", in place" : "");

	return TINROM_OK;
}

/* ------------------------------------------------------------------------ */
/* Still stubs. */

TinromResult tinromMcWriteImage(unsigned int port, const void *buffer) {
	(void) port;
	(void) buffer;

	return TINROM_ERR_UNIMPLEMENTED;
}

/*
 * Block 0 of an empty card, as psx-spx lays it out ("Memory Card Data
 * Format"): the header frame, fifteen directory frames, twenty frames of the
 * broken sector list, and a copy of the header in frame 63 - the frame
 * written to test the card. The saves' own blocks, 1 to 15, are not touched,
 * as the BIOS does not touch them: with every directory frame freshly
 * formatted, nothing refers to them any more.
 */
#define MC_FRAME_HEADER       0
#define MC_FRAME_BROKEN_FIRST 16
#define MC_FRAME_BROKEN_COUNT 20
#define MC_FRAME_WRITE_TEST   63

TinromResult tinromMcFormat(unsigned int port) {
	if (port >= MC_PORTS)
		return TINROM_ERR_BAD_ARG;

	uint8_t      frame[TINROM_MC_FRAME_SIZE];
	TinromResult result;

	/* The directory first: every slot free, as formatted, linked to nothing. */
	for (unsigned int slot = 0; slot < TINROM_MC_SAVE_SLOTS; slot++) {
		memset(frame, 0, sizeof(frame));
		frame[DIR_STATE] = DIR_STATE_FORMAT;
		write16(&frame[DIR_NEXT], DIR_NEXT_NONE);
		signFrame(frame);

		result = tinromMcWriteFrame(port, (uint16_t) (1 + slot), frame);

		if (result != TINROM_OK)
			return result;
	}

	/* No broken sectors: FFFFFFFFh where an entry's sector would be. */
	for (unsigned int i = 0; i < MC_FRAME_BROKEN_COUNT; i++) {
		memset(frame, 0, sizeof(frame));
		memset(frame, 0xff, 4);
		write16(&frame[DIR_NEXT], DIR_NEXT_NONE);
		signFrame(frame);

		result = tinromMcWriteFrame(
			port, (uint16_t) (MC_FRAME_BROKEN_FIRST + i), frame
		);

		if (result != TINROM_OK)
			return result;
	}

	/* The header last, after its copy: a card that was never formatted and
	 * is cut off half way through does not yet claim to be one. */
	memset(frame, 0, sizeof(frame));
	frame[0] = 'M';
	frame[1] = 'C';
	signFrame(frame);

	result = tinromMcWriteFrame(port, MC_FRAME_WRITE_TEST, frame);

	if (result == TINROM_OK)
		result = tinromMcWriteFrame(port, MC_FRAME_HEADER, frame);

	if (result == TINROM_OK)
		TINROM_LOG("mc%u: formatted", port);

	return result;
}

TinromResult tinromMcVerifyCapacity(unsigned int port, TinromMcInfo *info) {
	(void) port;
	(void) info;

	return TINROM_ERR_UNIMPLEMENTED;
}
