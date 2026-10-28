/* tinrom - (C) 2026 TinWhisker - MIT licensed
 *
 * Host-side test of tinrom's saves: the card driver's file functions
 * (src/hal/mcard.c) talking to two emulated memory cards at the SIO0 byte
 * level, the save container (core/savefile.c), the settings
 * (core/settings.c) and the terminal's bookmarks (comms/config.c) on top.
 * Run by scripts/test-saves.sh.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "comms/config.h"
#include "core/app.h"
#include "core/module.h"
#include "core/profile.h"
#include "core/savefile.h"
#include "core/settings.h"
#include "hal/exp.h"
#include "hal/mcard.h"

volatile uint32_t fakeGp0;
static int fails;
#define CHECK(c) do { if (!(c)) { printf("  FAIL %d: %s\n", __LINE__, #c); fails++; } } while (0)

/* --- two memory cards on SIO0 -------------------------------------------- */

static uint8_t  cards[2][128 * 1024];
static bool     inserted[2] = { true, true };
static unsigned selected, step;
static uint8_t  command, sectorHi, sectorLo, sum, incoming[128];
static unsigned writes;

/*
 * A real card programs its flash after a write and answers nothing, its
 * address included, until it is done; the driver has to wait it out. Each
 * card here ignores this many selects after every write.
 */
#define BUSY_SELECTS 3
static unsigned busy[2];

void tinromSio0Select(unsigned int port) { selected = port; step = 0; }
void tinromSio0Deselect(void) { step = 0; }
bool tinromSio0WaitAck(unsigned int timeout) {
	(void) timeout;
	/* The address is byte 0, so its /ACK is waited for with step at 1. */
	if ((step == 1) && busy[selected]) { busy[selected]--; return false; }
	return inserted[selected];
}

uint8_t tinromSio0Exchange(uint8_t value) {
	uint8_t *card = cards[selected];
	unsigned at   = step++;
	unsigned frame = ((unsigned) sectorHi << 8) | sectorLo;

	if (!inserted[selected])
		return 0xff;

	switch (at) {
		case 0: return 0xff;
		case 1: command = value; return 0x08;
		case 2: return 0x5a;
		case 3: return 0x5d;
		case 4: sectorHi = value; return 0x00;
		case 5: sectorLo = value; return sectorHi;
	}

	if (command == 'R') {
		if (at == 6) return 0x5c;
		if (at == 7) return 0x5d;
		if (at == 8) { sum = sectorHi ^ sectorLo; return sectorHi; }
		if (at == 9) return sectorLo;
		if (at < 138) {
			uint8_t b = card[frame * 128 + (at - 10)];
			sum ^= b;
			return b;
		}
		if (at == 138) return sum;
		return 'G';
	}

	/* Write. */
	if (at < 134) { incoming[at - 6] = value; return 0; }
	if (at == 134) {
		uint8_t want = sectorHi ^ sectorLo;
		for (int i = 0; i < 128; i++) want ^= incoming[i];
		sum = (want == value);
		return 0;
	}
	if (at == 135) return 0x5c;
	if (at == 136) return 0x5d;
	if (!sum) return 'N';
	memcpy(&card[frame * 128], incoming, 128);
	writes++;
	busy[selected] = BUSY_SELECTS;
	return 'G';
}

static void format(unsigned port) {
	uint8_t *card = cards[port];

	memset(card, 0, sizeof(cards[port]));
	card[0] = 'M'; card[1] = 'C';
	for (int f = 1; f <= 15; f++) {
		uint8_t *e = &card[f * 128];
		e[0] = 0xa0; e[8] = 0xff; e[9] = 0xff;
		uint8_t x = 0;
		for (int i = 0; i < 127; i++) x ^= e[i];
		e[127] = x;
	}
}

/* Fills every free block with a game's save, so nothing is left. */
static void fill(unsigned port) {
	uint8_t *card = cards[port];

	for (int f = 1; f <= 15; f++) {
		uint8_t *e = &card[f * 128];
		if (e[0] != 0xa0 && e[0] != 0xa1 && e[0] != 0xa2 && e[0] != 0xa3)
			continue;
		memset(e, 0, 128);
		e[0] = 0x51; e[4] = 0x00; e[5] = 0x20; e[8] = 0xff; e[9] = 0xff;
		snprintf((char *) &e[10], 20, "BASLUS-0%04dGAME", f);
		uint8_t x = 0;
		for (int i = 0; i < 127; i++) x ^= e[i];
		e[127] = x;
	}
}

static unsigned countInUse(unsigned port, const char *name) {
	unsigned n = 0;
	for (int f = 1; f <= 15; f++) {
		uint8_t *e = &cards[port][f * 128];
		if (e[0] == 0x51 && !strncmp((char *) &e[10], name, 20)) n++;
	}
	return n;
}

/*
 * The bookmarks' save serial in `name` on `port`; with `set`, writes that
 * serial instead (rehashing the container) and returns it.
 */
static uint32_t commsSerialAt(unsigned port, const char *name, uint32_t set) {
	unsigned slot;
	if (tinromMcFindFile(port, name, &slot) != TINROM_OK) return 0;
	uint8_t *h = &cards[port][(slot + 1) * 8192 + 256];
	uint16_t count; uint32_t length;
	memcpy(&count, h + 6, 2); memcpy(&length, h + 8, 4);
	uint32_t at = 0, serial = 0;
	for (unsigned i = 0; i < count; i++) {
		uint32_t tag, len;
		memcpy(&tag, h + 16 + at, 4); memcpy(&len, h + 16 + at + 4, 4);
		if (tag == TINROM_SECTION_COMMS) {
			uint8_t *p = h + 16 + at + 8 + 8;
			if (set) memcpy(p, &set, 4);
			memcpy(&serial, p, 4);
		}
		at += 8 + ((len + 3) & ~3u);
	}
	if (set) {
		uint32_t hash = 0x811c9dc5;
		for (uint32_t i = 0; i < length; i++) { hash ^= h[16 + i]; hash *= 0x01000193; }
		memcpy(h + 12, &hash, 4);
	}
	return serial;
}

/* --- the rest of tinrom, faked ------------------------------------------- */

void tinromLogWrite(const char *f, ...) { (void) f; }
void tinromDelayMicroseconds(int us) { (void) us; }
static bool switchOn;
bool tinromExpSwitchOn(TinromSwitchSource source) {
	return (source != TINROM_SWITCH_NONE) && switchOn;
}
const char *tinromResultString(TinromResult r) { return "err"; }

static const TinromModule modules[] = {
	{ .id = "about", .minTier = TINROM_TIER_BASIC },
	{ .id = "terminal", .minTier = TINROM_TIER_ADVANCED },
	{ .id = "hexedit", .minTier = TINROM_TIER_DEVELOPER }
};
static size_t moduleCount = 3;
size_t tinromModuleCount(void) { return moduleCount; }
const TinromModule *tinromModuleAt(size_t i) { return &modules[i]; }

static TinromContext context;
TinromContext *tinromContext(void) { return &context; }

void commsModemDefaults(CommsModemProfile *p) {
	memset(p, 0, sizeof(*p));
	strcpy(p->init, "ATE0V1");
	strcpy(p->dialPrefix, "ATDT");
	p->baud = 9600;
}

int main(void) {
	format(0);
	format(1);

	puts("settings");
	TinromSettings s;
	tinromSettingsDefaults(&s);
	CHECK(tinromSettingsLoad(&s) == TINROM_ERR_NOT_PRESENT);
	CHECK(tinromSettingsCard() == -1 && tinromSettingsSaveCard() == 0);

	s.videoPreference = TINROM_VIDEO_PAL;
	CHECK(tinromSettingsSave(&s, TINROM_STORE_MEMCARD) == TINROM_OK);
	CHECK(countInUse(0, "BETNRM-00001SETTINGS") == 1);
	CHECK(tinromSettingsCard() == 0);

	/* It reads back, and the directory shows the title. */
	TinromSettings back;
	tinromSettingsDefaults(&back);
	CHECK(tinromSettingsLoad(&back) == TINROM_OK);
	CHECK(back.videoPreference == TINROM_VIDEO_PAL);
	TinromMcSlot slots[15];
	CHECK(tinromMcReadDirectory(0, slots) == TINROM_OK);
	CHECK(!strcmp(slots[0].title, "tinrom settings"));

	/* Card 2 too; card 1 still wins at load time. */
	s.videoPreference = TINROM_VIDEO_NTSC;
	CHECK(tinromSettingsSaveToCard(&s, 1) == TINROM_OK);
	CHECK(tinromSettingsLoad(&back) == TINROM_OK);
	CHECK(back.videoPreference == TINROM_VIDEO_PAL && tinromSettingsCard() == 0);

	/* With card 1 out, card 2's. */
	inserted[0] = false;
	CHECK(tinromSettingsLoad(&back) == TINROM_OK);
	CHECK(back.videoPreference == TINROM_VIDEO_NTSC && tinromSettingsCard() == 1);
	inserted[0] = true;

	/* Saving again replaces, rather than adding a second file. */
	CHECK(tinromSettingsSaveToCard(&s, 0) == TINROM_OK);
	CHECK(countInUse(0, "BETNRM-00001SETTINGS") == 1);

	/* A build with other modules keeps the settings, redoes the module mask. */
	s.moduleEnableMask = 0;
	CHECK(tinromSettingsSaveToCard(&s, 0) == TINROM_OK);
	moduleCount = 2;
	CHECK(tinromSettingsLoad(&back) == TINROM_OK);
	CHECK(back.videoPreference == TINROM_VIDEO_NTSC && back.moduleEnableMask != 0);
	moduleCount = 3;

	puts("bookmarks");
	context.settings = s;
	CommsConfig *config = commsConfig();

	/* With the settings on card 1: the settings stay readable. */
	strcpy(config->bookmarks[0].name, "First");
	CommsPlace withSettings = { 0, COMMS_PLACE_SETTINGS };
	CHECK(commsConfigSaveTo(withSettings) == TINROM_OK);
	CHECK(tinromSettingsLoad(&back) == TINROM_OK);
	CHECK(commsConfigSavedAt(withSettings));

	/* Then on their own on card 2, newer. */
	strcpy(config->bookmarks[0].name, "Second");
	CommsPlace own2 = { 1, COMMS_PLACE_OWN };
	CHECK(commsConfigSaveTo(own2) == TINROM_OK);
	CHECK(countInUse(1, "BETNRM-00002COMMS") == 1);

	/* Saving settings to card 1 keeps the bookmarks in that file. */
	CHECK(tinromSettingsSaveToCard(&s, 0) == TINROM_OK);
	CHECK(commsConfigSavedAt(withSettings));

	/* The newest is the one loaded. */
	strcpy(config->bookmarks[0].name, "changed");
	CHECK(commsConfigLoadOnce() == TINROM_OK);
	CHECK(!strcmp(commsConfig()->bookmarks[0].name, "Second"));
	CommsPlace place;
	CHECK(commsConfigCurrentPlace(&place) && place.port == 1
		&& place.kind == COMMS_PLACE_OWN);

	/* And the older one on request. */
	CHECK(commsConfigLoadFrom(withSettings) == TINROM_OK);
	CHECK(!strcmp(commsConfig()->bookmarks[0].name, "First"));

	/* A settings file made to hold bookmarks is a settings file too. */
	format(1);
	CommsPlace with2 = { 1, COMMS_PLACE_SETTINGS };
	CHECK(commsConfigSaveTo(with2) == TINROM_OK);
	inserted[0] = false;
	CHECK(tinromSettingsLoad(&back) == TINROM_OK && tinromSettingsCard() == 1);
	inserted[0] = true;

	puts("full cards");
	format(0);
	fill(0);
	CHECK(tinromSettingsSaveToCard(&s, 0) == TINROM_ERR_NO_SPACE);
	CHECK(commsConfigSaveTo(withSettings) == TINROM_ERR_NO_SPACE);

	/* Full, but with tinrom's own file there: written over in place. */
	format(0);
	CHECK(tinromSettingsSaveToCard(&s, 0) == TINROM_OK);
	fill(0);
	s.videoPreference = TINROM_VIDEO_AUTO;
	CHECK(tinromSettingsSaveToCard(&s, 0) == TINROM_OK);
	CHECK(countInUse(0, "BETNRM-00001SETTINGS") == 1);
	CHECK(tinromSettingsLoad(&back) == TINROM_OK
		&& back.videoPreference == TINROM_VIDEO_AUTO);

	/* No card at all. */
	inserted[0] = inserted[1] = false;
	CHECK(tinromSettingsSaveCard() == -1);
	CHECK(tinromSettingsSave(&s, TINROM_STORE_MEMCARD) == TINROM_ERR_NOT_PRESENT);
	CHECK(commsConfigSaveTo(withSettings) == TINROM_ERR_NOT_PRESENT);
	inserted[0] = inserted[1] = true;

	/* A damaged file is refused, not believed. */
	format(0);
	CHECK(tinromSettingsSaveToCard(&s, 0) == TINROM_OK);
	unsigned slot;
	CHECK(tinromMcFindFile(0, "BETNRM-00001SETTINGS", &slot) == TINROM_OK);
	cards[0][(slot + 1) * 8192 + 256 + 20] ^= 0x55;
	inserted[1] = false;
	CHECK(tinromSettingsLoad(&back) == TINROM_ERR_CORRUPT);
	inserted[1] = true;

	puts("damage");
	/* Our entry's chain pointing into a game's save: refuse, touch nothing. */
	format(0);
	CHECK(tinromSettingsSaveToCard(&s, 0) == TINROM_OK);
	CHECK(tinromMcFindFile(0, "BETNRM-00001SETTINGS", &slot) == TINROM_OK);
	fill(0);
	{
		uint8_t *e = &cards[0][(slot + 1) * 128];
		unsigned game = (slot == 0) ? 1 : 0;
		e[8] = (uint8_t) game; e[9] = 0;
		uint8_t x = 0;
		for (int i = 0; i < 127; i++) x ^= e[i];
		e[127] = x;

		uint8_t before[128];
		memcpy(before, &cards[0][(game + 1) * 128], 128);
		CHECK(tinromSettingsSaveToCard(&s, 0) == TINROM_ERR_CORRUPT);
		CHECK(!memcmp(before, &cards[0][(game + 1) * 128], 128));
		inserted[1] = false;
		CHECK(tinromSettingsLoad(&back) != TINROM_OK);
		inserted[1] = true;
	}

	/* A good hash over a section that runs past the end: corrupt, no crash. */
	format(0);
	CHECK(tinromSettingsSaveToCard(&s, 0) == TINROM_OK);
	CHECK(tinromMcFindFile(0, "BETNRM-00001SETTINGS", &slot) == TINROM_OK);
	{
		uint8_t *h = &cards[0][(slot + 1) * 8192 + 256];
		uint32_t length; memcpy(&length, h + 8, 4);
		uint32_t big = 0xfffffff8u;
		memcpy(h + 16 + 4, &big, 4);          /* first section's length */
		uint32_t hash = 0x811c9dc5;
		for (uint32_t i = 0; i < length; i++) { hash ^= h[16 + i]; hash *= 0x01000193; }
		memcpy(h + 12, &hash, 4);
		inserted[1] = false;
		CHECK(tinromSettingsLoad(&back) == TINROM_ERR_CORRUPT);
		CHECK(tinromSettingsSaveToCard(&s, 0) == TINROM_OK);
		CHECK(tinromSettingsLoad(&back) == TINROM_OK);
		inserted[1] = true;
	}

	puts("late card");
	/*
	 * Bookmarks on a card the terminal never read - put in later - carry a
	 * serial it has not seen. A save must still beat them.
	 */
	format(0); format(1);
	CHECK(commsConfigSaveTo(own2) == TINROM_OK);
	CHECK(commsSerialAt(1, "BETNRM-00002COMMS", 5000));
	CHECK(commsConfigSaveTo(withSettings) == TINROM_OK);
	CHECK(commsSerialAt(0, "BETNRM-00001SETTINGS", 0) > 5000);

	puts("reuse");
	{
		/*
		 * Two games, a free block, and another game's deleted save further
		 * on. Saving again and again should swap between two blocks - the
		 * free one and the next - and leave the deleted game alone.
		 */
		format(0);
		inserted[1] = false;

		for (int slot = 0; slot < 2; slot++) {
			uint8_t *e = &cards[0][(slot + 1) * 128];
			memset(e, 0, 128);
			e[0] = 0x51; e[5] = 0x20; e[8] = 0xff; e[9] = 0xff;
			snprintf((char *) &e[10], 20, "BASCUS-9442%dGAME", slot);
			uint8_t x = 0;
			for (int i = 0; i < 127; i++) x ^= e[i];
			e[127] = x;
		}
		{
			uint8_t *e = &cards[0][(5 + 1) * 128];
			memset(e, 0, 128);
			e[0] = 0xa1; e[5] = 0x20; e[8] = 0xff; e[9] = 0xff;
			strcpy((char *) &e[10], "BASLUS-00001OLD");
			uint8_t x = 0;
			for (int i = 0; i < 127; i++) x ^= e[i];
			e[127] = x;
		}

		TinromSettings r;
		tinromSettingsDefaults(&r);
		tinromProfilesReset(&r);

		static const int expect[] = { 2, 3, 2, 3 };

		for (int n = 0; n < 4; n++) {
			unsigned slot = 99;
			CHECK(tinromSettingsSaveToCard(&r, 0) == TINROM_OK);
			CHECK(tinromMcFindFile(0, "BETNRM-00001SETTINGS", &slot) == TINROM_OK);
			CHECK((int) slot == expect[n]);
			CHECK(countInUse(0, "BETNRM-00001SETTINGS") == 1);
		}

		CHECK(cards[0][(5 + 1) * 128] == 0xa1);   /* still recoverable */
		CHECK(cards[0][(4 + 1) * 128] == 0xa0);   /* never needed */
		CHECK(tinromSettingsLoad(&r) == TINROM_OK);
		inserted[1] = true;
	}

	puts("cart switch");
	{
		format(0); format(1);
		inserted[1] = false;

		TinromSettings a;
		tinromSettingsDefaults(&a);
		tinromProfilesReset(&a);

		/* Switch off: advanced, terminal only. */
		switchOn = false;
		tinromSwitchSetSource(&a, TINROM_SWITCH_XPLORER);
		CHECK(tinromProfileActive() == TINROM_PROFILE_OFF);
		tinromSettingsApplyTier(&a, TINROM_TIER_ADVANCED);
		a.moduleEnableMask = 1u << 1;

		/* Switch on: developer, hexedit, autoboot into it. */
		/* ON starts as everything did, before OFF was changed. */
		TinromSettings fresh;
		tinromSettingsDefaults(&fresh);
		tinromProfileUse(&a, TINROM_PROFILE_ON);
		CHECK(a.tier == fresh.tier && a.moduleEnableMask == fresh.moduleEnableMask);
		tinromSettingsApplyTier(&a, TINROM_TIER_DEVELOPER);
		a.moduleEnableMask = 1u << 2;
		a.autobootModule   = 2;
		a.videoPreference  = TINROM_VIDEO_PAL; /* not a profile's */
		tinromSwitchSetLive(true);

		/* Saved while ON is in use: the settings section still holds OFF. */
		CHECK(tinromSettingsSaveToCard(&a, 0) == TINROM_OK);
		CHECK(tinromSaveLoad(0, &tinromSaveSettings) == TINROM_OK);
		size_t length;
		const uint8_t *raw = tinromSaveSection(TINROM_SECTION_SETTINGS, &length);
		TinromSettings inFile;
		CHECK(raw && length == sizeof(TinromSettings) + 4);
		memcpy(&inFile, raw, sizeof(inFile));
		CHECK(inFile.tier == TINROM_TIER_ADVANCED && inFile.moduleEnableMask == (1u << 1));
		CHECK(inFile.autobootModule == 0xff && inFile.videoPreference == TINROM_VIDEO_PAL);
		CHECK(tinromSaveSection(TINROM_SECTION_SWITCH, NULL) != NULL);

		/* Power-on with it off, then on. */
		TinromSettings b;
		for (int pass = 0; pass < 2; pass++) {
			tinromSettingsDefaults(&b);
			tinromProfilesReset(&b);
			switchOn = pass == 1;
			CHECK(tinromSettingsLoad(&b) == TINROM_OK);
			tinromSwitchBoot(&b);
			CHECK(tinromSwitchSource() == TINROM_SWITCH_XPLORER && tinromSwitchLive());
			if (pass == 0)
				CHECK(b.tier == TINROM_TIER_ADVANCED && b.moduleEnableMask == (1u << 1)
					&& b.autobootModule == 0xff);
			else
				CHECK(b.tier == TINROM_TIER_DEVELOPER && b.moduleEnableMask == (1u << 2)
					&& b.autobootModule == 2);
			CHECK(b.videoPreference == TINROM_VIDEO_PAL);
		}

		/* Live: a flip needs two readings that agree before it swaps. */
		switchOn = false;
		CHECK(!tinromSwitchPoll(&b, 1000));
		CHECK(!tinromSwitchPoll(&b, 1005));      /* too soon to read */
		CHECK(tinromSwitchPoll(&b, 1015));
		CHECK(b.tier == TINROM_TIER_ADVANCED && tinromProfileActive() == TINROM_PROFILE_OFF);
		switchOn = true;
		CHECK(!tinromSwitchPoll(&b, 1030));
		switchOn = false;                        /* a bounce: back again */
		CHECK(!tinromSwitchPoll(&b, 1045));
		CHECK(b.tier == TINROM_TIER_ADVANCED);
		tinromSwitchSetLive(false);
		switchOn = true;
		CHECK(!tinromSwitchPoll(&b, 1060) && !tinromSwitchPoll(&b, 1075));

		/* Not read any more: what is in use becomes the one profile. */
		tinromProfileUse(&b, TINROM_PROFILE_ON);
		tinromSwitchSetSource(&b, TINROM_SWITCH_NONE);
		CHECK(b.tier == TINROM_TIER_DEVELOPER && tinromProfileActive() == TINROM_PROFILE_OFF);
		CHECK(tinromSwitchLabel() == NULL);
		CHECK(tinromSettingsSaveToCard(&b, 0) == TINROM_OK);
		tinromSettingsDefaults(&b);
		tinromProfilesReset(&b);
		CHECK(tinromSettingsLoad(&b) == TINROM_OK);
		tinromSwitchBoot(&b);
		CHECK(b.tier == TINROM_TIER_DEVELOPER && b.moduleEnableMask == (1u << 2));

		/* Another build's modules: both profiles redone, autoboot dropped. */
		tinromSwitchSetSource(&b, TINROM_SWITCH_XPLORER);   /* switch on */
		CHECK(tinromProfileActive() == TINROM_PROFILE_ON);
		CHECK(tinromSettingsSaveToCard(&b, 0) == TINROM_OK);
		moduleCount = 2;
		tinromSettingsDefaults(&b);
		tinromProfilesReset(&b);
		CHECK(tinromSettingsLoad(&b) == TINROM_OK);
		tinromSwitchBoot(&b);
		CHECK(b.autobootModule == 0xff && b.tier == TINROM_TIER_DEVELOPER);
		moduleCount = 3;

		inserted[1] = true;
		switchOn = false;
	}

	puts("version 1");
	/*
	 * What the first terminal release saved: bookmarks without IRC fields.
	 * Read and brought up to date, so nothing saved then is lost.
	 */
	{
		struct {
			char    name[COMMS_NAME_MAX];
			char    target[COMMS_TARGET_MAX];
			uint8_t kind, telnet;
		} typedef V1Bookmark;
		struct {
			CommsModemProfile modem;
			V1Bookmark        bookmarks[COMMS_BOOKMARKS];
			uint8_t           bookmarkCount;
			char              phrases[COMMS_PHRASES][COMMS_PHRASE_MAX];
		} typedef V1Config;
		static struct {
			uint32_t version, size, serial;
			V1Config config;
		} v1;

		memset(&v1, 0, sizeof(v1));
		v1.version = 1;
		v1.size    = sizeof(V1Config);
		v1.serial  = 3;
		v1.config.modem.baud     = 19200;
		v1.config.bookmarkCount  = 2;
		strcpy(v1.config.bookmarks[0].name, "Old BBS");
		strcpy(v1.config.bookmarks[0].target, "old.bbs:23");
		v1.config.bookmarks[0].telnet = COMMS_TELNET_ON;
		strcpy(v1.config.bookmarks[1].name, "Old IRC");
		strcpy(v1.config.bookmarks[1].target, "irc.old:6667");
		v1.config.bookmarks[1].kind = COMMS_KIND_IRC;
		strcpy(v1.config.phrases[0], "hello");

		format(0);
		tinromSaveClear();
		CHECK(tinromSaveSetSection(TINROM_SECTION_COMMS, &v1, sizeof(v1)) == TINROM_OK);
		CHECK(tinromSaveStore(0, &tinromSaveComms) == TINROM_OK);

		CommsPlace own1 = { 0, COMMS_PLACE_OWN };
		CHECK(commsConfigLoadFrom(own1) == TINROM_OK);

		CommsConfig *c = commsConfig();
		CHECK(c->modem.baud == 19200 && c->bookmarkCount == 2);
		CHECK(!strcmp(c->bookmarks[0].name, "Old BBS") && c->bookmarks[0].telnet == COMMS_TELNET_ON);
		CHECK(!strcmp(c->bookmarks[1].target, "irc.old:6667") && c->bookmarks[1].kind == COMMS_KIND_IRC);
		CHECK(c->bookmarks[1].irc.nick[0] && !c->bookmarks[1].irc.serverPassword.state);
		CHECK(!strcmp(c->phrases[0], "hello") && !c->vault.enabled);

		/* Saved again, it is the current version. */
		CHECK(commsConfigSaveTo(own1) == TINROM_OK);
		CHECK(commsConfigLoadFrom(own1) == TINROM_OK);
		CHECK(!strcmp(commsConfig()->bookmarks[0].name, "Old BBS"));
	}

	puts("save titles");
	{
		/*
		 * A game's title as games write them: plain ASCII, full-width
		 * letters, and a prime for an apostrophe - then every other
		 * quote-like mark the table approximates, and the degree sign
		 * beside the primes, which has no ASCII form.
		 */
		static const uint8_t title[] = {
			'S', 'p', 'y', 'r', 'o', '2', 0x81, 0x40,
			0x82, 0x71, 0x82, 0x89, 0x82, 0x90, 0x82, 0x94, 0x82, 0x8f,
			0x81, 0x8c, 0x82, 0x93, 0x81, 0x40,
			0x81, 0x65, 0x81, 0x66, 0x81, 0x67, 0x81, 0x68, 0x81, 0x4c,
			0x81, 0x8d, 0x81, 0x8b, 0x82, 0xa0, 0
		};
		TinromMcSlot titled[15];

		format(0);
		CHECK(tinromSettingsSaveToCard(&s, 0) == TINROM_OK);
		memset(&cards[0][8192 + 4], 0, 64);
		memcpy(&cards[0][8192 + 4], title, sizeof(title));
		CHECK(tinromMcReadDirectory(0, titled) == TINROM_OK);
		CHECK(!strcmp(titled[0].title, "Spyro2 Ripto's ''\"\"'\"??"));
	}

	printf(fails ? "%d failure(s)\n" : "all passed (0 failures)\n", fails);

	return fails ? 1 : 0;
}
