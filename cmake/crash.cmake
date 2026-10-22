# tinrom - (C) 2026 TinWhisker - MIT licensed
#
# Builds the cart's crash screen (src/crash) as its own tiny executable,
# linked to run straight out of the cart ROM, at TINROM_ROM_BASE seen
# uncached plus TINROM_ROM_CRASH_OFFSET. tools/makerom.py puts it there; it
# is not part of any payload. See src/crash/crash_api.h and
# docs/exceptions.md.
#
# Like the resident monitor it is a separate link, compiled with custom
# commands so that none of the flags every other target inherits (the -G8
# small-data model above all) leak into it - and so that a call into tinrom,
# which will have been overwritten by the time this runs, is an undefined
# symbol here rather than a hang on the console.

cmake_minimum_required(VERSION 3.25)

# tinromAddCrashBlob(<outVariable>)
#
# Sets <outVariable> to the raw image, to hand to makerom.py.
function(tinromAddCrashBlob outVariable)
	set(outDir "${PROJECT_BINARY_DIR}/crash")
	set(elf    "${outDir}/crash.elf")
	set(bin    "${outDir}/crash.bin")
	set(api    "${PROJECT_SOURCE_DIR}/src/crash/crash_api.h")
	set(linkerScript "${PROJECT_SOURCE_DIR}/src/crash/crash.ld")

	# The offset is written down once, in the header tinrom and makerom.py
	# read too.
	file(STRINGS "${api}" offsetLine
		REGEX "^#define TINROM_ROM_CRASH_OFFSET[ \t]+0x[0-9a-fA-F]+")
	string(REGEX REPLACE ".*(0x[0-9a-fA-F]+).*" "\\1" offset "${offsetLine}")

	if(NOT offset)
		message(FATAL_ERROR "No TINROM_ROM_CRASH_OFFSET in ${api}")
	endif()

	# Uncached: KSEG1 is the physical address with A0000000h set.
	math(EXPR base "(${TINROM_ROM_BASE} & 0x1fffffff) + ${offset} + 0xa0000000"
		OUTPUT_FORMAT HEXADECIMAL)

	set(sources
		"${PROJECT_SOURCE_DIR}/src/crash/entry.S"
		"${PROJECT_SOURCE_DIR}/src/crash/crash.c"
		"${PROJECT_BINARY_DIR}/generated/crashfont.c"
	)

	add_custom_command(
		OUTPUT  "${elf}"
		DEPENDS ${sources} "${linkerScript}" "${api}"
			"${PROJECT_SOURCE_DIR}/src/crash/hw.h"
			"${PROJECT_SOURCE_DIR}/src/hal/exception.h"
		COMMAND "${CMAKE_COMMAND}" -E make_directory "${outDir}"
		COMMAND
			"${CMAKE_C_COMPILER}"
			-march=r3000 -mabi=32 -mfp32 -mno-abicalls -fno-pic
			-mno-mt -mno-llsc
			# No gp-relative data: there is no data, and gp is the game's.
			-G0 -mno-gpopt
			-mno-check-zero-division
			-std=gnu17 -Os -g -Wall -Wextra -Wno-unused-parameter
			-ffreestanding -fno-builtin -nostdlib -static
			# Or GCC may turn a loop into a call to memset(), which is not here.
			-fno-tree-loop-distribute-patterns
			-fdata-sections -ffunction-sections -fsigned-char
			-Wa,--strip-local-absolute
			"-I${PROJECT_SOURCE_DIR}/src"
			"-I${TINROM_BARE_METAL_DIR}/src"
			${sources}
			"-Wl,--defsym=TINROM_CRASH_BASE=${base}"
			"-Wl,-T,${linkerScript}"
			-Wl,--gc-sections
			"-Wl,-Map,${outDir}/crash.map"
			-o "${elf}"
		COMMENT "Building the cart's crash screen"
		VERBATIM
	)
	add_custom_command(
		OUTPUT  "${bin}"
		DEPENDS "${elf}"
		COMMAND "${CMAKE_OBJCOPY}" -O binary
			-j .header -j .text -j .rodata
			"${elf}" "${bin}"
		COMMENT "Extracting the crash screen image"
		VERBATIM
	)

	set(${outVariable} "${bin}" PARENT_SCOPE)
endfunction()
