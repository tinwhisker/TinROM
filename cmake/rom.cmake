# tinrom - (C) 2026 TinWhisker - MIT licensed
#
# Builds the flashable EXP1 cart image:
#
#   [ 100h header ][ descriptor ][ pre/post-boot stub ][ crash screen ]
#   [ tinrom.psexe ]
#
# The stub is hand-written position-independent assembly assembled straight to
# a raw binary - it is never linked, so it needs no linker script and can sit
# at whatever offset makerom.py puts it at. It copies the payload out of ROM
# into RAM and jumps to it. See docs/cart-rom.md. The crash screen, which
# games get when they crash with the cart in (TINROM_ROM_CRASH_SCREEN), is
# built by cmake/crash.cmake.

cmake_minimum_required(VERSION 3.25)

function(tinromAddRomImage target payloadTarget)
	set(stubObj "${PROJECT_BINARY_DIR}/rom/rom_stub.o")
	set(stubBin "${PROJECT_BINARY_DIR}/rom/rom_stub.bin")
	set(romOut  "${PROJECT_BINARY_DIR}/tinrom.rom")

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
	set(crashArg "")
	set(crashBin "")
	if(TINROM_ROM_CRASH_SCREEN)
		tinromAddCrashBlob(crashBin)
		set(crashArg --crash "${crashBin}")
	endif()

	add_custom_command(
		OUTPUT  "${romOut}"
		DEPENDS "${stubBin}" ${crashBin} ${payloadTarget}
			"${PROJECT_SOURCE_DIR}/tools/makerom.py"
		COMMAND
			"${Python3_EXECUTABLE}"
			"${PROJECT_SOURCE_DIR}/tools/makerom.py"
			--stub    "${stubBin}"
			--payload "${PROJECT_BINARY_DIR}/${payloadTarget}.psexe"
			--size    "${TINROM_ROM_SIZE}"
			--base    "${TINROM_ROM_BASE}"
			--version "${PROJECT_VERSION}"
			${licenceArg} ${prebootArg} ${postbootArg} ${ttyArg} ${crashArg}
			--output  "${romOut}"
		COMMENT "Building cart ROM image tinrom.rom"
		VERBATIM
	)

	add_custom_target(${target} DEPENDS "${romOut}")
endfunction()
