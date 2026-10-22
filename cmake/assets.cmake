# tinrom - (C) 2026 TinWhisker - MIT licensed
#
# Asset conversion.
#
# ps1-bare-metal's convertImage() is not used: it needs NumPy and Pillow,
# neither of which has a wheel for an MSYS2 Python, so on a devkitPro or MSYS2
# setup the build dies at asset conversion with ModuleNotFoundError. tinrom
# converts one small texture, which is not worth a dependency that can fail
# to install at all.
#
# tools/convertimage.py produces byte-identical output using only the standard
# library. See NOTICE.md - it is a derivative of upstream's script.

cmake_minimum_required(VERSION 3.25)

# tinromConvertImage(<input> <bpp> <imageOut> [<paletteOut>])
#
# Paths are relative to the source tree for the input and the build tree for
# the outputs, matching upstream's convertImage().
function(tinromConvertImage input bpp)
	add_custom_command(
		OUTPUT  ${ARGN}
		DEPENDS "${PROJECT_SOURCE_DIR}/${input}"
			"${PROJECT_SOURCE_DIR}/tools/convertimage.py"
			"${PROJECT_SOURCE_DIR}/tools/mktim.py"
		COMMAND
			"${Python3_EXECUTABLE}"
			"${PROJECT_SOURCE_DIR}/tools/convertimage.py"
			-b ${bpp}
			"${PROJECT_SOURCE_DIR}/${input}"
			${ARGN}
		COMMENT "Converting ${input} to ${bpp}bpp"
		VERBATIM
	)
endfunction()

# tinromConvertFont(<input.tim> <output.c> [converter options...])
#
# A bitmap font sheet, PNG or TIM, into the C tables src/ui/font.c draws
# from; see tools/convertfont.py, which reads PNGs with tools/mktim.py. The input is relative to the source tree, the output
# to the build tree. The output is a source file, so list it in a target's
# sources rather than embedding it.
function(tinromConvertFont input output)
	add_custom_command(
		OUTPUT  "${PROJECT_BINARY_DIR}/${output}"
		DEPENDS "${PROJECT_SOURCE_DIR}/${input}"
			"${PROJECT_SOURCE_DIR}/tools/convertfont.py"
			"${PROJECT_SOURCE_DIR}/tools/mktim.py"
		COMMAND
			"${Python3_EXECUTABLE}"
			"${PROJECT_SOURCE_DIR}/tools/convertfont.py"
			"${PROJECT_SOURCE_DIR}/${input}"
			-o "${PROJECT_BINARY_DIR}/${output}"
			${ARGN}
		COMMENT "Converting ${input} to font tables"
		VERBATIM
	)
endfunction()

# tinromConvertBitFont(<input.txt> <output.c> [converter options...])
#
# A 1bpp font drawn as text into a C table; see tools/convertbitfont.py. The
# input is relative to the source tree, the output to the build tree.
function(tinromConvertBitFont input output)
	add_custom_command(
		OUTPUT  "${PROJECT_BINARY_DIR}/${output}"
		DEPENDS "${PROJECT_SOURCE_DIR}/${input}"
			"${PROJECT_SOURCE_DIR}/tools/convertbitfont.py"
		COMMAND
			"${Python3_EXECUTABLE}"
			"${PROJECT_SOURCE_DIR}/tools/convertbitfont.py"
			"${PROJECT_SOURCE_DIR}/${input}"
			-o "${PROJECT_BINARY_DIR}/${output}"
			${ARGN}
		COMMENT "Converting ${input} to a font table"
		VERBATIM
	)
endfunction()

# tinromConvertMesh(<input.obj> <texture image> <output.msh> [subdivideRatio])
#
# tools/mkmesh.py is the maintainer's own exporter, brought across from the
# ps1-python project; see NOTICE.md. Standard library only, like everything
# else in the build. It is left exactly as it is there, so the splitting pass
# below is a separate tool that runs first rather than a change to it.
#
# subdivideRatio splits any face larger than that many times the median face
# area. The PS1 sorts a whole face into one ordering-table bucket, so a large
# polygon cannot interleave with detail laid across it - see
# docs/model-sorting.md for what that costs and what splitting buys. Omit it,
# or pass 0, to convert the mesh as authored.
# tinromPngSize(<image> <widthVar> <heightVar>)
#
# Width and height straight out of a PNG's IHDR. A PNG is an 8-byte signature,
# a 4-byte chunk length and "IHDR", so the two 32-bit fields start at byte 16 -
# 32 hex characters in.
function(tinromPngSize image widthVar heightVar)
	file(READ "${image}" header LIMIT 24 HEX)

	string(LENGTH "${header}" headerLength)

	if(headerLength LESS 48)
		message(FATAL_ERROR "${image} is too short to be a PNG")
	endif()

	#
	# Checked with plain string compares rather than a regex: CMake's regex
	# flavour has no {n} repetition, so "[0-9a-f]{8}" matches those characters
	# literally and never succeeds. The signature is bytes 0..7 and the first
	# chunk's type - which must be IHDR - is bytes 12..15, so in hex
	# characters that is 0..15 and 24..31.
	#
	string(SUBSTRING "${header}"  0 16 signature)
	string(SUBSTRING "${header}" 24  8 chunkType)

	if(NOT signature STREQUAL "89504e470d0a1a0a")
		message(FATAL_ERROR "${image} is not a PNG")
	endif()
	if(NOT chunkType STREQUAL "49484452")
		message(FATAL_ERROR "${image} has no IHDR chunk, so it is not a PNG")
	endif()

	string(SUBSTRING "${header}" 32 8 widthHex)
	string(SUBSTRING "${header}" 40 8 heightHex)

	math(EXPR width  "0x${widthHex}")
	math(EXPR height "0x${heightHex}")

	set("${widthVar}"  "${width}"  PARENT_SCOPE)
	set("${heightVar}" "${height}" PARENT_SCOPE)
endfunction()

function(tinromConvertMesh input textureImage output)
	set(ratio 0)

	if(ARGC GREATER 3)
		set(ratio "${ARGV3}")
	endif()

	#
	# The texture size is read from the image rather than stated here.
	#
	# It used to be two numbers in the call, and the UV scaling in mkmesh.py
	# (`uv * (width - 1)`) has to match the image the texture converter
	# actually produces. Shrinking rom.png from 128x128 to 64x64 without
	# changing those numbers left every UV scaled for a texture twice the size,
	# AND made the uploader read 16 KB out of a 4 KB array. One fact, one
	# place.
	#
	set(textureFile "${PROJECT_SOURCE_DIR}/${textureImage}")

	tinromPngSize("${textureFile}" texWidth texHeight)

	# So that changing the image's dimensions re-runs CMake rather than
	# silently keeping the size read on the last configure.
	set_property(
		DIRECTORY "${PROJECT_SOURCE_DIR}"
		APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${textureFile}"
	)

	set(meshInput "${PROJECT_SOURCE_DIR}/${input}")
	set(meshDepends "${PROJECT_SOURCE_DIR}/${input}")

	if(NOT ratio EQUAL 0)
		get_filename_component(stem "${input}" NAME_WE)
		set(splitObj "${PROJECT_BINARY_DIR}/generated/${stem}-split.obj")

		add_custom_command(
			OUTPUT  "${splitObj}"
			DEPENDS "${PROJECT_SOURCE_DIR}/${input}"
				"${PROJECT_SOURCE_DIR}/tools/subdivide.py"
			COMMAND
				"${Python3_EXECUTABLE}"
				"${PROJECT_SOURCE_DIR}/tools/subdivide.py"
				"${PROJECT_SOURCE_DIR}/${input}"
				"${splitObj}"
				--ratio ${ratio}
			COMMENT "Splitting oversized faces in ${input}"
			VERBATIM
		)

		set(meshInput "${splitObj}")
		set(meshDepends "${splitObj}")
	endif()

	add_custom_command(
		OUTPUT  "${output}"
		DEPENDS "${meshDepends}"
			"${textureFile}"
			"${PROJECT_SOURCE_DIR}/tools/mkmesh.py"
			"${PROJECT_SOURCE_DIR}/tools/bbmodel.py"
			"${PROJECT_SOURCE_DIR}/tools/tmd.py"
		COMMAND
			"${Python3_EXECUTABLE}"
			"${PROJECT_SOURCE_DIR}/tools/mkmesh.py"
			"${meshInput}"
			-t ${texWidth} ${texHeight}
			-o "${output}"
		COMMENT "Converting ${input} to a PS1M mesh, ${texWidth}x${texHeight} texture"
		VERBATIM
	)
endfunction()

# tinromAddBinaryFile(<target> <symbol> <path>)
#
# Embeds a file into a target as a named symbol, like ps1-bare-metal's
# addBinaryFile(), but writes a RELATIVE path into the generated .incbin.
#
# Upstream writes an absolute one, which breaks whenever CMake and the
# toolchain disagree about path flavour. An MSYS2 CMake emits
#     .incbin "/d/Source/.../romTexture.dat"
# and a native Windows assembler cannot open that: the MSYS runtime rewrites
# paths passed on a command line, but never paths inside a file a tool reads.
#
# Ninja runs every command from the build directory, so a path relative to it
# is correct under either spelling, and survives a build directory with
# awkward characters in its name too.
#
# Also emits <symbol>Size, a uint32 of the blob's length, which anything
# parsing a container format needs in order to bounds-check it.
function(tinromAddBinaryFile target name path)
	set(asmFile "${PROJECT_BINARY_DIR}/includes/${target}_${name}.s")

	cmake_path(ABSOLUTE_PATH path OUTPUT_VARIABLE fullPath)
	cmake_path(
		RELATIVE_PATH fullPath
		BASE_DIRECTORY "${PROJECT_BINARY_DIR}"
		OUTPUT_VARIABLE incbinPath
	)

	file(
		CONFIGURE
		OUTPUT  "${asmFile}"
		CONTENT [[
.section .rodata.${name}, "a"
.balign 8

.global ${name}
.type ${name}, @object
.size ${name}, (${name}_end - ${name})

${name}:
	.incbin "${incbinPath}"
${name}_end:

.section .rodata.${name}Size, "a"
.balign 4

.global ${name}Size
.type ${name}Size, @object
.size ${name}Size, 4

${name}Size:
	.int (${name}_end - ${name})
]]
		ESCAPE_QUOTES
		NEWLINE_STYLE LF
	)

	target_sources(${target} PRIVATE "${asmFile}")

	# The dependency still has to be the absolute path: this one is consumed by
	# CMake, not written into a file.
	set_source_files_properties(
		"${asmFile}" PROPERTIES OBJECT_DEPENDS "${fullPath}"
	)
endfunction()
