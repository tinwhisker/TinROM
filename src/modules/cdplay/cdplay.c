/* tinrom - (C) 2026 TinWhisker - MIT licensed
 *
 * Plays an audio disc: an audio CD, or the music tracks on a PlayStation
 * disc.
 *
 * One grid cell per track, in reading order, GRID_X across. Cross on a
 * track plays it; Cross on the track that is playing pauses or resumes it.
 * The "..." button under the grid holds the rest: pause, stop, next and
 * previous, repeat, the room it sounds in (hal/spu.c's reverb), and mono.
 * Playback carries on from one track to the next, and stops when the
 * module is left.
 *
 * Behind the grid, in place of the spinning chip, a visualiser: a ring of
 * spectrum bars, a level meter or the waveform, stepped through with L1 and
 * R1. visual.c measures what the SPU captured of the CD audio; scene.c
 * draws it with the GTE.
 *
 * The drive does the playing (hal/cdrom.c, docs/cd-audio.md); this file
 * only asks it what it is doing, four times a second, from the grid's poll
 * hook - which is also where a disc being swapped is noticed.
 */

#include "core/iso9660.h"
#include "hal/spu.h"
#include "libc/string.h"
#include "modules/cdplay/scene.h"
#include "modules/cdplay/visual.h"
#include "modules/modules.h"
#include "ui/grid.h"

#define MAX_TRACKS TINROM_CD_TRACKS_MAX /* 99, the Red Book's limit */
#define GRID_X     11
#define GRID_Y     9

_Static_assert(GRID_X * GRID_Y >= MAX_TRACKS, "the grid must hold every track");
_Static_assert(GRID_X <= UI_GRID_MAX_COLUMNS, "too wide for the grid");
_Static_assert(GRID_Y <= UI_GRID_MAX_ROWS, "too tall for the grid");

/* How often the drive is asked what it is doing; GetlocP is cheap. */
#define POLL_FRAMES  15
/* How often to try a disc that would not read: GetID can take its time. */
#define RETRY_FRAMES 60
/* "Previous" restarts the track instead once this far into it. */
#define RESTART_SECONDS 3

typedef enum {
	PLAYER_STOPPED = 0,
	PLAYER_PLAYING,
	PLAYER_PAUSED
} PlayerState;

typedef enum {
	REPEAT_OFF = 0,
	REPEAT_TRACK,
	REPEAT_DISC
} RepeatMode;

/* The disc. */
static TinromCdToc toc;
static bool        haveDisc;
static bool        firstIsData; /* a PlayStation disc: track 1 is the game */

/* The player. Repeat, effect and mono outlive a visit; nothing else does. */
static PlayerState      state;
static unsigned int     playing;  /* track number, 0 for none */
static TinromCdPosition position; /* as of the last poll */
static RepeatMode       repeat;
static TinromSpuReverb  effect;
static bool             mono;

/* The screen. */
static UIGridCell   cells[GRID_X * GRID_Y];
static char         labels[MAX_TRACKS][4];
static char         paneTitle[48];
static UIGridPane   pane;
static UIGridCursor cursor;

static uint32_t lastPoll, lastTry;

/* Compared by address, to tell which "no disc" the pane is already saying. */
static const char REASON_LID[]  = "The lid is open.";
static const char REASON_DEAD[] = "The drive is not answering. Hello?";

/* --- the disc ----------------------------------------------------------- */

static bool isAudio(unsigned int track) {
	if (!haveDisc || (track < toc.first) || (track > toc.last))
		return false;

	return !(firstIsData && (track == toc.first));
}

static unsigned int firstAudio(void) {
	for (unsigned int t = toc.first; haveDisc && (t <= toc.last); t++)
		if (isAudio(t))
			return t;

	return 0;
}

static unsigned int lastAudio(void) {
	for (unsigned int t = toc.last; haveDisc && (t >= toc.first) && t; t--)
		if (isAudio(t))
			return t;

	return 0;
}

static unsigned int audioTracks(void) {
	unsigned int first = firstAudio();

	return first ? (lastAudio() - first + 1) : 0;
}

/* Track n is cell n - 1, so the numbers read left to right, top to bottom. */
static unsigned int trackAt(unsigned int cell) {
	return cell + 1;
}

static void formatTime(char *out, size_t size, unsigned int seconds) {
	snprintf(out, size, "%u:%02u", seconds / 60, seconds % 60);
}

/* --- the screen --------------------------------------------------------- */

/*
 * Rewrites the title and the cells from the state above. Called after
 * anything changes; the grid reads both every frame.
 */
static void refresh(void) {
	char length[12];

	for (unsigned int cell = 0; cell < (GRID_X * GRID_Y); cell++) {
		unsigned int track = trackAt(cell);
		UIGridCell  *c     = &cells[cell];

		*c = (UIGridCell) {
			.kind  = UI_GRID_EMPTY,
			.label = NULL,
			.icon  = UI_GRID_NO_ICON,
			.faded = false
		};

		if (!haveDisc || (track < toc.first) || (track > toc.last))
			continue;

		/*
		 * The data track keeps its number, so the audio tracks keep theirs,
		 * but it is drawn empty: there is nothing there to play.
		 */
		snprintf(labels[cell], sizeof(labels[cell]), "%s%u",
			(track == playing) ? ">" : "", track);

		c->label = labels[cell];
		c->kind  = isAudio(track) ? UI_GRID_ITEM : UI_GRID_EMPTY;
	}

	if (!haveDisc)
		return;

	switch (state) {
		case PLAYER_PLAYING:
		case PLAYER_PAUSED:
			formatTime(length, sizeof(length),
				tinromCdTrackLength(&toc, playing));
			snprintf(paneTitle, sizeof(paneTitle), "%s %u   %u:%02u / %s",
				(state == PLAYER_PLAYING) ? "Playing" : "Paused", playing,
				(unsigned int) position.minute,
				(unsigned int) position.second, length);
			break;

		default: {
			unsigned int count = audioTracks();
			unsigned int first = firstAudio();

			formatTime(length, sizeof(length),
				first ? (toc.leadOut - toc.start[first]) : 0);
			snprintf(paneTitle, sizeof(paneTitle), "%u track%s   %s",
				count, (count == 1) ? "" : "s", length);
			break;
		}
	}
}

/*
 * Sizes the pane to the disc: GRID_X across and only as many rows as its
 * tracks need, so a ten-track album gets big cells rather than a sea of
 * empty ones. Without a disc it keeps the full size, which is the box the
 * "no disc" message is drawn in.
 */
static void shapePane(void) {
	unsigned int rows = GRID_Y;

	if (haveDisc && toc.last)
		rows = (toc.last + GRID_X - 1) / GRID_X;

	pane.columns = GRID_X;
	pane.rows    = (uint8_t) rows;

	/* The grid only re-checks the cursor when a pane goes away entirely. */
	if ((cursor.cell != UI_GRID_CELL_MENU)
		&& (cursor.cell >= uiGridPaneCells(&pane)))
		cursor.cell = 0;
}

static void noDisc(const char *reason) {
	haveDisc       = false;
	state          = PLAYER_STOPPED;
	playing        = 0;
	pane.available = false;
	pane.title     = "Audio CD";

	pane.unavailableReason = reason;

	shapePane();
	refresh();
}

/*
 * Whether track 1 is data - a PlayStation disc's game - rather than music.
 * The TOC does not say, and on a PlayStation disc only track 1 can be.
 *
 * GetID does, on a console: an audio CD answers with its audio flag set.
 * Not on every emulator, though. pcsx-redux answers every GetID alike, as
 * a licensed PlayStation disc, so an audio CD would lose its first track.
 * So a "data" answer is checked where it matters: a data track 1 holds an
 * ISO9660 volume descriptor at sector 16, and music does not. Reading it
 * costs a seek, once per disc. A disc whose data cannot be read - a CD-R on
 * a console that has not been unlocked - is taken as music, and its track
 * 1 then plays as silence rather than going missing.
 * We could not do this, but I like to iterate in redux and a cd player is
 * mostly for novelty purposes.
 */
static bool trackOneIsData(void) {
	static uint8_t sector[ISO_SECTOR_SIZE] __attribute__((aligned(4)));
	TinromDiscInfo info;

	if ((tinromCdIdentify(&info) != TINROM_OK)
		|| (info.type != TINROM_DISC_DATA))
		return false;

	if (tinromCdReadSectors(ISO_PVD_LBA, sector, 1) != TINROM_OK)
		return false;

	/* Type 1, primary volume descriptor, then the standard identifier. */
	return (sector[0] == 1) && !memcmp(&sector[1], "CD001", 5);
}

/* Reads the TOC of whatever is in the drive. True if there is a disc. */
static bool loadDisc(void) {
	TinromResult result = tinromCdReadToc(&toc);

	switch (result) {
		case TINROM_OK:
			break;

		case TINROM_ERR_NOT_PRESENT:
			noDisc("No disc.");
			return false;

		case TINROM_ERR_CORRUPT:
			noDisc("This disc's track list does not make sense.");
			return false;

		default:
			noDisc(REASON_DEAD);
			return false;
	}

	haveDisc    = true;
	firstIsData = trackOneIsData();

	TINROM_LOG("cdplay: tracks %u-%u, %s", (unsigned int) toc.first,
		(unsigned int) toc.last, firstIsData ? "data first" : "all audio");

	if (!audioTracks()) {
		noDisc("This disc has no audio tracks.");
		return false;
	}

	state          = PLAYER_STOPPED;
	playing        = 0;
	pane.available = true;
	pane.title     = paneTitle;

	pane.unavailableReason = NULL;

	shapePane();
	refresh();

	/* Land on something that plays. */
	if (cursor.cell != UI_GRID_CELL_MENU && !isAudio(trackAt(cursor.cell)))
		cursor.cell = firstAudio() - 1;

	return true;
}

/* --- the player --------------------------------------------------------- */

static TinromResult play(unsigned int track) {
	TinromResult result = tinromCdPlayTrack(&toc, track);

	if (result == TINROM_OK) {
		state   = PLAYER_PLAYING;
		playing = track;

		position = (TinromCdPosition) { .track = (uint8_t) track };
	}

	refresh();

	return result;
}

static TinromResult stop(void) {
	state   = PLAYER_STOPPED;
	playing = 0;

	refresh();

	return tinromCdStop();
}

static TinromResult togglePause(void) {
	TinromResult result;

	if (state == PLAYER_PLAYING) {
		result = tinromCdPause();

		if (result == TINROM_OK)
			state = PLAYER_PAUSED;
	} else {
		result = tinromCdResume();

		if (result == TINROM_OK)
			state = PLAYER_PLAYING;
	}

	refresh();

	return result;
}

/* The track after this one, by the repeat setting; 0 for none. */
static unsigned int nextTrack(unsigned int track) {
	if (repeat == REPEAT_TRACK)
		return track;

	for (unsigned int t = track + 1; t <= toc.last; t++)
		if (isAudio(t))
			return t;

	return (repeat == REPEAT_DISC) ? firstAudio() : 0;
}

static unsigned int previousTrack(unsigned int track) {
	for (unsigned int t = track - 1; t >= toc.first && t; t--)
		if (isAudio(t))
			return t;

	return (repeat == REPEAT_DISC) ? lastAudio() : track;
}

/* What happens when a track runs out, or "Next" is picked. */
static TinromResult advance(void) {
	unsigned int next = nextTrack(playing);

	return next ? play(next) : stop();
}

/* --- polling ------------------------------------------------------------ */

/*
 * Runs once a frame from the grid. `light` is for the popup's background,
 * which polls from inside a frame and so must not go reading a new disc.
 */
static bool pollDrive(bool light) {
	uint32_t now = tinromFrameCounter();

	if ((now - lastPoll) < POLL_FRAMES)
		return false;

	lastPoll = now;

	TinromCdStatus status;

	if (tinromCdGetStatus(&status) != TINROM_OK) {
		if (!haveDisc && (pane.unavailableReason == REASON_DEAD))
			return false;

		noDisc(REASON_DEAD);

		return true;
	}

	/*
	 * The lid bit is latched: it stays set until a status is read with the
	 * lid shut, so even a quick swap between two polls shows up here.
	 */
	if (status.lidOpen) {
		if (!haveDisc && (pane.unavailableReason == REASON_LID))
			return false;

		noDisc(REASON_LID);

		return true;
	}

	if (!haveDisc) {
		if (light || ((now - lastTry) < RETRY_FRAMES))
			return false;

		lastTry = now;

		return loadDisc();
	}

	/* AutoPause stopped the drive at the end of a track. */
	if (tinromCdAudioEnded() && (state == PLAYER_PLAYING)) {
		advance();

		return true;
	}

	if (state == PLAYER_PLAYING) {
		TinromCdPosition where;

		if (tinromCdGetPosition(&where) == TINROM_OK) {
			/* Past the last track, AutoPause or not: nothing left to play. */
			if (where.leadOut) {
				stop();

				return true;
			}

			/* Straight after a Play it can still be saying where it was. */
			if (where.track == playing)
				position = where;
		}

		refresh();
	}

	return false;
}

/* --- the visualiser ------------------------------------------------------ */

#define HINT "D-pad: move   Cross: play   L1/R1: look   Circle: back"

/* How long the name of a newly picked mode stays up: 1.5 s at 60 Hz. */
#define BANNER_FRAMES 90

static Visual     visual;
static VisualMode visualMode; /* outlives a visit */
static uint32_t   bannerUntil;

/* DMA'd into, so word-aligned. */
static int16_t captureLeft[TINROM_SPU_CAPTURE_SAMPLES]
	__attribute__((aligned(4)));
static int16_t captureRight[TINROM_SPU_CAPTURE_SAMPLES]
	__attribute__((aligned(4)));

/*
 * The UI's background hook, so it runs every frame - under the "..." menu
 * and message boxes too. Only reads the capture while something plays;
 * otherwise everything sinks back to rest.
 */
static void drawVisualiser(UIContext *ui, unsigned int budget, void *user) {
	(void) user;

	bool sound = (state == PLAYER_PLAYING)
		&& tinromSpuReadCdCapture(captureLeft, captureRight);

	visualAnalyse(&visual, sound ? captureLeft : NULL,
		sound ? captureRight : NULL, TINROM_SPU_CAPTURE_SAMPLES);
	sceneDraw(ui, budget, &visual, visualMode, tinromFrameCounter());
}

/* L1 and R1, which the grid leaves alone. */
static void pollVisualiser(void) {
	uint16_t pressed = tinromPadPressed(0) | tinromPadPressed(1);
	int      step    = 0;

	if (pressed & TINROM_PAD_R1)
		step++;
	if (pressed & TINROM_PAD_L1)
		step--;

	if (!step)
		return;

	visualMode  = (VisualMode)
		((visualMode + VISUAL_MODES + step) % VISUAL_MODES);
	bannerUntil = tinromFrameCounter() + BANNER_FRAMES;
}

static bool pollGrid(void *user) {
	(void) user;

	pollVisualiser();

	return pollDrive(false);
}

static void drawGridBehind(void *user) {
	TinromContext *ctx = (TinromContext *) user;

	pollDrive(true);
	uiGridDraw(&ctx->ui, &pane, 1, &cursor);
}

static const char *describeCell(
	const UIGridPane *p, unsigned int cell, void *user
) {
	static char detail[64];
	char        length[12];

	(void) p;
	(void) user;

	if ((int32_t) (bannerUntil - tinromFrameCounter()) > 0) {
		snprintf(detail, sizeof(detail), "Visualiser: %s",
			visualModeName(visualMode));

		return detail;
	}

	if (cell == UI_GRID_CELL_MENU)
		return "Pause, stop, skip, repeat, effect and mono";

	if (!haveDisc)
		return "Put an audio CD or a game with music in, and close the lid.";

	unsigned int track = trackAt(cell);

	if ((track < toc.first) || (track > toc.last))
		return NULL;

	if (!isAudio(track)) {
		snprintf(detail, sizeof(detail),
			"Track %u   data - the game itself, not music", track);

		return detail;
	}

	formatTime(length, sizeof(length), tinromCdTrackLength(&toc, track));

	if (track != playing) {
		snprintf(detail, sizeof(detail), "Track %u   %s", track, length);
	} else {
		snprintf(detail, sizeof(detail), "Track %u   %s   Cross %s",
			track, length, (state == PLAYER_PLAYING) ? "pauses" : "resumes");
	}

	return detail;
}

/* --- the "..." menu ----------------------------------------------------- */

enum {
	ACTION_PAUSE, ACTION_STOP, ACTION_NEXT, ACTION_PREVIOUS,
	ACTION_REPEAT, ACTION_EFFECT, ACTION_MONO
};

static void playerMenu(TinromContext *ctx) {
	static const char *const repeatLabels[] = {
		"Repeat: off", "Repeat: track", "Repeat: disc"
	};
	static const char *const effectLabels[TINROM_SPU_REVERB_COUNT] = {
		[TINROM_SPU_REVERB_OFF]    = "Effect: direct",
		[TINROM_SPU_REVERB_STUDIO] = "Effect: studio",
		[TINROM_SPU_REVERB_HALL]   = "Effect: hall",
		[TINROM_SPU_REVERB_CHURCH] = "Effect: church",
		[TINROM_SPU_REVERB_DOME]   = "Effect: dome"
	};

	size_t row = 0;

	/*
	 * Repeat, the effect and mono are settings, so picking one changes it
	 * and leaves the menu open on it; everything else does what it says and
	 * closes.
	 */
	for (;;) {
		bool active = (state != PLAYER_STOPPED);

		const UIMenuItem items[] = {
			{ (state == PLAYER_PAUSED) ? "Resume" : "Pause",
				"Keeps the place", active, ACTION_PAUSE },
			{ "Stop", "Stops and spins the disc down", active, ACTION_STOP },
			{ "Next track", "Skips to the next track", active, ACTION_NEXT },
			{ "Previous track", "Back to the start, or the track before",
				active, ACTION_PREVIOUS },
			{ repeatLabels[repeat], "After a track: the next, the same, "
				"or round again", true, ACTION_REPEAT },
			{ effectLabels[effect], "The room it plays in, or direct: as "
				"recorded", true, ACTION_EFFECT },
			{ mono ? "Sound: mono" : "Sound: stereo",
				"Both channels to both speakers", true, ACTION_MONO }
		};

		int anchorX, anchorY;

		uiGridCellAnchor(0, 1, UI_GRID_CELL_MENU, &anchorX, &anchorY);

		size_t choice = uiPopupRunAt(
			ctx, "Player", items, sizeof(items) / sizeof(items[0]),
			anchorX, anchorY + 2, drawGridBehind, ctx, &row
		);

		if (choice == UI_MENU_CANCELLED)
			return;

		TinromResult result = TINROM_OK;
		const char  *action = items[choice].label;

		switch (items[choice].tag) {
			case ACTION_PAUSE:
				result = togglePause();
				break;

			case ACTION_STOP:
				result = stop();
				break;

			case ACTION_NEXT:
				result = advance();
				break;

			case ACTION_PREVIOUS: {
				unsigned int into = (position.minute * 60u) + position.second;

				result = play((into >= RESTART_SECONDS) ? playing
					: previousTrack(playing));
				break;
			}

			case ACTION_REPEAT:
				repeat = (RepeatMode) ((repeat + 1) % 3);
				continue;

			case ACTION_EFFECT:
				/* Smallest room to largest, then back to none. */
				effect = (TinromSpuReverb)
					((effect + 1) % TINROM_SPU_REVERB_COUNT);
				tinromSpuSetCdReverb(effect);
				continue;

			default:
				mono   = !mono;
				result = tinromCdAudioInit(mono);

				if (result == TINROM_OK)
					continue;
				break;
		}

		if (result != TINROM_OK)
			uiShowResult(ctx, action, result);

		return;
	}
}

/* ------------------------------------------------------------------------ */

static void pick(TinromContext *ctx) {
	unsigned int track = trackAt(cursor.cell);
	TinromResult result;

	if (!isAudio(track))
		return;

	if ((track == playing) && (state != PLAYER_STOPPED))
		result = togglePause();
	else
		result = play(track);

	if (result != TINROM_OK)
		uiShowResult(ctx, "Play", result);
}

static TinromResult cdplayEnter(TinromContext *ctx) {
	UIContext *ui = &ctx->ui;

	/* The visualiser takes the chip's place behind the grid. */
	visualInit(&visual);
	bannerUntil = tinromFrameCounter();
	uiSetBackground(ui, drawVisualiser, NULL);
	uiGridSetHint(HINT);

	pane = (UIGridPane) {
		.title     = "Audio CD",
		.cells     = cells,
		.menuLabel = "...",
	};

	/*
	 * The first look at the disc waits for the first poll, so the screen is
	 * up before a drive that is still spinning up keeps everyone waiting.
	 */
	noDisc("Reading the disc...");

	lastPoll = tinromFrameCounter();
	lastTry  = lastPoll - RETRY_FRAMES;

	TinromResult result = tinromCdAudioInit(mono);

	if (result == TINROM_OK)
		tinromSpuSetCdReverb(effect);
	else
		uiShowResult(ctx, "Start CD audio", result);

	for (;;) {
		UIGridOutcome outcome = uiGridRun(
			ctx, "Audio CD player", &pane, 1, &cursor,
			describeCell, pollGrid, ctx
		);

		if (outcome == UI_GRID_OUTCOME_SELECTED)
			pick(ctx);
		else if (outcome == UI_GRID_OUTCOME_MENU)
			playerMenu(ctx);
		else
			break;
	}

	/*
	 * The music stops with the player, and whatever reads the disc next
	 * would otherwise find the drive in audio mode. A disc that was only
	 * looked at is left spinning.
	 */
	if (state != PLAYER_STOPPED)
		tinromCdStop();

	/* Nothing else in tinrom wants a church around it. */
	tinromSpuSetCdReverb(TINROM_SPU_REVERB_OFF);

	haveDisc = false;
	state    = PLAYER_STOPPED;
	playing  = 0;

	uiSetBackground(ui, NULL, NULL);
	uiGridSetHint(NULL);

	return TINROM_OK;
}

const TinromModule tinromModule_cdplay = {
	.id      = "cdplay",
	.name    = "Audio disc player",
	.summary = "Play an audio CD",
	.minTier = TINROM_TIER_BASIC,
	.flags   = TINROM_MODULE_MENU | TINROM_MODULE_AUTOSTART,
	.init    = NULL,
	.enter   = cdplayEnter,
	.tick    = NULL,
	.shutdown = NULL
};
