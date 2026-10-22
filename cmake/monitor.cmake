# tinrom - (C) 2026 TinWhisker - MIT licensed
#
# Builds the resident debug monitor (src/monitor) as its own tiny executable,
# linked at TINROM_MONITOR_BASE, and embeds the raw image in tinrom as
# `monitorBlob`. tinrom copies it into place at boot (hal/debugger.c).
#
# It is a separate link on purpose: the monitor must keep working after a
# debugger has loaded a program over tinrom, so it may not call a single
# function in tinrom. Linking it alone turns any such call into an undefined
# symbol at build time instead of a crash on the console. See docs/debugger.md.
#
# Like the cart ROM stub it is compiled with custom commands rather than as a
# CMake target, so none of the flags every other target inherits (the -G8
# small-data model above all) leak into it.

cmake_minimum_required(VERSION 3.25)

function(tinromAddMonitorBlob target)
	set(outDir  "${PROJECT_BINARY_DIR}/monitor")
	set(elf     "${outDir}/monitor.elf")
	set(bin     "${outDir}/monitor.bin")
	set(linkerScript "${PROJECT_SOURCE_DIR}/src/monitor/monitor.ld")

	# One blob, however many payloads embed it.
	get_property(built GLOBAL PROPERTY TINROM_MONITOR_BLOB_RULES)

	if(built)
		tinromAddBinaryFile(${target} monitorBlob "${bin}")

		return()
	endif()

	set_property(GLOBAL PROPERTY TINROM_MONITOR_BLOB_RULES TRUE)

	set(sources
		"${PROJECT_SOURCE_DIR}/src/monitor/entry.S"
		"${PROJECT_SOURCE_DIR}/src/monitor/core.c"
		"${PROJECT_SOURCE_DIR}/src/monitor/sio.c"
		"${PROJECT_SOURCE_DIR}/src/monitor/util.c"
		"${PROJECT_SOURCE_DIR}/src/monitor/nops.c"
		"${PROJECT_SOURCE_DIR}/src/monitor/rsp.c"
		# ps1-bare-metal's instruction cache flush (MIT, spicyjpeg), linked in
		# a second time: tinrom's copy may not be there when it is needed.
		"${TINROM_BARE_METAL_DIR}/src/common/cache.s"
	)
	file(GLOB headers CONFIGURE_DEPENDS "${PROJECT_SOURCE_DIR}/src/monitor/*.h")

	add_custom_command(
		OUTPUT  "${elf}"
		DEPENDS ${sources} ${headers} "${linkerScript}"
			"${PROJECT_SOURCE_DIR}/src/hal/exception.h"
		COMMAND "${CMAKE_COMMAND}" -E make_directory "${outDir}"
		COMMAND
			"${CMAKE_C_COMPILER}"
			-march=r3000 -mabi=32 -mfp32 -mno-abicalls -fno-pic
			-mno-mt -mno-llsc
			# No gp-relative data: the handler runs with whatever gp the
			# stopped program had, and does not set its own.
			-G0 -mno-gpopt
			-mno-check-zero-division
			-std=gnu17 -Os -g -Wall -Wextra -Wno-unused-parameter
			-ffreestanding -fno-builtin -nostdlib -static
			# Or GCC may turn memset()'s own loop into a call to memset().
			-fno-tree-loop-distribute-patterns
			-fdata-sections -ffunction-sections -fsigned-char
			-Wa,--strip-local-absolute
			"-I${PROJECT_SOURCE_DIR}/src"
			"-I${TINROM_BARE_METAL_DIR}/src"
			${sources}
			"-Wl,-T,${linkerScript}"
			-Wl,--gc-sections
			"-Wl,-Map,${outDir}/monitor.map"
			-o "${elf}"
		COMMENT "Building the resident debug monitor"
		VERBATIM
	)
	add_custom_command(
		OUTPUT  "${bin}"
		DEPENDS "${elf}"
		COMMAND "${CMAKE_OBJCOPY}" -O binary
			-j .header -j .text -j .rodata -j .data
			"${elf}" "${bin}"
		COMMENT "Extracting the resident monitor image"
		VERBATIM
	)

	tinromAddBinaryFile(${target} monitorBlob "${bin}")
endfunction()
