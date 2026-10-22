/* tinrom - (C) 2026 TinWhisker - MIT licensed
 *
 * PS-EXE entry point. This is the build you upload over serial or run in an
 * emulator, and it is byte-for-byte the same payload the cart ROM image
 * carries - see cmake/rom.cmake.
 *
 * ps1-bare-metal's crt0 has already cleared .bss, set up $gp and the stack,
 * and run the init_array by the time main() is called.
 */

#include "core/app.h"

int main(int argc, const char **argv) {
	(void) argc;
	(void) argv;

	/*
	 * STUB: the cart stub passes a boot-source marker in $a0, which crt0
	 * forwards as argc. Once that is agreed on, detect it here instead of
	 * hardcoding TINROM_BOOT_PSEXE. See src/boot/rom_stub.S.
	 */
	tinromMain(TINROM_BOOT_PSEXE);

	return 0;
}
