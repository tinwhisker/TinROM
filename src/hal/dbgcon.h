/* tinrom - (C) 2026 TinWhisker - MIT licensed
 *
 * Emulator debug console. PCSX-Redux exposes a handful of registers in the
 * top half of EXP2 (DEV8); the one that matters here is a putchar port whose
 * output lands in Redux's own log window, next to the BIOS TTY output, with
 * nothing to configure on the host side.
 *
 * On a real console these addresses read back as open bus, the probe fails,
 * and every call below becomes a no-op. See docs/architecture.md.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Widens EXP2, probes for the ID, and latches the result. Returns presence. */
bool tinromDbgConsoleInit(void);
bool tinromDbgConsolePresent(void);

/** Name of the detected host, or NULL. For the sysinfo screen. */
const char *tinromDbgConsoleName(void);

void tinromDbgConsoleWriteByte(uint8_t value);
void tinromDbgConsoleWriteString(const char *string);

/** Drops the emulator into its debugger. No-op on hardware. */
void tinromDbgConsoleBreak(void);

#ifdef __cplusplus
}
#endif
