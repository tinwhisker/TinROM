# tinrom - (C) 2026 TinWhisker - MIT licensed
#
# FreePSXBoot-style memory card image. tools/makemcard.py is a STUB: it lays
# out a valid .mcr with a tinrom save block, but the exploit payload that makes
# the BIOS jump into it is not implemented. See docs/memcard-boot.md and the
# FreePSXBoot entry in NOTICE.md before filling it in.

cmake_minimum_required(VERSION 3.25)

function(tinromAddMemcardImage target payloadTarget)
	set(mcrOut "${PROJECT_BINARY_DIR}/tinrom.mcr")

	add_custom_command(
		OUTPUT  "${mcrOut}"
		DEPENDS ${payloadTarget} "${PROJECT_SOURCE_DIR}/tools/makemcard.py"
		COMMAND
			"${Python3_EXECUTABLE}"
			"${PROJECT_SOURCE_DIR}/tools/makemcard.py"
			--payload "${PROJECT_BINARY_DIR}/${payloadTarget}.psexe"
			--output  "${mcrOut}"
		COMMENT "Building memory card image tinrom.mcr (stub)"
		VERBATIM
	)
	add_custom_target(${target} DEPENDS "${mcrOut}")
endfunction()
