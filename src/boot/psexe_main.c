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
	/*
	 * The cart stub (src/boot/rom_stub.S) enters with the boot source in $a0
	 * and 0 in $a1, which crt0 hands on as argc and argv. Anything else -
	 * a serial upload, the BIOS running tinrom's own disc, an emulator - is
	 * a PS-EXE that was loaded. tinromMain() checks a cart's claim against
	 * what is actually at EXP1, since an uploader may leave anything in $a0.
	 */
	TinromBootSource source = TINROM_BOOT_PSEXE;

	if (!argv && ((argc == TINROM_BOOT_CART) || (argc == TINROM_BOOT_PREBOOT)
		|| (argc == TINROM_BOOT_MIDBOOT)))
		source = (TinromBootSource) argc;

	tinromMain(source);

	return 0;
}
