/* tinrom - (C) 2026 TinWhisker - MIT licensed */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "core/result.h"

/*
 * Memory cards over SIO0 (address 81h). Frame size, directory layout and the
 * XOR checksum are documented hardware behaviour; see psx-spx in NOTICE.md.
 *
 * Reading is real: probe, raw frames, the directory and a full card image.
 * Writing is real for frames and for the three directory operations - delete,
 * undelete and copy; imaging, formatting and the capacity sweep are still
 * stubs. The bus underneath is shared with the controller driver; see
 * hal/sio0.h.
 */

#define TINROM_MC_FRAME_SIZE  128
#define TINROM_MC_BLOCK_SIZE  8192
#define TINROM_MC_BLOCKS      16
#define TINROM_MC_CARD_SIZE   (TINROM_MC_BLOCK_SIZE * TINROM_MC_BLOCKS)
#define TINROM_MC_SAVE_SLOTS  15

/*
 * Save icons. 16x16 at 4bpp with a 16-entry palette, and 1..3 animation
 * frames declared by the title frame's display flag.
 */
#define TINROM_MC_ICON_SIZE       16
#define TINROM_MC_ICON_MAX_FRAMES 3
/* Frames laid out left to right, ready to upload as one 48x16 texture. */
#define TINROM_MC_ICON_STRIDE \
	((TINROM_MC_ICON_SIZE * TINROM_MC_ICON_MAX_FRAMES) / 2)
#define TINROM_MC_ICON_BYTES (TINROM_MC_ICON_STRIDE * TINROM_MC_ICON_SIZE)

typedef enum {
	TINROM_MC_SLOT_FREE    = 0,
	TINROM_MC_SLOT_USED    = 1,
	TINROM_MC_SLOT_LINK    = 2,
	TINROM_MC_SLOT_DELETED = 3
} TinromMcSlotState;

typedef struct {
	TinromMcSlotState state;
	/*
	 * True for every block of a deleted file, including its continuation
	 * blocks - `state` alone cannot say so, because a continuation block
	 * reports TINROM_MC_SLOT_LINK either way.
	 */
	bool              deleted;
	uint32_t          size;
	char              filename[21];
	char              title[33];   /* Shift-JIS in the raw header */
	uint8_t           blocks;
} TinromMcSlot;

/*
 * Both arrays below are handed straight to the GPU's DMA, which requires a
 * 4-byte aligned source and asserts - fatally, the assert spins forever - if
 * it does not get one. `frames` is a uint32_t purely to force that alignment
 * on everything after it; as a uint8_t it put `palette` at offset 2 and
 * `pixels` at 34, and the struct's own alignment at 2. The static asserts
 * below are there so a future reordering fails the build rather than the
 * console.
 */
typedef struct {
	/** 0 means the file has no usable icon. */
	uint32_t frames;
	/*
	 * Straight off the card, so 0000h is transparent and 8000h is solid
	 * black - the same convention the GPU uses for a texture drawn without
	 * semi-transparency.
	 */
	uint16_t palette[16];
	uint8_t  pixels[TINROM_MC_ICON_BYTES];
} TinromMcIcon;

_Static_assert(
	!(offsetof(TinromMcIcon, palette) % 4), "icon palette must be 4-aligned"
);
_Static_assert(
	!(offsetof(TinromMcIcon, pixels) % 4), "icon pixels must be 4-aligned"
);
_Static_assert(_Alignof(TinromMcIcon) >= 4, "icon struct must be 4-aligned");

typedef struct {
	bool     present;
	/* Some counterfeit cards report 16 blocks but alias after 1. */
	bool     suspectCapacity;
	uint32_t detectedSize;
	/*
	 * The card's status byte, as returned alongside every command.
	 * bit 3 = untouched since power-up, bit 2 = last write failed,
	 * bit 5 = set by some third-party cards.
	 */
	uint8_t  flag;
} TinromMcInfo;

#ifdef __cplusplus
extern "C" {
#endif

TinromResult tinromMcInit(void);

/**
 * Address the card and fill in `info`. TINROM_ERR_NOT_PRESENT means an empty
 * slot; TINROM_ERR_UNSUPPORTED means something answered 81h that is not a
 * card.
 */
TinromResult tinromMcProbe(unsigned int port, TinromMcInfo *info);

/** Raw 128-byte frame access - everything else is built on these two. */
TinromResult tinromMcReadFrame(unsigned int port, uint16_t frame, void *data);
/**
 * Writes one frame and returns what the card made of it. The frame is not
 * read back: TINROM_OK means the card answered 'G'. TINROM_ERR_VERIFY is the
 * card rejecting our checksum, so nothing was written; TINROM_ERR_BAD_ARG a
 * sector it will not accept.
 */
TinromResult tinromMcWriteFrame(
	unsigned int port, uint16_t frame, const void *data
);

/** Reports frames written, for a progress bar. Called often; keep it cheap. */
typedef void (*TinromMcProgressFn)(
	void *user, unsigned int done, unsigned int total
);

/**
 * Marks a save's whole block chain deleted. The data is untouched, so
 * tinromMcUndelete() can put it back until something else claims the blocks.
 * TINROM_ERR_BAD_ARG if `slot` is not the first block of a file in use.
 */
TinromResult tinromMcDelete(unsigned int port, unsigned int slot);
/**
 * Puts a deleted chain back in use. TINROM_ERR_BUSY means one of its blocks
 * has since been taken by another save, and nothing was written.
 * TINROM_ERR_BAD_ARG if `slot` is not the first block of a deleted file.
 */
TinromResult tinromMcUndelete(unsigned int port, unsigned int slot);
/**
 * Fills `blocks` with the directory indices a save occupies, in chain order,
 * and `count` with how many. `blocks` needs TINROM_MC_SAVE_SLOTS entries.
 * Deleted saves are walkable too, which is what makes reading one out
 * possible. TINROM_ERR_BAD_ARG if `slot` is not a file's first block.
 */
TinromResult tinromMcSaveChain(
	unsigned int port, unsigned int slot, uint8_t *blocks, unsigned int *count
);

/**
 * Copies the save at `sourceSlot` onto the other card, into any free blocks -
 * they need not be adjacent - and always in use, so copying a deleted save
 * restores it. `targetSlot` receives the first block it landed on; pass NULL
 * if you do not care.
 *
 * TINROM_ERR_UNSUPPORTED to copy within one card, TINROM_ERR_BUSY if a save
 * of the same filename is already there, TINROM_ERR_NO_SPACE if the card has
 * fewer free blocks than the save needs.
 */
TinromResult tinromMcCopy(
	unsigned int       sourcePort,
	unsigned int       sourceSlot,
	unsigned int       targetPort,
	unsigned int      *targetSlot,
	TinromMcProgressFn progress,
	void              *user
);

/*
 * Files by name, for tinrom's own saves. A name is the directory's 20-byte
 * filename, such as "BETNRM-00001SETTINGS". TINROM_ERR_NOT_PRESENT means no
 * file of that name is in use on the card (a deleted one does not count).
 */
TinromResult tinromMcFindFile(
	unsigned int port, const char *name, unsigned int *slot
);
/** Reads the first `size` bytes of the file, title frame and all. */
TinromResult tinromMcReadFile(
	unsigned int port, const char *name, void *data, size_t size
);
/**
 * Writes a whole file of `size` bytes - a whole number of blocks, title
 * frame and all - replacing any of the same name. TINROM_ERR_NO_SPACE if
 * the card has neither free blocks for it nor an old copy big enough to
 * write over. See mcard.c for the order things are written in.
 */
TinromResult tinromMcWriteFile(
	unsigned int port, const char *name, const void *data, size_t size
);

/**
 * Fills TINROM_MC_SAVE_SLOTS entries from the card's directory, with titles
 * folded from Shift-JIS into ASCII. Slow - it reads up to thirty frames - so
 * call it when a screen opens, not per frame.
 */
TinromResult tinromMcReadDirectory(unsigned int port, TinromMcSlot *slots);
/**
 * Non-destructive counterfeit check. Asks for a sector past the end of the
 * card: a genuine card refuses with FFFFh, a clone answers with the sector
 * ANDed with 3FFh and hands over the wrong data. Only catches clones that
 * alias at the top of the address space - tinromMcVerifyCapacity() is the one
 * that proves it, and that one writes. TINROM_ERR_UNSUPPORTED means the card
 * answered in neither way, so the test told us nothing.
 */
TinromResult tinromMcCheckAliasing(unsigned int port, bool *aliases);

/**
 * Reads the save icon for directory slot `slot` (0..14) - its palette and
 * every animation frame it declares - into a sheet ready for the GPU. Costs
 * up to four frame reads. TINROM_ERR_NOT_PRESENT means the block has no title
 * frame; TINROM_ERR_UNSUPPORTED means its display flag is not 11h..13h, which
 * is what the BIOS treats as a corrupt file.
 */
TinromResult tinromMcReadIcon(
	unsigned int port, unsigned int slot, TinromMcIcon *icon
);

/** Full-card image in, for the .mcr tooling. Needs TINROM_MC_CARD_SIZE. */
TinromResult tinromMcReadImage(unsigned int port, void *buffer);
/** STUB. */
TinromResult tinromMcWriteImage(unsigned int port, const void *buffer);
/** STUB. Write a fresh, empty directory. */
TinromResult tinromMcFormat(unsigned int port);
/**
 * STUB. Write a pattern across the whole card and read it back to catch cards
 * that alias, which is how the fake-capacity check is meant to work.
 */
TinromResult tinromMcVerifyCapacity(unsigned int port, TinromMcInfo *info);

#ifdef __cplusplus
}
#endif
