/* tinrom - (C) 2026 TinWhisker - MIT licensed
 *
 * Memory cards in the hex editor. A card is shown one 128-byte frame at a
 * time - exactly one screen - from a copy read when the frame comes into
 * view. Edits change the copy and mark the bytes; the card itself is written
 * only when asked, one frame, with a read-back to check it.
 */

#include <stdio.h>
#include "libc/string.h"
#include "modules/hexedit/hexedit.h"

/*
 * Block 0 frames 0-35 (the header, the directory and the broken sector list)
 * end in a checksum: every other byte of the frame XORed together. A frame
 * written with a stale one is a corrupt directory, so it is recomputed.
 */
#define CHECKSUMMED_FRAMES 36

bool hexCardLoad(HexEditor *ed, uint32_t frameNumber) {
	const HexRegion *region = ed->region;

	ed->frameNumber = (int32_t) frameNumber;
	memset(ed->frameDirty, 0, sizeof(ed->frameDirty));
	ed->undoCount = 0;

	TinromResult result = tinromMcReadFrame(
		region->port, (uint16_t) frameNumber, ed->frame
	);

	ed->frameLoaded = (result == TINROM_OK);

	if (!ed->frameLoaded)
		snprintf(ed->message, sizeof(ed->message), "frame %u: %s",
			(unsigned int) frameNumber, tinromResultString(result));

	return ed->frameLoaded;
}

bool hexCardDirty(const HexEditor *ed) {
	for (unsigned int i = 0; i < sizeof(ed->frameDirty); i++) {
		if (ed->frameDirty[i])
			return true;
	}

	return false;
}

bool hexCardByteDirty(const HexEditor *ed, uint32_t index) {
	return (ed->frameDirty[index / 8] >> (index % 8)) & 1;
}

void hexCardMark(HexEditor *ed, uint32_t index) {
	ed->frameDirty[index / 8] |= (uint8_t) (1 << (index % 8));
}

bool hexCardWriteBack(TinromContext *ctx, HexEditor *ed) {
	if (!ed->frameLoaded || !hexCardDirty(ed))
		return true;

	uint32_t number    = (uint32_t) ed->frameNumber;
	bool     checksum  = number < CHECKSUMMED_FRAMES;
	char     body[256];

	snprintf(body, sizeof(body),
		"Write frame %03Xh (block %u) back to memory card %u?\n\n"
		"%s",
		(unsigned int) number, (unsigned int) (number / 64),
		ed->region->port + 1,
		checksum
			? "This frame is in the directory block. Its last byte "
			  "is a checksum, and tinrom will recompute it so the "
			  "card still reads as valid."
			: "Only this frame is written.");

	if (!uiConfirm(ctx, "Write frame", body, true))
		return false;

	if (checksum) {
		uint8_t sum = 0;

		for (unsigned int i = 0; i < (HEX_PAGE_BYTES - 1); i++)
			sum ^= ed->frame[i];

		if (ed->frame[HEX_PAGE_BYTES - 1] != sum) {
			ed->frame[HEX_PAGE_BYTES - 1] = sum;
			hexCardMark(ed, HEX_PAGE_BYTES - 1);
		}
	}

	TinromResult result = tinromMcWriteFrame(
		ed->region->port, (uint16_t) number, ed->frame
	);

	if (result != TINROM_OK) {
		snprintf(ed->message, sizeof(ed->message), "write failed: %s",
			tinromResultString(result));

		return false;
	}

	/* The card answering 'G' is not the same as the data being there. */
	static uint8_t check[HEX_PAGE_BYTES];

	result = tinromMcReadFrame(ed->region->port, (uint16_t) number, check);

	if ((result == TINROM_OK) && !memcmp(check, ed->frame, HEX_PAGE_BYTES))
		snprintf(ed->message, sizeof(ed->message),
			"frame %03Xh written and verified", (unsigned int) number);
	else
		snprintf(ed->message, sizeof(ed->message),
			"frame %03Xh written, but reads back different",
			(unsigned int) number);

	TINROM_LOG("hexedit: card %u frame %03x written",
		ed->region->port + 1, (unsigned int) number);

	memset(ed->frameDirty, 0, sizeof(ed->frameDirty));
	ed->undoCount = 0;

	return true;
}

bool hexCardLeave(TinromContext *ctx, HexEditor *ed) {
	if (!hexCardDirty(ed))
		return true;

	static const UIMenuItem items[] = {
		{ "Write it to the card", "", true, 0 },
		{ "Discard the changes",  "", true, 1 },
		{ "Stay on this frame",   "", true, 2 }
	};

	size_t choice = uiPopupRun(ctx, "Unsaved frame", items,
		sizeof(items) / sizeof(items[0]), 60, 80, hexViewBackground, ed);

	switch (choice) {
		case 0:
			return hexCardWriteBack(ctx, ed);

		case 1:
			memset(ed->frameDirty, 0, sizeof(ed->frameDirty));
			ed->undoCount = 0;

			return true;

		default:
			return false;
	}
}
