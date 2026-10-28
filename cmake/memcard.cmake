# tinrom - (C) 2026 TinWhisker - MIT licensed
#
# FreePSXBoot-style memory card image. tools/makemcard.py is a STUB: it lays
# out a valid .mcr with a tinrom save block, but the exploit payload that makes
# the BIOS jump into it is not implemented. See docs/memcard-boot.md and the
# FreePSXBoot entry in NOTICE.md before filling it in.
#
# With TINROM_MCR_COMPRESS the card carries the payload packed, as a PS-EXE
# that unpacks itself (tools/packexe.py, src/boot/exe_stub.S), which about
# halves it: a card's 15 blocks are a hard ceiling.

cmake_minimum_required(VERSION 3.25)

# tinromAddExeStub(<variable>)
#
# Sets <variable> to the raw image of src/boot/exe_stub.S, the entry of a
# self-extracting PS-EXE. Assembled like the cart's stub and never linked.
function(tinromAddExeStub variable)
	set(stubSrc "${PROJECT_SOURCE_DIR}/src/boot/exe_stub.S")
	set(stubObj "${PROJECT_BINARY_DIR}/rom/exe_stub.o")
	set(stubBin "${PROJECT_BINARY_DIR}/rom/exe_stub.bin")

	get_property(made GLOBAL PROPERTY TINROM_EXE_STUB_BIN SET)

	if(NOT made)
		add_custom_command(
			OUTPUT  "${stubBin}"
			DEPENDS "${stubSrc}"
			COMMAND "${CMAKE_COMMAND}" -E make_directory "${PROJECT_BINARY_DIR}/rom"
			COMMAND
				"${CMAKE_C_COMPILER}"
				-march=r3000 -mabi=32 -mfp32 -mno-abicalls -fno-pic
				-c "${stubSrc}" -o "${stubObj}"
			COMMAND "${CMAKE_OBJCOPY}" -O binary -j .text "${stubObj}" "${stubBin}"
			COMMENT "Assembling the self-extracting PS-EXE stub"
			VERBATIM
		)
		set_property(GLOBAL PROPERTY TINROM_EXE_STUB_BIN "${stubBin}")
	endif()

	set(${variable} "${stubBin}" PARENT_SCOPE)
endfunction()

function(tinromAddMemcardImage target payloadTarget)
	set(mcrOut "${PROJECT_BINARY_DIR}/tinrom.mcr")
	set(cardPayload "${PROJECT_BINARY_DIR}/${payloadTarget}.psexe")
	set(packDepends "")

	if(TINROM_MCR_COMPRESS)
		tinromAddUnpacker(unpackBin)
		tinromAddExeStub(exeStubBin)

		set(packed "${PROJECT_BINARY_DIR}/${payloadTarget}.packed.psexe")

		add_custom_command(
			OUTPUT  "${packed}"
			DEPENDS ${payloadTarget} "${unpackBin}" "${exeStubBin}"
				"${PROJECT_SOURCE_DIR}/tools/packexe.py"
				"${PROJECT_SOURCE_DIR}/tools/tinlz.py"
			COMMAND
				"${Python3_EXECUTABLE}"
				"${PROJECT_SOURCE_DIR}/tools/packexe.py"
				--stub     "${exeStubBin}"
				--unpacker "${unpackBin}"
				--payload  "${cardPayload}"
				--output   "${packed}"
			COMMENT "Packing ${payloadTarget}.psexe for the memory card"
			VERBATIM
		)

		set(cardPayload "${packed}")
		set(packDepends "${packed}")
	endif()

	add_custom_command(
		OUTPUT  "${mcrOut}"
		DEPENDS ${payloadTarget} ${packDepends}
			"${PROJECT_SOURCE_DIR}/tools/makemcard.py"
		COMMAND
			"${Python3_EXECUTABLE}"
			"${PROJECT_SOURCE_DIR}/tools/makemcard.py"
			--payload "${cardPayload}"
			--output  "${mcrOut}"
		COMMENT "Building memory card image tinrom.mcr (stub)"
		VERBATIM
	)
	add_custom_target(${target} DEPENDS "${mcrOut}")
endfunction()
