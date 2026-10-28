# tinrom - (C) 2026 TinWhisker - MIT licensed
#
# Builds the flashable EXP1 cart image:
#
#   [ 100h header ][ descriptor ][ pre/post-boot stub ][ decoder ]
#   [ crash screen ][ tinrom.psexe, packed ]
#
# The stub is hand-written position-independent assembly assembled straight to
# a raw binary - it is never linked, so it needs no linker script and can sit
# at whatever offset makerom.py puts it at. It copies the payload out of ROM
# into RAM and jumps to it. See docs/cart-rom.md. The crash screen, which
# games get when they crash with the cart in (TINROM_ROM_CRASH_SCREEN), is
# built by cmake/crash.cmake.

cmake_minimum_required(VERSION 3.25)

# tinromAddRomStub(<stubVariable> <crashVariable> <unpackVariable>)
#
# The loader stub, the crash screen and the payload decoder are the same in
# every image, so they are built once; this sets the three variables to the
# raw images makerom.py takes - the crash screen's and the decoder's empty
# when TINROM_ROM_CRASH_SCREEN or TINROM_ROM_COMPRESS is off.
function(tinromAddRomStub stubVariable crashVariable unpackVariable)
	get_property(made GLOBAL PROPERTY TINROM_ROM_STUB_BIN SET)

	if(made)
		foreach(what STUB CRASH UNPACK)
			get_property(file GLOBAL PROPERTY TINROM_ROM_${what}_BIN)
			string(TOLOWER "${what}" lower)
			set(${${lower}Variable} "${file}" PARENT_SCOPE)
		endforeach()
		return()
	endif()

	set(stubObj "${PROJECT_BINARY_DIR}/rom/rom_stub.o")
	set(stubBin "${PROJECT_BINARY_DIR}/rom/rom_stub.bin")
	set(stubSrc "${PROJECT_SOURCE_DIR}/src/boot/rom_stub.S")

	set(stubTraceFlag "")
	if(TINROM_ROM_TRACE)
		set(stubTraceFlag "-DTINROM_ROM_TRACE")
	endif()
	set(stubMidBootFlag "")
	if(TINROM_ROM_MIDBOOT)
		set(stubMidBootFlag "-DTINROM_ROM_MIDBOOT")
	endif()

	add_custom_command(
		OUTPUT  "${stubObj}"
		DEPENDS "${stubSrc}"
		COMMAND "${CMAKE_COMMAND}" -E make_directory "${PROJECT_BINARY_DIR}/rom"
		COMMAND
			"${CMAKE_C_COMPILER}"
			-march=r3000 -mabi=32 -mfp32 -mno-abicalls -fno-pic
			-DTINROM_ROM_BASE=${TINROM_ROM_BASE}
			${stubTraceFlag} ${stubMidBootFlag}
			-c "${stubSrc}" -o "${stubObj}"
		COMMENT "Assembling cart ROM stub"
		VERBATIM
	)
	add_custom_command(
		OUTPUT  "${stubBin}"
		DEPENDS "${stubObj}"
		COMMAND "${CMAKE_OBJCOPY}" -O binary -j .text "${stubObj}" "${stubBin}"
		COMMENT "Extracting cart ROM stub binary"
		VERBATIM
	)

	set(crashBin "")
	if(TINROM_ROM_CRASH_SCREEN)
		tinromAddCrashBlob(crashBin)
	endif()

	# The payload decoder, src/boot/unpack.c: compiled on its own and never
	# linked - its .text is cut out and called where it lands in the ROM - so
	# it must have no relocations, no data and no second function, which
	# tools/checkblob.py makes sure of. Position-independent code keeps the
	# compiler's jumps relative; the rest keeps it from inventing calls
	# (memcpy for a copy loop) or tables.
	set(unpackBin "")
	if(TINROM_ROM_COMPRESS)
		set(unpackSrc "${PROJECT_SOURCE_DIR}/src/boot/unpack.c")
		set(unpackObj "${PROJECT_BINARY_DIR}/rom/unpack.o")
		set(unpackBin "${PROJECT_BINARY_DIR}/rom/unpack.bin")

		add_custom_command(
			OUTPUT  "${unpackBin}"
			DEPENDS "${unpackSrc}" "${PROJECT_SOURCE_DIR}/src/boot/unpack.h"
				"${PROJECT_SOURCE_DIR}/tools/checkblob.py"
			COMMAND "${CMAKE_COMMAND}" -E make_directory "${PROJECT_BINARY_DIR}/rom"
			COMMAND
				"${CMAKE_C_COMPILER}"
				-march=r3000 -mabi=32 -mfp32 -fpic -mabicalls
				-mno-mt -mno-llsc -G0 -mno-gpopt
				-std=gnu17 -Os -Wall -Wextra
				-ffreestanding -fno-builtin -nostdlib -fno-stack-protector
				-fno-tree-loop-distribute-patterns -fno-jump-tables
				"-I${PROJECT_SOURCE_DIR}/src"
				-c "${unpackSrc}" -o "${unpackObj}"
			COMMAND
				"${Python3_EXECUTABLE}"
				"${PROJECT_SOURCE_DIR}/tools/checkblob.py"
				"${unpackObj}" tinromUnpack
			COMMAND "${CMAKE_OBJCOPY}" -O binary -j .text "${unpackObj}" "${unpackBin}"
			COMMENT "Building the cart's payload decoder"
			VERBATIM
		)
	endif()

	set_property(GLOBAL PROPERTY TINROM_ROM_STUB_BIN   "${stubBin}")
	set_property(GLOBAL PROPERTY TINROM_ROM_CRASH_BIN  "${crashBin}")
	set_property(GLOBAL PROPERTY TINROM_ROM_UNPACK_BIN "${unpackBin}")
	set(${stubVariable}   "${stubBin}"   PARENT_SCOPE)
	set(${crashVariable}  "${crashBin}"  PARENT_SCOPE)
	set(${unpackVariable} "${unpackBin}" PARENT_SCOPE)
endfunction()

# tinromAddRomImage(<target> <payloadTarget> OUTPUT <file> SIZE <bytes>
#     [SIZE_OPTION <setting>] [EXCLUDE_OPTION <setting>])
#
# One cart image, <file> in the build directory, carrying <payloadTarget>
# and padded to SIZE bytes. SIZE_OPTION and EXCLUDE_OPTION name the settings
# that decide its size and what its payload leaves out, for makerom.py to
# point at when the payload does not fit.
function(tinromAddRomImage target payloadTarget)
	cmake_parse_arguments(arg "" "OUTPUT;SIZE;SIZE_OPTION;EXCLUDE_OPTION" ""
		${ARGN})

	if(NOT arg_OUTPUT OR NOT arg_SIZE)
		message(FATAL_ERROR "tinromAddRomImage(${target}): OUTPUT and SIZE are required")
	endif()
	if(NOT arg_SIZE_OPTION)
		set(arg_SIZE_OPTION TINROM_ROM_SIZE)
	endif()
	if(NOT arg_EXCLUDE_OPTION)
		set(arg_EXCLUDE_OPTION TINROM_ROM_EXCLUDE_MODULES)
	endif()

	set(romOut "${PROJECT_BINARY_DIR}/${arg_OUTPUT}")

	tinromAddRomStub(stubBin crashBin unpackBin)

	set(crashArg "")
	if(crashBin)
		set(crashArg --crash "${crashBin}")
	endif()
	set(unpackArg "")
	if(unpackBin)
		set(unpackArg --unpacker "${unpackBin}")
		if(NOT TINROM_ROM_UNPACK_CACHED)
			list(APPEND unpackArg --unpack-uncached)
		endif()
	endif()

	set(licenceArg "")
	if(NOT TINROM_ROM_LICENCE_STRING)
		set(licenceArg "--no-licence-string")
	endif()
	set(prebootArg "")
	if(NOT TINROM_ROM_PREBOOT)
		set(prebootArg "--no-preboot")
	endif()
	set(postbootArg "")
	if(NOT TINROM_ROM_POSTBOOT)
		set(postbootArg "--no-postboot")
	endif()
	set(ttyArg "")
	if(NOT "${TINROM_ROM_TTY_MESSAGE}" STREQUAL "")
		set(ttyArg --tty-message "${TINROM_ROM_TTY_MESSAGE}")
	endif()

	add_custom_command(
		OUTPUT  "${romOut}"
		DEPENDS "${stubBin}" ${crashBin} ${unpackBin} ${payloadTarget}
			"${PROJECT_SOURCE_DIR}/tools/makerom.py"
			"${PROJECT_SOURCE_DIR}/tools/tinlz.py"
		COMMAND
			"${Python3_EXECUTABLE}"
			"${PROJECT_SOURCE_DIR}/tools/makerom.py"
			--stub    "${stubBin}"
			--payload "${PROJECT_BINARY_DIR}/${payloadTarget}.psexe"
			--size    "${arg_SIZE}"
			--size-option "${arg_SIZE_OPTION}"
			--exclude-option "${arg_EXCLUDE_OPTION}"
			--base    "${TINROM_ROM_BASE}"
			--version "${PROJECT_VERSION}"
			${licenceArg} ${prebootArg} ${postbootArg} ${ttyArg} ${crashArg}
			${unpackArg}
			--output  "${romOut}"
		COMMENT "Building cart ROM image ${arg_OUTPUT}"
		VERBATIM
	)

	add_custom_target(${target} DEPENDS "${romOut}")
endfunction()
