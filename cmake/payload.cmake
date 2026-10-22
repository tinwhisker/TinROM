# tinrom - (C) 2026 TinWhisker - MIT licensed
#
# tinromAddPayload(<target>
#     MODEL <ON|OFF> DEBUGGER <ON|OFF> [OPTIMIZE <flags>...]
#     [EXCLUDE_MODULES <id>...])
#
# One RAM-resident tinrom executable, <target>.psexe. The main payload is
# `tinrom`; the memory card image gets a leaner one of its own when its
# settings differ (see "What goes into each image" in cmake/options.cmake).
#
#   MODEL     embed the menu's background model. Without it the menu has a
#             plain background, and the model code is dropped by the linker.
#   DEBUGGER  embed the resident monitor. Without it there is no exception
#             handler, no PCDRV break servicing and no debugger, so the gdb
#             module - which is only a view of it - goes too.
#   OPTIMIZE  compiler flags for this payload's own sources that win over
#             the build type's: TINROM_OPTIMIZE for the main payload,
#             TINROM_MCR_OPTIMIZE for the memory card's. Both -Os by default.

cmake_minimum_required(VERSION 3.25)

function(tinromAddPayload target)
	cmake_parse_arguments(arg "" "MODEL;DEBUGGER" "OPTIMIZE;EXCLUDE_MODULES"
		${ARGN})

	set(exclude ${arg_EXCLUDE_MODULES})

	if(NOT arg_DEBUGGER)
		list(APPEND exclude gdb)
		list(REMOVE_DUPLICATES exclude)
	endif()

	if(target STREQUAL "tinrom")
		tinromCollectModules(modules EXCLUDE ${exclude})
		set(table "")
	else()
		tinromCollectModules(modules TARGET ${target} EXCLUDE ${exclude})
		set(table TABLE "${PROJECT_BINARY_DIR}/generated-${target}")
	endif()

	addPS1Executable(${target} src/boot/psexe_main.c
		${TINROM_CORE_SOURCES} ${modules})
	tinromConfigureTarget(${target} ${table} MODULES ${modules_ENABLED})

	# ps1-bare-metal puts the build type's -O on its `flags` interface
	# target, which every executable links. A target's own compile options
	# come before its dependencies', so an -Os given to the target would lose
	# to that -O. So the per-target flags go on `flags` itself, once, as a
	# generator expression that reads the target being compiled:
	# TINROM_OPTIMIZE on that target, and nothing on any other.
	get_property(hooked GLOBAL PROPERTY TINROM_OPTIMIZE_HOOKED)

	if(NOT hooked)
		set_property(GLOBAL PROPERTY TINROM_OPTIMIZE_HOOKED TRUE)
		target_compile_options(flags INTERFACE
			"$<TARGET_PROPERTY:TINROM_OPTIMIZE>")
	endif()

	if(arg_OPTIMIZE)
		set_property(TARGET ${target} PROPERTY TINROM_OPTIMIZE ${arg_OPTIMIZE})
	endif()

	if(arg_MODEL)
		tinromAddBinaryFile(${target} romMesh    "${PROJECT_BINARY_DIR}/assets/rom.msh")
		tinromAddBinaryFile(${target} romTexture "${PROJECT_BINARY_DIR}/assets/romTexture.dat")
		tinromAddBinaryFile(${target} romPalette "${PROJECT_BINARY_DIR}/assets/romPalette.dat")
		target_compile_definitions(${target} PRIVATE TINROM_WITH_MODEL=1)
	else()
		target_compile_definitions(${target} PRIVATE TINROM_WITH_MODEL=0)
	endif()

	if(arg_DEBUGGER)
		# Linked on its own, embedded, copied into place at boot. See
		# docs/debugger.md.
		tinromAddMonitorBlob(${target})
		target_compile_definitions(${target} PRIVATE TINROM_WITH_DEBUGGER=1)
	else()
		target_compile_definitions(${target} PRIVATE TINROM_WITH_DEBUGGER=0)
	endif()

	set(${target}_MODULES "${modules_ENABLED}" PARENT_SCOPE)
endfunction()
