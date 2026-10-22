# tinrom - (C) 2026 TinWhisker - MIT licensed
#
# Turns the TINROM_MODULE_* options into (a) a generated X-macro header that
# the runtime registry includes, and (b) a list of source files to compile.

cmake_minimum_required(VERSION 3.25)

# Per-module source lists. A module may add files here as it grows; the
# convention is src/modules/<id>/<id>.c plus whatever it needs.
set(TINROM_MODULE_SRC_about       src/modules/about/about.c)
set(TINROM_MODULE_SRC_settingsui  src/modules/settingsui/settingsui.c)
set(TINROM_MODULE_SRC_sysinfo     src/modules/sysinfo/sysinfo.c)
set(TINROM_MODULE_SRC_cdboot      src/modules/cdboot/cdboot.c)
set(TINROM_MODULE_SRC_filebrowser src/modules/filebrowser/filebrowser.c)
set(TINROM_MODULE_SRC_memcard     src/modules/memcard/memcard.c)
set(TINROM_MODULE_SRC_cheats      src/modules/cheats/cheats.c)
set(TINROM_MODULE_SRC_siocons     src/modules/siocons/siocons.c
                                  src/modules/siocons/nops_server.c)
set(TINROM_MODULE_SRC_gdb         src/modules/gdb/gdb.c)
set(TINROM_MODULE_SRC_flasher     src/modules/flasher/flasher.c)
set(TINROM_MODULE_SRC_hexedit     src/modules/hexedit/hexedit.c
                                  src/modules/hexedit/card.c
                                  src/modules/hexedit/dial.c
                                  src/modules/hexedit/find.c
                                  src/modules/hexedit/regions.c
                                  src/modules/hexedit/view.c)
set(TINROM_MODULE_SRC_regionpatch src/modules/regionpatch/regionpatch.c)
set(TINROM_MODULE_SRC_pcdrv       src/modules/pcdrv/pcdrv.c)
set(TINROM_MODULE_SRC_cdplay      src/modules/cdplay/cdplay.c
                                  src/modules/cdplay/visual.c
                                  src/modules/cdplay/scene.c)

# The modem, the ANSI screen, telnet and the screens for the modem's settings
# and saving, shared by the terminal and the IRC client; listed by each, and
# compiled once.
set(TINROM_COMMS_SOURCES
	src/comms/commsui.c
	src/comms/config.c
	src/comms/modem.c
	src/comms/telnet.c
	src/comms/term.c
	src/comms/termview.c
	"${PROJECT_BINARY_DIR}/generated/termfont.c")
set(TINROM_MODULE_SRC_terminal    src/modules/terminal/terminal.c
                                  src/modules/terminal/session.c
                                  ${TINROM_COMMS_SOURCES})
set(TINROM_MODULE_SRC_irc         src/modules/irc/irc.c
                                  src/modules/irc/client.c
                                  src/comms/crypto.c
                                  src/comms/irc.c
                                  src/comms/irctext.c
                                  src/comms/vault.c
                                  ${TINROM_COMMS_SOURCES})

# tinromCollectModules(<outSources> [TARGET <name>] [EXCLUDE <id>...])
#
# Returns the source list for the enabled modules in <outSources>, and the
# enabled ids in <outSources>_ENABLED. Without TARGET it writes the module
# table every target uses by default; with TARGET, a table of that target's
# own (in generated-<name>/), so one build can produce images with different
# module sets - the memory card image, which has a hard size ceiling, most of
# all. EXCLUDE drops modules on top of the TINROM_MODULE_* options.
#
# Dependencies are followed rather than failed on: excluding siocons also
# excludes gdb and pcdrv, which cannot work without it.
function(tinromCollectModules outSources)
	cmake_parse_arguments(arg "" "TARGET" "EXCLUDE" ${ARGN})

	set(exclude ${arg_EXCLUDE})

	# An id from a module that no longer exists is dropped rather than
	# failed on, so an exclude list written for an older tinrom still builds.
	foreach(id IN LISTS TINROM_RETIRED_MODULES)
		if(id IN_LIST exclude)
			list(REMOVE_ITEM exclude ${id})
			message(STATUS "Ignoring '${id}' in an exclude list: there is no \
such module any more")
		endif()
	endforeach()

	if("siocons" IN_LIST exclude)
		foreach(dependent gdb pcdrv)
			if(NOT dependent IN_LIST exclude)
				list(APPEND exclude ${dependent})
			endif()
		endforeach()
	endif()

	set(sources "")
	set(entries "")
	set(enabled "")

	foreach(id IN LISTS TINROM_ALL_MODULES)
		if("${id}" STREQUAL "")
			continue()
		endif()
		string(TOUPPER "${id}" upper)
		if(NOT TINROM_MODULE_${upper})
			continue()
		endif()
		if(id IN_LIST exclude)
			continue()
		endif()
		if(NOT DEFINED TINROM_MODULE_SRC_${id})
			message(FATAL_ERROR "Module '${id}' is enabled but has no \
TINROM_MODULE_SRC_${id} source list in cmake/modules.cmake")
		endif()
		list(APPEND sources ${TINROM_MODULE_SRC_${id}})
		list(APPEND enabled "${id}")
		string(APPEND entries "TINROM_MODULE_ENTRY(${id})\n")
	endforeach()

	foreach(id IN LISTS exclude)
		if(NOT id IN_LIST TINROM_ALL_MODULES)
			set(ids ${TINROM_ALL_MODULES})
			list(REMOVE_ITEM ids "")
			list(JOIN ids ", " ids)
			message(FATAL_ERROR "Cannot exclude '${id}': there is no such \
module. The ids are: ${ids}")
		endif()
	endforeach()

	if(arg_TARGET)
		set(tableDir "${PROJECT_BINARY_DIR}/generated-${arg_TARGET}")
	else()
		set(tableDir "${PROJECT_BINARY_DIR}/generated")
		set(TINROM_ENABLED_MODULES "${enabled}" CACHE INTERNAL "")
	endif()

	# The registry includes this file twice, once to declare the externs and
	# once to build the table. See src/core/module.c.
	file(
		CONFIGURE
		OUTPUT  "${tableDir}/tinrom/module_list.h"
		CONTENT "/* Generated by cmake/modules.cmake - do not edit. */\n${entries}"
		NEWLINE_STYLE LF
	)

	list(REMOVE_DUPLICATES sources)
	set(${outSources} "${sources}" PARENT_SCOPE)
	set(${outSources}_ENABLED "${enabled}" PARENT_SCOPE)
endfunction()

# Common include paths and defines for any tinrom executable.
#
# tinromConfigureTarget(<target> [TABLE <dir>] [MODULES <id>...])
#
# TABLE is a generated-<name> directory from tinromCollectModules(TARGET),
# searched before the shared one so its module table wins. MODULES is the
# enabled list to define TINROM_HAVE_<ID> for; the default build's otherwise.
function(tinromConfigureTarget target)
	cmake_parse_arguments(arg "" "TABLE" "MODULES" ${ARGN})

	if(arg_TABLE)
		target_include_directories(${target} BEFORE PRIVATE "${arg_TABLE}")
	endif()

	target_include_directories(
		${target} PRIVATE
		"${PROJECT_SOURCE_DIR}/src"
		"${PROJECT_BINARY_DIR}/generated"
	)
	string(TOUPPER "${TINROM_DEFAULT_TIER}" tierUpper)

	if(TINROM_LOG_TO_SERIAL)
		set(logSerialDefault 1)
	else()
		set(logSerialDefault 0)
	endif()

	target_compile_definitions(
		${target} PRIVATE
		TINROM_DEFAULT_TIER=TINROM_TIER_${tierUpper}
		TINROM_DEFAULT_BAUD=${TINROM_DEFAULT_BAUD}
		TINROM_LOG_SERIAL_DEFAULT=${logSerialDefault}
		TINROM_ROM_BASE=${TINROM_ROM_BASE}
		TINROM_ROM_SIZE=${TINROM_ROM_SIZE}
	)

	if(DEFINED arg_MODULES)
		set(modules ${arg_MODULES})
	else()
		set(modules ${TINROM_ENABLED_MODULES})
	endif()

	foreach(id IN LISTS modules)
		string(TOUPPER "${id}" upper)
		target_compile_definitions(${target} PRIVATE TINROM_HAVE_${upper}=1)
	endforeach()
endfunction()
