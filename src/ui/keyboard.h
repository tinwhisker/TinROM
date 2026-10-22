/* tinrom - (C) 2026 TinWhisker - MIT licensed
 *
 * Typing with a pad: an on-screen keyboard, and a line editor built on it.
 *
 * The keyboard is a US layout in five rows. On the pad:
 *
 *   D-pad     move between keys          Cross     press the key
 *   Square    backspace                  Triangle  shift, for one key
 *   L1        space                      Start     Enter
 *
 * Shift and Ctrl on the keyboard itself also hold for one key; Ctrl turns a
 * letter (or one of @[\]^_) into its control code. What it produces is a key
 * code: a character, a C0 control (Enter is 0Dh, backspace 08h, Tab 09h,
 * Esc 1Bh) or one of the UI_KEY_* arrows. Circle is left to the caller,
 * which usually means "put the keyboard away".
 *
 * Anything else that produces key codes - a keyboard adapter, one day - can
 * feed the same consumers.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "ui/ui.h"

typedef struct TinromContext TinromContext;

enum {
	UI_KEY_NONE  = 0,
	UI_KEY_UP    = 0x100,
	UI_KEY_DOWN,
	UI_KEY_LEFT,
	UI_KEY_RIGHT,
	/* Ctrl-@: a NUL, which cannot be 0 since that is UI_KEY_NONE. */
	UI_KEY_NUL
};

typedef struct {
	uint8_t row, col;
	bool    shift, ctrl;
} UIKeyboard;

/* Height of the drawn keyboard, in pixels. */
#define UI_KEYBOARD_HEIGHT 69

#ifdef __cplusplus
extern "C" {
#endif

void uiKeyboardInit(UIKeyboard *kb);

/** One frame of pad input. Returns the key pressed, or UI_KEY_NONE. */
int  uiKeyboardUpdate(UIKeyboard *kb, uint16_t pressed, uint16_t repeated);

/**
 * Called with a value that depends on when each key was pressed, for a
 * caller that wants entropy (comms/vault.h). NULL for none.
 */
void uiKeyboardSetKeyHook(void (*hook)(uint32_t value));

/** Draws it with its top edge at `y`, across the screen. */
void uiKeyboardDraw(const UIKeyboard *kb, UIContext *ui, int y);

/**
 * Edits `buffer` (a C string of at most `size` - 1 characters) on a screen
 * of its own. Returns true if it was accepted with Enter, false if Circle
 * left it - in which case `buffer` is as it was.
 */
bool uiEditText(
	TinromContext *ctx, const char *title, const char *prompt,
	char *buffer, size_t size
);

/**
 * The same for a password: each character shows as a star. The editor's own
 * copy is wiped when it returns.
 */
bool uiEditSecret(
	TinromContext *ctx, const char *title, const char *prompt,
	char *buffer, size_t size
);

#ifdef __cplusplus
}
#endif
