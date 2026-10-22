# tinrom - (C) 2026 TinWhisker - MIT licensed
#
# Build entry point. tinrom does not ship its own toolchain or libc; it reuses
# ps1-bare-metal (extern/ps1-bare-metal, MIT, by spicyjpeg) for all of that.
# See NOTICE.md.

cmake_minimum_required(VERSION 3.25)

set(TINROM_BARE_METAL_DIR "${CMAKE_CURRENT_LIST_DIR}/extern/ps1-bare-metal"
	CACHE PATH "Path to the ps1-bare-metal checkout")

# ps1-bare-metal is a git submodule. The commit tinrom's tree records is the
# supported revision; the bootstrap script checks that out, or upstream's
# newest main on request. See extern/README.md.
if(NOT EXISTS "${TINROM_BARE_METAL_DIR}/cmake/toolchain.cmake")
	message(FATAL_ERROR
		"ps1-bare-metal is missing from ${TINROM_BARE_METAL_DIR}.\n"
		"Run the bootstrap script, which checks it out:\n"
		"      scripts/bootstrap.ps1     (Windows PowerShell)\n"
		"      ./scripts/bootstrap.sh    (Linux, macOS, MSYS2, Git Bash)\n"
		"or, for the supported revision alone:\n"
		"      git submodule update --init\n"
		"Or point CMake at a checkout you already have:\n"
		"      -DTINROM_BARE_METAL_DIR=/path/to/ps1-bare-metal\n")
endif()

# Which revision it is, for the summary. Built against another, it may well
# build - or fail in ways that look like tinrom's fault - so a checkout off
# the supported revision is said, not refused: upstream's latest, on
# purpose, is how an update gets tried.
find_package(Git QUIET)
set(tinromBareMetalRevision "")

if(GIT_FOUND AND EXISTS "${TINROM_BARE_METAL_DIR}/.git")
	execute_process(
		COMMAND "${GIT_EXECUTABLE}" -C "${CMAKE_CURRENT_LIST_DIR}"
			ls-files -s extern/ps1-bare-metal
		OUTPUT_VARIABLE tinromBareMetalEntry
		ERROR_QUIET
	)
	execute_process(
		COMMAND "${GIT_EXECUTABLE}" -C "${TINROM_BARE_METAL_DIR}" rev-parse HEAD
		OUTPUT_VARIABLE tinromBareMetalHead
		OUTPUT_STRIP_TRAILING_WHITESPACE
		ERROR_QUIET
	)

	if(tinromBareMetalEntry MATCHES "^160000 ([0-9a-f]+)"
		AND tinromBareMetalHead)
		set(tinromBareMetalSupported "${CMAKE_MATCH_1}")
		string(SUBSTRING "${tinromBareMetalHead}" 0 12 tinromBareMetalHeadShort)
		string(SUBSTRING "${tinromBareMetalSupported}" 0 12
			tinromBareMetalSupportedShort)

		if(tinromBareMetalHead STREQUAL tinromBareMetalSupported)
			set(tinromBareMetalRevision
				"${tinromBareMetalHeadShort}, the supported revision")
		else()
			set(tinromBareMetalRevision
				"${tinromBareMetalHeadShort}, not the supported ${tinromBareMetalSupportedShort}")
			message(STATUS
				"ps1-bare-metal is at ${tinromBareMetalHeadShort}, not the "
				"supported ${tinromBareMetalSupportedShort}. If that is upstream's "
				"latest on purpose, commit the submodule once tinrom builds; "
				"otherwise run the bootstrap script with --supported.")
		endif()
	endif()
endif()

# Must be set before project(). This pulls in the mipsel-none-elf toolchain.
set(CMAKE_TOOLCHAIN_FILE "${TINROM_BARE_METAL_DIR}/cmake/toolchain.cmake")

project(
	tinrom
	LANGUAGES    C ASM
	VERSION      0.1.0
	DESCRIPTION  "A modular, replacement utility ROM for the PlayStation 1."
	HOMEPAGE_URL "https://github.com/TinWhisker/ps1-tinrom"
)

# Compiler/linker flags and the addPS1Executable()/convertImage() helpers come
# from ps1-bare-metal unmodified.
include("${TINROM_BARE_METAL_DIR}/cmake/setup.cmake")
include("${TINROM_BARE_METAL_DIR}/cmake/tools.cmake")

include(cmake/assets.cmake)
include(cmake/options.cmake)
include(cmake/modules.cmake)
include(cmake/rom.cmake)
include(cmake/iso.cmake)
include(cmake/memcard.cmake)
include(cmake/monitor.cmake)
include(cmake/crash.cmake)
include(cmake/payload.cmake)

# ---------------------------------------------------------------------------
# ps1-bare-metal support library
#
# ps1-bare-metal has no install target; its sources are compiled straight into
# an object library here. This mirrors what its own CMakeLists.txt does.
# ---------------------------------------------------------------------------

add_library(
	bare_metal OBJECT
	"${TINROM_BARE_METAL_DIR}/src/libc/clz.s"
	"${TINROM_BARE_METAL_DIR}/src/libc/crt0.c"
	# malloc.c references sbrk(), which ps1-bare-metal does not define - an
	# application that uses the heap has to supply it. tinrom does not, so
	# -gc-sections drops the whole thing. Calling malloc() means writing an
	# sbrk() first.
	"${TINROM_BARE_METAL_DIR}/src/libc/malloc.c"
	"${TINROM_BARE_METAL_DIR}/src/libc/misc.c"
	"${TINROM_BARE_METAL_DIR}/src/libc/setjmp.s"
	"${TINROM_BARE_METAL_DIR}/src/libc/string.c"
	"${TINROM_BARE_METAL_DIR}/src/libc/string.s"
	"${TINROM_BARE_METAL_DIR}/src/common/cache.s"
	"${TINROM_BARE_METAL_DIR}/src/vendor/printf.c"
	"${TINROM_BARE_METAL_DIR}/src/common/gpu.c"
	# Needed by the background model: the GTE does the transform, cull and
	# depth sort, and trig.c drives the rotation.
	"${TINROM_BARE_METAL_DIR}/src/common/gte.c"
	"${TINROM_BARE_METAL_DIR}/src/common/trig.c"
)
target_include_directories(
	bare_metal PUBLIC
	"${TINROM_BARE_METAL_DIR}/src"
	"${TINROM_BARE_METAL_DIR}/src/libc"
)
link_libraries(bare_metal)

# ---------------------------------------------------------------------------
# Generated headers
# ---------------------------------------------------------------------------

configure_file(
	"${PROJECT_SOURCE_DIR}/src/core/version.h.in"
	"${PROJECT_BINARY_DIR}/generated/tinrom/version.h"
	@ONLY
)

# The build stamp is regenerated on every build, not at configure time, so it
# identifies the image you are holding rather than the last time you ran cmake.
# Listing an OUTPUT that is never created is what keeps the command out of
# date; the script itself only rewrites the file when the value has changed, so
# two builds in the same minute do not relink.
set(TINROM_BUILD_STAMP "" CACHE STRING
	"Pin the build stamp to YYMMDDHHMM instead of reading the clock")

set(tinromBuildSource "${PROJECT_BINARY_DIR}/generated/tinrom_build.c")

add_custom_command(
	OUTPUT
		"${tinromBuildSource}"
		"${PROJECT_BINARY_DIR}/generated/.stamp-always"
	COMMAND
		"${CMAKE_COMMAND}"
		"-DOUTPUT=${tinromBuildSource}"
		"-DVERSION=${PROJECT_VERSION}"
		"-DSTAMP=${TINROM_BUILD_STAMP}"
		-P "${PROJECT_SOURCE_DIR}/cmake/buildstamp.cmake"
	DEPENDS "${PROJECT_SOURCE_DIR}/cmake/buildstamp.cmake"
	COMMENT "Stamping the build"
	VERBATIM
)

# ---------------------------------------------------------------------------
# Assets
# ---------------------------------------------------------------------------

# The UI font: Superpowers' outlined font, CC0 - see src/ui/fonts/README.md.
# It is drawn at 20x20. The screen is 640 wide, two pixels to a layout unit,
# so the font keeps all 20 columns and is scaled down only to the 10 rows
# the UI is laid out for. Its outline is two of those columns, which
# neighbours share (--tracking -2); --space is 4 units. A different size
# means new sizes in src/ui/font.h, which the build will name. Its backtick
# cell is empty, so ` borrows '.
tinromConvertFont(src/ui/fonts/font-20x20.png generated/uifont.c
	--cell 20x20 --size 10 --width 20 --space 8 --tracking -2 --alias 96=39)

# The terminal's 4x8 CP437 font, tinrom's own: see the file's header. Only
# compiled when the terminal or IRC client is (cmake/modules.cmake).
tinromConvertBitFont(src/ui/fonts/term4x8.txt generated/termfont.c)
# Its printable ASCII again, for the cart's crash screen (cmake/crash.cmake).
tinromConvertBitFont(src/ui/fonts/term4x8.txt generated/crashfont.c
	--name crashFont --first 0x20 --last 0x7e)

# The model idling behind the menu. 8bpp rather than 4: the texture has 17
# distinct colours, one more than a 4bpp CLUT holds.
tinromConvertImage(
	src/ui/objects/rom.png 8
	assets/romTexture.dat assets/romPalette.dat
)
# No face splitting: this mesh is fifteen separate convex boxes, and it is
# sorted per piece rather than per face, which subdividing cannot help and did
# measurably hurt. See docs/model-sorting.md. The threshold argument stays
# available for a mesh that is one connected concave shell.
tinromConvertMesh(src/ui/objects/rom.obj src/ui/objects/rom.png assets/rom.msh)

# ---------------------------------------------------------------------------
# Core / HAL / UI: everything that is not a feature module
# ---------------------------------------------------------------------------

set(TINROM_CORE_SOURCES
	# Generated every build by cmake/buildstamp.cmake; see docs/versioning.md.
	"${tinromBuildSource}"
	# The UI font's tables, made from src/ui/fonts/font-20x20.png; see above.
	"${PROJECT_BINARY_DIR}/generated/uifont.c"
	src/core/app.c
	src/core/iso9660.c
	src/core/log.c
	src/core/module.c
	src/core/pcdrv.c
	src/core/profile.c
	src/core/psexe.c
	src/core/result.c
	src/core/savefile.c
	src/core/settings.c
	src/hal/cdrom.c
	src/hal/crash.c
	src/hal/dbgcon.c
	src/hal/debugger.c
	src/hal/exception.c
	src/hal/exp.c
	src/hal/hal.c
	src/hal/handoff.S
	src/hal/handoff.c
	src/hal/mcard.c
	src/hal/pad.c
	src/hal/sio0.c
	src/hal/spu.c
	src/hal/uart_sio.c
	src/hal/timer.c
	src/hal/video.c
	src/ui/canvas.c
	src/ui/font.c
	src/ui/grid.c
	src/ui/keyboard.c
	src/ui/launch.c
	src/ui/menu.c
	src/ui/model.c
	src/ui/ui.c
	src/ui/widgets.c
)

# Wildcard the source.
# (Modules are different: one that is switched off is legitimately unlisted,
# and tinromCollectModules() checks those separately.)
file(GLOB tinromFoundSources
	RELATIVE "${PROJECT_SOURCE_DIR}"
	CONFIGURE_DEPENDS
	"${PROJECT_SOURCE_DIR}/src/core/*.c"
	"${PROJECT_SOURCE_DIR}/src/hal/*.c"
	"${PROJECT_SOURCE_DIR}/src/ui/*.c"
)
foreach(source IN LISTS tinromFoundSources)
	if(NOT source IN_LIST TINROM_CORE_SOURCES)
		list(APPEND tinromUnlisted "${source}")
	endif()
endforeach()
if(DEFINED tinromUnlisted)
	message(FATAL_ERROR
		"These source files exist but are not in TINROM_CORE_SOURCES, so they \
would not be compiled and you would get an undefined reference at link \
time:\n      ${tinromUnlisted}\n"
		"Add them to the list in CMakeLists.txt, or delete them.\n")
endif()

# ---------------------------------------------------------------------------
# Target 1: tinrom.psexe — the RAM-resident payload.
#
# This is both the thing you upload over serial to test, and the payload the
# cart ROM image and the boot disc carry. The TINROM_MODULE_* options and
# TINROM_MODEL / TINROM_DEBUGGER decide what goes in; see cmake/options.cmake.
# ---------------------------------------------------------------------------

tinromAddPayload(tinrom MODEL ${TINROM_MODEL} DEBUGGER ${TINROM_DEBUGGER}
	OPTIMIZE ${TINROM_OPTIMIZE})

# The cart ROM and the boot disc carry the main payload, unless their own
# exclusion lists leave something out - then each gets a payload of its own,
# the same in every other respect. A disc whose list matches the cart's
# shares the cart's. See "What goes into each image" in cmake/options.cmake.
set(tinromRomPayload tinrom)
set(tinromIsoPayload tinrom)

if(TINROM_ROM_EXCLUDE_MODULES)
	set(tinromRomPayload tinrom_cart)
	tinromAddPayload(tinrom_cart MODEL ${TINROM_MODEL} DEBUGGER ${TINROM_DEBUGGER}
		OPTIMIZE ${TINROM_OPTIMIZE}
		EXCLUDE_MODULES ${TINROM_ROM_EXCLUDE_MODULES})
endif()

if(TINROM_ISO_EXCLUDE_MODULES)
	set(tinromRomSorted ${TINROM_ROM_EXCLUDE_MODULES})
	set(tinromIsoSorted ${TINROM_ISO_EXCLUDE_MODULES})
	list(SORT tinromRomSorted)
	list(SORT tinromIsoSorted)

	if("${tinromIsoSorted}" STREQUAL "${tinromRomSorted}")
		set(tinromIsoPayload tinrom_cart)
	else()
		set(tinromIsoPayload tinrom_disc)
		tinromAddPayload(tinrom_disc MODEL ${TINROM_MODEL}
			DEBUGGER ${TINROM_DEBUGGER} OPTIMIZE ${TINROM_OPTIMIZE}
			EXCLUDE_MODULES ${TINROM_ISO_EXCLUDE_MODULES})
	endif()
endif()

# The memory card image has a hard 15-block ceiling, so it has settings of its
# own and, when they differ, a payload of its own. See docs/memcard-boot.md.
set(tinromCardDiffers FALSE)

# Compared by truthiness, not spelling: ON, TRUE and 1 are all the same.
foreach(what MODEL DEBUGGER)
	if((TINROM_MCR_${what} AND NOT TINROM_${what})
		OR (TINROM_${what} AND NOT TINROM_MCR_${what}))
		set(tinromCardDiffers TRUE)
	endif()
endforeach()

set(tinromCardExclude ${TINROM_MCR_EXCLUDE_MODULES})

if(tinromCardExclude
	OR NOT "${TINROM_MCR_OPTIMIZE}" STREQUAL "${TINROM_OPTIMIZE}")
	set(tinromCardDiffers TRUE)
endif()

if(NOT tinromCardDiffers)
	set(tinromCardPayload tinrom)
else()
	set(tinromCardPayload tinrom_card)
	tinromAddPayload(tinrom_card
		MODEL ${TINROM_MCR_MODEL} DEBUGGER ${TINROM_MCR_DEBUGGER}
		OPTIMIZE ${TINROM_MCR_OPTIMIZE}
		EXCLUDE_MODULES ${tinromCardExclude})
endif()

# ---------------------------------------------------------------------------
# Target 2: tinrom.rom — the flashable cart image.
# Target 3: tinrom.iso/.cue — the boot disc.
# Target 4: tinrom.mcr — the FreePSXBoot memory card image.
# ---------------------------------------------------------------------------

tinromAddRomImage(tinrom_rom ${tinromRomPayload})
tinromAddBootDisc(tinrom_iso ${tinromIsoPayload})
tinromAddMemcardImage(tinrom_mcr ${tinromCardPayload})

add_custom_target(
	tinrom_all ALL
	DEPENDS tinrom tinrom_rom tinrom_iso tinrom_mcr
)

include(cmake/summary.cmake)
