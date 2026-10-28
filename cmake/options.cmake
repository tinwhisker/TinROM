# tinrom - (C) 2026 TinWhisker - MIT licensed
#
# Build-time configuration. Every feature module has a TINROM_MODULE_<ID>
# option; turning one off removes its code from the image entirely.
#
# The runtime "tier" (basic / advanced / developer) is a separate, orthogonal
# axis: it hides modules that ARE compiled in. See docs/modules.md.

cmake_minimum_required(VERSION 3.25)

# --- Settings that follow this file -----------------------------------------
#
# tinromCacheSetting(<name> <BOOL|STRING> <default> <description>
#                    [PREVIOUS <old default>])
#
# A plain option() or set(... CACHE) takes its default only when the build
# directory is new. After that the cache keeps what it first got, so editing
# a default below would change nothing until the cache was deleted. That is
# right for a value someone chose - with -D, a preset or the cache editor -
# and wrong for a default nobody chose to keep.
#
# So the default each setting was last configured with is remembered beside
# it, as an internal <name>_FILE_DEFAULT that cache editors do not show.
# While the setting still equals that default it follows this file, and says
# so when it moves; a value set any other way is left alone.
#
# PREVIOUS is only for a default that changed before tinrom remembered them:
# a build directory configured by an older tinrom has nothing remembered, so
# a value equal to the old default counts as never changed. It can go once
# no such build directory is left.
#
# Setting one with -D to exactly its current default looks the same as never
# setting it, and so follows this file afterwards.

function(tinromCacheSetting name type default description)
	set(remembered "${name}_FILE_DEFAULT")
	set(was "${default}")

	# By hand rather than with cmake_parse_arguments(), which before CMake
	# 3.31 cannot tell PREVIOUS "" from no PREVIOUS at all.
	if((ARGC EQUAL 6) AND ("${ARGV4}" STREQUAL "PREVIOUS"))
		set(was "${ARGV5}")
	elseif(NOT ARGC EQUAL 4)
		message(FATAL_ERROR "tinromCacheSetting(${name}): unexpected arguments")
	endif()

	if(DEFINED CACHE{${remembered}})
		set(was "$CACHE{${remembered}}")
	endif()

	if(DEFINED CACHE{${name}}
		AND (NOT "$CACHE{${name}}" STREQUAL "${default}")
		AND ("$CACHE{${name}}" STREQUAL "${was}"))
		set(${name} "${default}" CACHE ${type} "${description}" FORCE)
		message(STATUS "${name} follows cmake/options.cmake: now \"${default}\"")
	endif()

	set(${name} "${default}" CACHE ${type} "${description}")
	set(${remembered} "${default}" CACHE INTERNAL "")
endfunction()

# --- Runtime defaults -------------------------------------------------------

tinromCacheSetting(TINROM_DEFAULT_TIER STRING "advanced"
	"Tier selected on a fresh settings block")
set_property(CACHE TINROM_DEFAULT_TIER PROPERTY STRINGS basic advanced developer)

tinromCacheSetting(TINROM_DEFAULT_BAUD STRING "115200"
	"SIO1 baud rate used before any /fast negotiation")

tinromCacheSetting(TINROM_LOG_TO_SERIAL BOOL OFF
	"Mirror TINROM_LOG output to SIO1 from the very first line. Without this \
the mirror starts only when \"TTY to serial\" is switched on in the settings \
screen - and by then every boot-time line has already been written. Turning \
the toggle on at runtime now replays the ring buffer, so this option only \
matters for output you cannot get back afterwards: a hang or a crash during \
HAL bring-up, module init or the first frame. Costs nothing but boot time.")

# --- Cart ROM image ---------------------------------------------------------

tinromCacheSetting(TINROM_ROM_SIZE STRING "0x80000"
	"Cart ROM image size in bytes (0x20000=128K, 0x60000=384K, 0x80000=512K)")

tinromCacheSetting(TINROM_ROM_BASE STRING "0x1F000000"
	"Address EXP1 is mapped at")

tinromCacheSetting(TINROM_ROM_LICENCE_STRING BOOL ON
	"Emit the BIOS-checked ID string in the EXP1 ROM header. Without it the \
console's boot ROM will not call tinrom's entry points; see docs/cart-rom.md.")

tinromCacheSetting(TINROM_ROM_TTY_MESSAGE STRING " would be great, but it isn't."
	"Text that follows both ID strings in the cart header, so that it reads \
\"Licensed by Sony Computer Entertainment Inc. would be great, but it \
isn't.\" - a wish, not a claim. The BIOS prints the post-boot one over TTY; \
a retail BIOS checks only the ID itself. Up to 79 ASCII characters; mind the \
leading space. Empty leaves both fields zero. See docs/cart-rom.md.")

tinromCacheSetting(TINROM_ROM_MIDBOOT BOOL ON
	"Take over the console from a cart with no disc in the drive. The \
pre-boot handler arms a COP0 opcode breakpoint on the kernel's shell entry \
point and returns; the kernel initialises normally and then traps into tinrom \
before the CD-ROM is touched. Without this the cart only runs when a disc is \
present, because the post-boot entry point is the only other way in. See \
docs/cart-rom.md.")

tinromCacheSetting(TINROM_ROM_TRACE BOOL ON
	"Have the cart stub write progress bytes to the EXP2 POST register \
(1F802041h). Emulators log these - PCSX-Redux shows them as \"BIOS Trace1\" - \
and it is the only way to see whether the console called the ROM at all, \
since at that point tinrom owns no screen and no serial port. \
0xE0 post-boot entered, 0xE1 pre-boot entered, 0xE2 reading the descriptor, \
0xE3 payload copied and jumping, 0xEF nothing to load, returning.")

tinromCacheSetting(TINROM_ROM_COMPRESS BOOL ON
	"Pack the cart images' payload with tools/tinlz.py, which about halves \
it; the cart unpacks it into RAM as it boots, with a decoder of a few \
hundred bytes in the ROM (src/boot/unpack.c). Makes room in a small chip and \
boots faster, since the cart's bus is slow. Off stores it as it is, and \
saves the couple of seconds packing takes a build. See docs/cart-rom.md.")

tinromCacheSetting(TINROM_ROM_UNPACK_CACHED BOOL ON
	"Run the cart's payload decoder through the instruction cache, so it is \
fetched over the cart's slow 8-bit bus once rather than for every byte it \
unpacks: several times faster. Off runs it uncached, as the crash screen \
runs - for a console that will not run cart code through the cache.")

tinromCacheSetting(TINROM_ROM_CRASH_SCREEN BOOL ON
	"Put a crash screen in the cart for games. Before handing over the \
console, tinrom points the kernel's A(40h), which a crashed game would \
otherwise hang in, at code in the cart's ROM that shows what went wrong \
and the registers. Costs about 5K of the ROM, nothing in RAM, and nothing \
at all to a game that does not crash. See docs/exceptions.md.")

tinromCacheSetting(TINROM_ROM_PREBOOT BOOL ON
	"Install the pre-boot entry point (runs before the kernel is initialised)")

tinromCacheSetting(TINROM_ROM_POSTBOOT BOOL ON
	"Install the post-boot entry point (runs while the logo is on screen)")

# --- Feature modules --------------------------------------------------------
#
# tinromModuleOption(<id> <default> <description>)

set(TINROM_ALL_MODULES "" CACHE INTERNAL "")

# Every module defaulted ON before the defaults were remembered.
function(tinromModuleOption id default description)
	string(TOUPPER "${id}" upper)
	tinromCacheSetting(TINROM_MODULE_${upper} BOOL "${default}"
		"${description}" PREVIOUS ON)
	set(TINROM_ALL_MODULES "${TINROM_ALL_MODULES};${id}" CACHE INTERNAL "")
endfunction()

#                   id            default  description
tinromModuleOption(about        ON  "About / credits screen (always keep this: it carries attribution)")
tinromModuleOption(settingsui   ON  "Settings screen, including the tier selector")
tinromModuleOption(sysinfo      ON  "Console, BIOS, cart and region identification")
tinromModuleOption(cdboot       ON  "Boot a disc: region patching, swap and unlock handling")
tinromModuleOption(filebrowser  ON  "Browse a disc or memory card and launch EXEs")
tinromModuleOption(memcard      ON  "Memory card manager: format, copy, image, verify")
tinromModuleOption(cheats       ON  "Cheat engine: code entry, storage and application")
tinromModuleOption(siocons      ON  "nops-compatible serial server (upload, dump, poke, TTY)")
tinromModuleOption(gdb          ON  "GDB remote serial protocol stub (needs siocons)")
tinromModuleOption(flasher      ON  "Cart EEPROM identification and flashing")
tinromModuleOption(hexedit      ON  "Memory and memory-card hex editor")
tinromModuleOption(regionpatch  ON  "PAL/NTSC soft patching")
tinromModuleOption(pcdrv        ON  "PCDRV host filesystem redirection (needs siocons)")
tinromModuleOption(terminal     ON  "Serial terminal: ANSI BBSes through a WiFi modem on SIO1")
tinromModuleOption(irc          ON  "IRC client through the same WiFi modem")
tinromModuleOption(cdplay       ON  "Audio CD player: tracks, repeat, room effects and a visualiser")

# Modules that have been taken out. An exclude list that still names one
# builds anyway (cmake/modules.cmake drops it), and a build directory that
# still has its option has it removed, so nothing stale shows in the cache.
#
#   exception  the crash handler's menu. Its install and test moved to the
#              pcdrv module, and games now get the cart's crash screen
#              instead (TINROM_ROM_CRASH_SCREEN).
set(TINROM_RETIRED_MODULES exception)

foreach(id IN LISTS TINROM_RETIRED_MODULES)
	string(TOUPPER "${id}" upper)

	if(DEFINED CACHE{TINROM_MODULE_${upper}})
		unset(TINROM_MODULE_${upper} CACHE)
		unset(TINROM_MODULE_${upper}_FILE_DEFAULT CACHE)
		message(STATUS "TINROM_MODULE_${upper} is gone: there is no '${id}' module any more")
	endif()
endforeach()

# --- What goes into each image ---------------------------------------------
#
# Two pieces are big enough to matter and are not modules: the model spinning
# behind the menu (its mesh, texture and drawing code) and the resident debug
# monitor (the exception handler, PCDRV breaks and the GDB/nops debugger,
# see docs/debugger.md). Each can be left out of the main payload -
# tinrom.psexe, and so the cart ROM and the boot disc - and, separately, out
# of the memory card image.
#
# The TINROM_MODULE_* options above apply to every image. On top of them each
# image has a list of modules to leave out of it alone: by default flasher
# for the cart ROM, nothing for the boot disc, and for the memory card
# terminal, irc, siocons and flasher. An image whose list is not empty gets a
# payload of its own - tinrom_cart.psexe, tinrom_disc.psexe,
# tinrom_card.psexe - built from the same sources, which is a second build of
# most of tinrom.
#
# The memory card defaults are leaner because a card holds 15 blocks and not
# a byte more: see docs/memcard-boot.md. When they match the main payload's
# settings - TINROM_MCR_OPTIMIZE against TINROM_OPTIMIZE included - the card
# reuses tinrom.psexe; otherwise a second payload, tinrom_card.psexe, is
# built from the same sources.

tinromCacheSetting(TINROM_MODEL BOOL ON
	"Spinning background model in the main payload")
tinromCacheSetting(TINROM_DEBUGGER BOOL ON
	"Resident debug monitor (exception handler, PCDRV breaks, GDB and nops \
debugging) in the main payload")

tinromCacheSetting(TINROM_OPTIMIZE STRING "-Os"
	"Optimisation for the main payload's own code - tinrom.psexe, and so the \
cart ROM and the boot disc - after the build type's. -Os by default: a \
debug build with everything in is past the 256K of EXP1 that pcsx-redux \
maps, and smaller code is what a 2 MB console wants anyway. Empty compiles \
it with the build type's (-Og for Debug), which GDB follows more closely.")

tinromCacheSetting(TINROM_MCR_MODEL BOOL OFF
	"Spinning background model in the memory card image")
tinromCacheSetting(TINROM_MCR_DEBUGGER BOOL OFF
	"Resident debug monitor in the memory card image")
tinromCacheSetting(TINROM_MCR_OPTIMIZE STRING "-Os"
	"Optimisation for the memory card payload's own code, after the build \
type's: -Os by default, because the card's 15 blocks are the limit that \
matters there. Empty compiles it like everything else.")
tinromCacheSetting(TINROM_MCR_COMPRESS BOOL ON
	"Pack the memory card's payload with tools/tinlz.py, as the cart's is, \
into a PS-EXE that unpacks itself as it starts (tools/packexe.py), which \
about halves it. The card's 15 blocks are a hard ceiling, and tinrom no \
longer fits under it unpacked. See docs/memcard-boot.md.")

# The card's whole exclusion list, in one place. terminal and irc are on it
# by default because they need SIO1, which the PSone does not have, and do
# not fit alongside the rest; siocons and flasher to save room.
# Take ids off to put them back.
#
# This replaced a separate TINROM_MCR_COMMS switch. A build directory that
# still has one in its cache is carried over once, so it builds the same
# card it did: OFF puts terminal and irc on the list it had, ON leaves them
# off it, and the old switch is then dropped.
if(DEFINED CACHE{TINROM_MCR_COMMS})
	set(tinromCarried "${TINROM_MCR_EXCLUDE_MODULES}")

	if(NOT TINROM_MCR_COMMS)
		foreach(id terminal irc)
			if(NOT id IN_LIST tinromCarried)
				list(APPEND tinromCarried ${id})
			endif()
		endforeach()
	endif()

	set(TINROM_MCR_EXCLUDE_MODULES "${tinromCarried}" CACHE STRING "" FORCE)
	unset(TINROM_MCR_COMMS CACHE)
	message(STATUS "TINROM_MCR_COMMS is gone: the card now leaves out \"${tinromCarried}\" (TINROM_MCR_EXCLUDE_MODULES)")
endif()

tinromCacheSetting(TINROM_MCR_EXCLUDE_MODULES STRING
	"terminal;irc;siocons;flasher"
	"Modules to leave out of the memory card image only, as a ;-separated \
list of ids. By default terminal and irc, which need SIO1 and the PSone has \
none; siocons, which serves a host the card build does not talk to; and \
flasher. Add to it to save more room, take ids off to put them back. The \
TINROM_MODULE_* options still apply on top."
	PREVIOUS "terminal;irc")

tinromCacheSetting(TINROM_ROM_EXCLUDE_MODULES STRING "flasher"
	"Modules to leave out of the cart ROM image only, as a ;-separated list \
of ids. flasher by default, so a cart is never asked to rewrite the chip it \
booted from; a disc can still recover one. Empty: the cart carries \
tinrom.psexe as it is. Otherwise it gets tinrom_cart.psexe."
	PREVIOUS "")
tinromCacheSetting(TINROM_ISO_EXCLUDE_MODULES STRING ""
	"Modules to leave out of the boot disc only, as a ;-separated list of \
ids. Empty: the disc carries tinrom.psexe as it is. The same list as the \
cart ROM's shares the cart's payload; any other gets tinrom_disc.psexe.")

# --- The 128K cart image ----------------------------------------------------
#
# A second cart image beside tinrom.rom, for carts with a 128K chip - a
# 29F010, as many Datel carts (Action Replay, GameShark) and clones have.
# tinrom.rom does not fit one, so this image carries a leaner payload.
# Its settings default to the memory card's, which has to fit in 120K
# anyway; while they match, the two images share tinrom_card.psexe and
# nothing is built twice. Change them and it gets tinrom_small.psexe.
# See "The 128K image" in docs/cart-rom.md.

tinromCacheSetting(TINROM_ROM_SMALL BOOL ON
	"Also build a small cart image, tinrom-128k.rom, for carts with a 128K \
(29F010) chip. Its payload leaves out what TINROM_ROM_SMALL_EXCLUDE_MODULES \
lists. See docs/cart-rom.md.")
tinromCacheSetting(TINROM_ROM_SMALL_SIZE STRING "0x20000"
	"Size of the small cart image in bytes. 0x20000 (128K) for a 29F010; \
the file is named for it, tinrom-128k.rom.")
tinromCacheSetting(TINROM_ROM_SMALL_MODEL BOOL OFF
	"Spinning background model in the small cart image")
tinromCacheSetting(TINROM_ROM_SMALL_DEBUGGER BOOL OFF
	"Resident debug monitor in the small cart image")
tinromCacheSetting(TINROM_ROM_SMALL_OPTIMIZE STRING "-Os"
	"Optimisation for the small cart payload's own code, after the build \
type's: -Os by default, as for the memory card.")
tinromCacheSetting(TINROM_ROM_SMALL_EXCLUDE_MODULES STRING
	"terminal;irc;siocons;flasher"
	"Modules to leave out of the small cart image only, as a ;-separated \
list of ids. The memory card's list by default, which is known to fit: \
terminal, irc, siocons and flasher. Take ids off to put them back; makerom \
says if the image no longer fits.")

# --- Dependency checks ------------------------------------------------------

if(TINROM_MODULE_GDB AND NOT TINROM_MODULE_SIOCONS)
	message(FATAL_ERROR "TINROM_MODULE_GDB requires TINROM_MODULE_SIOCONS")
endif()
if(TINROM_MODULE_PCDRV AND NOT TINROM_MODULE_SIOCONS)
	message(FATAL_ERROR "TINROM_MODULE_PCDRV requires TINROM_MODULE_SIOCONS")
endif()
