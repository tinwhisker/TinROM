# tinrom - (C) 2026 TinWhisker - MIT licensed
#
# Boot disc target. Needs mkpsxiso (https://github.com/Lameguy64/mkpsxiso),
# which the bootstrap script fetches into tools/bin/. If it is in neither
# there nor PATH, the target degrades to a message instead of failing the
# whole configure, so the psexe and rom targets still build.

cmake_minimum_required(VERSION 3.25)

# find_program() keeps what it found in the cache and never looks again, so
# a path that has since gone - tools/mkpsxiso.exe, from before the bootstrap
# script fetched it, or an uninstalled copy - would be used as is and fail
# at build time. Forget it and search afresh.
if(MKPSXISO_PATH AND NOT EXISTS "${MKPSXISO_PATH}")
	message(STATUS "mkpsxiso is no longer at ${MKPSXISO_PATH}; looking again")
	unset(MKPSXISO_PATH CACHE)
endif()

# Look in tools/bin/ before PATH: the bootstrap script fetches it there
# (tools/fetch_mkpsxiso.py). tinrom does not carry it - see NOTICE.md.
find_program(
	MKPSXISO_PATH
	NAMES mkpsxiso mkpsxiso.exe
	HINTS "${CMAKE_CURRENT_LIST_DIR}/../tools/bin"
	DOC   "Path to mkpsxiso"
)

function(tinromAddBootDisc target payloadTarget)
	if("${MKPSXISO_PATH}" STREQUAL "MKPSXISO_PATH-NOTFOUND")
		add_custom_target(
			${target}
			COMMAND "${CMAKE_COMMAND}" -E echo
				"mkpsxiso not found - skipping the boot disc. Run the bootstrap script, which fetches it, or put it on PATH; then re-run cmake."
			VERBATIM
		)
		set(TINROM_HAVE_MKPSXISO OFF CACHE INTERNAL "")
		return()
	endif()
	set(TINROM_HAVE_MKPSXISO ON CACHE INTERNAL "")

	# Absolute paths cannot go in the XML: an MSYS2 CMake writes /d/Source/...
	# and a native Windows mkpsxiso cannot open that, because the MSYS runtime
	# rewrites paths on a command line but never inside a file a tool reads.
	#
	# Relative paths are not enough on their own either. mkpsxiso resolves
	# source= against the XML FILE'S OWN DIRECTORY, not its working directory,
	# so a path that is correct relative to the build root silently loses a
	# level when the XML sits in a subdirectory of it.
	#
	# So: put the XML in the build root and stage every input it names beside
	# it. Then every path in it is a bare filename, which resolves the same way
	# under either convention and under either path flavour.
	configure_file(
		"${PROJECT_SOURCE_DIR}/iso/system.cnf"
		"${PROJECT_BINARY_DIR}/system.cnf"
		COPYONLY
	)
	# The disc's PSX.EXE: tinrom.psexe, or the disc's own payload when
	# TINROM_ISO_EXCLUDE_MODULES gives it one.
	set(TINROM_ISO_PAYLOAD "${payloadTarget}.psexe")

	configure_file(
		"${PROJECT_SOURCE_DIR}/iso/tinrom.xml"
		"${PROJECT_BINARY_DIR}/tinrom.xml"
		@ONLY
	)

	add_custom_command(
		OUTPUT  "${PROJECT_BINARY_DIR}/tinrom.bin" "${PROJECT_BINARY_DIR}/tinrom.cue"
		DEPENDS ${payloadTarget}
			"${PROJECT_BINARY_DIR}/tinrom.xml"
			"${PROJECT_BINARY_DIR}/system.cnf"
		COMMAND "${MKPSXISO_PATH}" -y -q
			-o tinrom.bin
			-c tinrom.cue
			tinrom.xml
		WORKING_DIRECTORY "${PROJECT_BINARY_DIR}"
		COMMENT "Building boot disc tinrom.bin/.cue"
		VERBATIM
	)
	add_custom_target(${target} DEPENDS "${PROJECT_BINARY_DIR}/tinrom.bin")
endfunction()
