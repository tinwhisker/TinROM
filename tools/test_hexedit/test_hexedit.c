/* tinrom - (C) 2026 TinWhisker - MIT licensed
 *
 * Host-side test of the hex editor: the real hexedit.c, find.c, card.c,
 * dial.c and view.c, driven by scripted pad input against fake RAM and a
 * fake memory card. Run by scripts/test-hexedit.sh.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "modules/hexedit/hexedit.h"
volatile uint32_t fakeGp0;
typedef struct { uint16_t pressed, repeated; } Step;
static Step script[512]; static int steps, pos, frames;
static uint16_t curP, curR;
void uploadIndexedTexture(TextureInfo *i, const void *img, const void *pal, unsigned ix, unsigned iy, unsigned px, unsigned py, unsigned w, unsigned h, GP0ColorDepth d) {}
void sendVRAMData(const void *d, unsigned x, unsigned y, unsigned w, unsigned h) {}
void waitForGPUDMADone(void) {}
void waitForGP0Ready(void) {}
void uiFillRect(UIContext *ui, int x, int y, int w, int h, uint32_t c) {}
void uiCanvasDraw(UIContext *ui, const UICanvas *c, int x, int y, int w, unsigned rows) {}
void uiText(UIContext *ui, int x, int y, const char *t) {}
void uiTextColored(UIContext *ui, int x, int y, uint32_t c, const char *t) {}
int uiTextWidth(const char *t) { return uiFontTextWidth(t); }
static bool modelShown = true; bool uiModelVisible(const UIContext *ui) { return modelShown; } void uiSetModelVisible(UIContext *ui, bool v) { modelShown = v; }
void uiSetChrome(UIContext *ui, const char *t, const char *h) { if (h && !strcmp(h, "Circle: stop searching")) { if (pos > 0 && curP != TINROM_PAD_CIRCLE) pos--; curP = 0; curR = 0; } }
void uiProgressBar(UIContext *ui, int x, int y, int w, uint32_t d, uint32_t t) {}
static TinromContext ctx; TinromContext *tinromContext(void) { return &ctx; }
void flushCache(void) {}
void tinromLogWrite(const char *f, ...) {}
const char *tinromResultString(TinromResult r) { return "err"; }
/* scripted input */
void uiBeginFrame(UIContext *ui) { frames++; if (pos < steps) { curP = script[pos].pressed; curR = script[pos].repeated; pos++; } else { curP = TINROM_PAD_CIRCLE; curR = 0; } if (frames > 100000) { puts("runaway"); exit(2);} }
void uiEndFrame(UIContext *ui) {}
uint16_t tinromPadPressed(unsigned p) { return p ? 0 : curP; }
uint16_t tinromPadRepeated(unsigned p) { return p ? 0 : curR; }
static void press(uint16_t b) { script[steps++] = (Step){ b, b }; }
static void rep(uint16_t b) { script[steps++] = (Step){ 0, b }; }
static int confirmAnswer = 1; static int confirms;
bool uiConfirm(TinromContext *c, const char *t, const char *b, bool d) { confirms++; return confirmAnswer; }
static int popupAnswers[16], popups, npop;
size_t uiPopupRunAt(TinromContext *c, const char *t, const UIMenuItem *it, size_t n, int x, int y, void (*bg)(void*), void *u, size_t *cur) { bg(u); return popups < npop ? (size_t) popupAnswers[popups++] : UI_MENU_CANCELLED; }
size_t uiPopupRun(TinromContext *c, const char *t, const UIMenuItem *it, size_t n, int x, int y, void (*bg)(void*), void *u) { return uiPopupRunAt(c,t,it,n,x,y,bg,u,NULL); }
static int menuPick = -1; static int menuCalls;
size_t uiMenuRunAt(TinromContext *c, const char *t, const UIMenuItem *it, size_t n, size_t *cur) { return (menuCalls++ == 0) ? (size_t) menuPick : UI_MENU_CANCELLED; }
void uiShowResult(TinromContext *c, const char *a, TinromResult r) { printf("showResult %s %d\n", a, r); }
/* fake memory regions */
uint8_t fakeRam[0x200000];
const HexRegion hexRegions[] = {
	{ "Main RAM", "", HEX_REGION_MEMORY, 0x80000000u, 0x200000, true, 0, 0x1000 },
	{ "BIOS ROM", "", HEX_REGION_MEMORY, 0xbfc00000u, 0x80000, false, 0, 0x1000 },
	{ "Memory card 1", "", HEX_REGION_CARD, 0, 0x20000, true, 0, 0x2000 } };
const unsigned hexRegionCount = 3;
int writes;
bool hexRead(const HexRegion *r, uint32_t off, uint32_t size, uint32_t *v) { uint32_t x=0; memcpy(&x, &fakeRam[off], size); *v=x; return true; }
bool hexWrite(const HexRegion *r, uint32_t off, uint32_t size, uint32_t v) { if (!r->writable) return false; writes++; memcpy(&fakeRam[off], &v, size); return true; }
const char *hexRamOwner(const HexRegion *r, uint32_t o) { return (r->kind == HEX_REGION_MEMORY && o < 0x10000) ? "kernel" : NULL; }
bool hexAddressToOffset(const HexRegion *r, uint32_t a, uint32_t *o) { if (r->kind == HEX_REGION_CARD) { if (a >= r->size) return false; *o = a; return true; } if ((a & 0x1fffffff) >= 0x200000) return false; *o = a & 0x1fffff; return true; }
uint8_t card[0x20000]; int cardWrites;
TinromResult tinromMcProbe(unsigned p, TinromMcInfo *i) { return TINROM_OK; }
TinromResult tinromMcReadFrame(unsigned p, uint16_t f, void *d) { memcpy(d, &card[f * 128], 128); return TINROM_OK; }
TinromResult tinromMcWriteFrame(unsigned p, uint16_t f, const void *d) { cardWrites++;  memcpy(&card[f * 128], d, 128); return TINROM_OK; }
extern const TinromModule tinromModule_hexedit;
static int fails;
#define CHECK(c) do { if (!(c)) { printf("  FAIL %d: %s\n", __LINE__, #c); fails++; } } while (0)
static void reset(int region) { steps = pos = frames = 0; menuCalls = 0; menuPick = region; popups = npop = 0; confirms = 0; confirmAnswer = 1; }
static void run(void) { tinromModule_hexedit.enter(&ctx); }
int main(void) {
	/* 1. RAM: move right twice, edit: roll high nibble up 1, low down 1, leave right -> written; undo via menu */
	for (int i = 0; i < 0x200000; i++) fakeRam[i] = 0;
	fakeRam[0x10002] = 0x42;
	reset(0);
	rep(TINROM_PAD_RIGHT); rep(TINROM_PAD_RIGHT);
	press(TINROM_PAD_CROSS); rep(TINROM_PAD_UP); rep(TINROM_PAD_RIGHT); rep(TINROM_PAD_DOWN); rep(TINROM_PAD_RIGHT);
	press(TINROM_PAD_CIRCLE); /* leave edit (next unit unchanged) */
	run();
	CHECK(fakeRam[0x10002] == 0x51); CHECK(writes == 1);
	/* 2. halfword view edit crossing and undo through menu (item 5) */
	writes = 0; reset(0);
	fakeRam[0x10000] = 0x34; fakeRam[0x10001] = 0x12;
	press(TINROM_PAD_SQUARE); press(TINROM_PAD_CROSS);
	rep(TINROM_PAD_UP); /* 1234 -> 2234 */
	press(TINROM_PAD_CROSS);
	press(TINROM_PAD_TRIANGLE); popupAnswers[npop++] = 5; /* undo */
	run();
	CHECK(writes == 2); CHECK(fakeRam[0x10000] == 0x34 && fakeRam[0x10001] == 0x12);
	/* 3. read-only BIOS: cross does nothing */
	writes = 0; reset(1); press(TINROM_PAD_CROSS); rep(TINROM_PAD_UP); press(TINROM_PAD_CROSS); run(); CHECK(writes == 0);
	/* 4. Find word 0xDEADBEEF forwards: menu Find (1) -> kind Word (2) -> dial */
	memset(fakeRam, 0, sizeof(fakeRam)); fakeRam[0x123454] = 0xef; fakeRam[0x123455] = 0xbe; fakeRam[0x123456] = 0xad; fakeRam[0x123457] = 0xde;
	fakeRam[0x123451] = 0xef; /* unaligned decoy */
	reset(0);
	press(TINROM_PAD_TRIANGLE); popupAnswers[npop++] = 1; popupAnswers[npop++] = 2;
	/* dial 8 digits from 00000000 to DEADBEEF: for each digit roll down (0->F->E..) */
	{ const char *target = "DEADBEEF"; for (int d = 0; d < 8; d++) { int v = (target[d] <= '9') ? target[d]-'0' : target[d]-'A'+10; for (int k = 0; k < v; k++) rep(TINROM_PAD_UP); if (d < 7) rep(TINROM_PAD_RIGHT); } }
	press(TINROM_PAD_CROSS);
	/* now in browse at found address: enter edit, check cursor via status by editing byte */
	press(TINROM_PAD_SQUARE); /* half */ press(TINROM_PAD_SQUARE); /* word */ press(TINROM_PAD_SQUARE); /* byte */
	press(TINROM_PAD_CROSS); rep(TINROM_PAD_UP); press(TINROM_PAD_CROSS);
	run();
	CHECK(fakeRam[0x123454] == 0xff);
	/* 5. Text search: find "HI" */
	memset(fakeRam, 0, sizeof(fakeRam)); memcpy(&fakeRam[0x40000], "xxHIyy", 6);
	reset(0);
	press(TINROM_PAD_TRIANGLE); popupAnswers[npop++] = 1; popupAnswers[npop++] = 3;
	/* text dial starts "A": roll up 7 -> 'H', right (append 'H'), up 1 -> 'I' */
	for (int k = 0; k < 7; k++) rep(TINROM_PAD_UP); rep(TINROM_PAD_RIGHT); rep(TINROM_PAD_UP); press(TINROM_PAD_CROSS);
	press(TINROM_PAD_CROSS); rep(TINROM_PAD_DOWN); press(TINROM_PAD_CROSS); /* H(48) -> 38 */
	run();
	CHECK(fakeRam[0x40002] == 0x38);
	/* 6. Card: edit a directory frame byte, move to next frame -> prompt Write (0) -> checksum fixed, written */
	memset(card, 0, sizeof(card)); card[128 + 5] = 0x11; card[128 + 127] = 0x11;
	reset(2);
	press(TINROM_PAD_R1); /* frame 1 */
	press(TINROM_PAD_CROSS); rep(TINROM_PAD_UP); press(TINROM_PAD_CROSS); /* byte 0: 00 -> 10 */
	press(TINROM_PAD_R1); popupAnswers[npop++] = 0; /* write */
	run();
	CHECK(cardWrites == 1); CHECK(card[128] == 0x10); CHECK(card[128 + 127] == (0x10 ^ 0x11));
	/* 7. Card: edit then leave region and discard */
	cardWrites = 0; reset(2);
	press(TINROM_PAD_CROSS); rep(TINROM_PAD_UP); press(TINROM_PAD_CROSS);
	press(TINROM_PAD_CIRCLE); popupAnswers[npop++] = 1; /* discard */
	run();
	CHECK(cardWrites == 0); CHECK(card[0] == 0);
	/* 8. Card search across a frame boundary */
	memset(card, 0, sizeof(card)); memcpy(&card[5 * 128 + 126], "OK", 2);
	reset(2);
	press(TINROM_PAD_TRIANGLE); popupAnswers[npop++] = 1; popupAnswers[npop++] = 3;
	/* the last search ("HI") is the starting text: H->O is 7 up, I->K 2 up */ for (int k = 0; k < 7; k++) rep(TINROM_PAD_UP); rep(TINROM_PAD_RIGHT); rep(TINROM_PAD_UP); rep(TINROM_PAD_UP); press(TINROM_PAD_CROSS);
	press(TINROM_PAD_CROSS); rep(TINROM_PAD_UP); press(TINROM_PAD_CROSS); /* 'O' 4F -> 5F in frame 5 */
	press(TINROM_PAD_CIRCLE); popupAnswers[npop++] = 0; /* write on leave */
	run();
	CHECK(card[5 * 128 + 126] == 0x5f);
	/* 9. Kernel-area warning declined: no write */
	memset(fakeRam, 0, sizeof(fakeRam)); writes = 0; reset(0);
	press(TINROM_PAD_TRIANGLE); popupAnswers[npop++] = 6; /* start of region */
	press(TINROM_PAD_CROSS); rep(TINROM_PAD_UP); press(TINROM_PAD_CROSS);
	confirmAnswer = 0;
	run();
	CHECK(writes == 0); CHECK(confirms >= 1);
	printf("%s (%d failures)\n", fails ? "FAILED" : "all passed", fails);
	return fails != 0;
}
