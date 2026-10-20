# Volt (VPhysics Jolt) build for Garry's Mod.
#
# Builds against a Source SDK tree (VJOLT_SDK_DIR, e.g. mini-source-sdk/gmod/src from
# https://github.com/RaphaelIT7/mini-source-sdk, branch gmod-changes). Normally driven by
# build.sh, which runs it in Docker for linux32, linux64, win32 and win64.
#
# The SDK2013 and Alien Swarm builds still use VPC (see build.md).

cmake_minimum_required( VERSION 3.25 )

project( vphysics_jolt LANGUAGES C CXX )

set( VJOLT_SDK_DIR "" CACHE PATH "Source SDK src directory (contains public/vphysics_interface.h)" )
set( VJOLT_BIN_DIR "bin" CACHE STRING "Install directory of the binaries, relative to the install prefix" )
set( VJOLT_VARIANTS "default" CACHE STRING
	"Link variants to build. default: links tier0/vstdlib. srv: links the _srv libraries and adds _srv to the file names (32-bit public-branch dedicated server). client: same with _client." )
set( VJOLT_LEVELS "sse2;sse42;avx2" CACHE STRING "CPU levels to build" )

if ( NOT EXISTS "${VJOLT_SDK_DIR}/public/vphysics_interface.h" )
	message( FATAL_ERROR "VJOLT_SDK_DIR (${VJOLT_SDK_DIR}) is not a Source SDK src directory" )
endif()
get_filename_component( SDK "${VJOLT_SDK_DIR}" ABSOLUTE )

if ( CMAKE_SIZEOF_VOID_P EQUAL 8 )
	set( VJOLT_64 ON )
else()
	set( VJOLT_64 OFF )
endif()

if ( NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES )
	set( CMAKE_BUILD_TYPE RelWithDebInfo )
endif()

set( CMAKE_CXX_STANDARD 20 )
set( CMAKE_CXX_STANDARD_REQUIRED ON )
set( CMAKE_CXX_EXTENSIONS ON )
set( CMAKE_POSITION_INDEPENDENT_CODE ON )
set( CMAKE_CXX_VISIBILITY_PRESET hidden )
set( CMAKE_C_VISIBILITY_PRESET hidden )
set( CMAKE_VISIBILITY_INLINES_HIDDEN ON )
set( CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded" ) # /MT, as the Source engine uses
set( CMAKE_INSTALL_SO_NO_EXE OFF ) # Install .so files as 755 like the game's own (Debian defaults to 644)

#--------------------------------------------------------------------------------------------------
# Flags shared by everything that includes Source SDK headers
#--------------------------------------------------------------------------------------------------

add_library( vjolt_sdk_flags INTERFACE )

target_include_directories( vjolt_sdk_flags INTERFACE
	"${SDK}/common"
	"${SDK}/public"
	"${SDK}/public/tier0"
	"${SDK}/public/tier1"
)

target_compile_definitions( vjolt_sdk_flags INTERFACE
	VPC
	RAD_TELEMETRY_DISABLED
	GAME_GMOD
	GAME_SDK2013
	NDEBUG
	VPROF_LEVEL=1
	$<$<BOOL:${VJOLT_64}>:PLATFORM_64BITS>
)

if ( WIN32 )
	target_compile_definitions( vjolt_sdk_flags INTERFACE
		WIN32 _WIN32 _WINDOWS _USRDLL
		COMPILER_MSVC
		$<IF:$<BOOL:${VJOLT_64}>,COMPILER_MSVC64;WIN64;_WIN64,COMPILER_MSVC32>
		_DLL_EXT=.dll
		_CRT_SECURE_NO_DEPRECATE _CRT_NONSTDC_NO_DEPRECATE
		_ALLOW_RUNTIME_LIBRARY_MISMATCH _ALLOW_ITERATOR_DEBUG_LEVEL_MISMATCH _ALLOW_MSC_VER_MISMATCH
	)
	target_compile_options( vjolt_sdk_flags INTERFACE
		/EHsc /GR /GF /Gy /Gw /Oi /Zc:inline /Zc:__cplusplus
		/W0
	)
else()
	target_compile_definitions( vjolt_sdk_flags INTERFACE
		GNUC COMPILER_GCC POSIX _POSIX LINUX _LINUX
		_DLL_EXT=.so
		NO_HOOK_MALLOC NO_MALLOC_OVERRIDE
	)
	target_compile_options( vjolt_sdk_flags INTERFACE
		-fno-strict-aliasing
		-fno-omit-frame-pointer
		-Usprintf -Ustrncpy -UPROTECTED_THINGS_ENABLE
		$<$<COMPILE_LANGUAGE:CXX>:-fpermissive>
		-w
	)
	if ( VJOLT_64 )
		target_compile_options( vjolt_sdk_flags INTERFACE -march=x86-64 -mtune=generic )
	else()
		target_compile_options( vjolt_sdk_flags INTERFACE -m32 -march=pentium4 -mtune=generic -msse2 -mfpmath=sse )
		target_link_options( vjolt_sdk_flags INTERFACE -m32 )
	endif()
endif()

# Linker settings for our shared libraries
add_library( vjolt_module_flags INTERFACE )
if ( WIN32 )
	target_link_libraries( vjolt_module_flags INTERFACE shell32 user32 advapi32 gdi32 ole32 )
	target_link_options( vjolt_module_flags INTERFACE /DEBUG:FULL /OPT:REF /OPT:ICF /ignore:4221
		$<$<NOT:$<BOOL:${VJOLT_64}>>:/SAFESEH:NO> )
else()
	target_link_options( vjolt_module_flags INTERFACE
		-Wl,--no-undefined
		-Wl,--as-needed
		-Wl,--build-id
		-static-libgcc
		-static-libstdc++
		"-Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/cmake/exports.map"
	)
	target_link_libraries( vjolt_module_flags INTERFACE dl pthread m )
endif()

# tier0 and vstdlib import libraries, per link variant
if ( WIN32 )
	set( SDK_LIBDIR "${SDK}/lib/public$<$<BOOL:${VJOLT_64}>:/x64>" )
elseif ( VJOLT_64 )
	set( SDK_LIBDIR "${SDK}/lib/public/linux64" )
else()
	set( SDK_LIBDIR "${SDK}/lib/public/linux32" )
endif()

function( vjolt_link_engine_libs target suffix )
	if ( WIN32 )
		target_link_libraries( ${target} PRIVATE "${SDK_LIBDIR}/tier0.lib" "${SDK_LIBDIR}/vstdlib.lib" )
	else()
		# Linked by name (-l), so DT_NEEDED holds the bare file name: the SDK's stubs have no SONAME.
		target_link_directories( ${target} PRIVATE "${SDK_LIBDIR}" )
		target_link_libraries( ${target} PRIVATE tier0${suffix} vstdlib${suffix} )
	endif()
endfunction()

# Linux: split the debug info into <file>.dbg next to the binary
function( vjolt_split_debug target )
	if ( NOT WIN32 AND CMAKE_OBJCOPY AND CMAKE_STRIP )
		add_custom_command( TARGET ${target} POST_BUILD
			WORKING_DIRECTORY "$<TARGET_FILE_DIR:${target}>"
			COMMAND "${CMAKE_OBJCOPY}" --only-keep-debug "$<TARGET_FILE_NAME:${target}>" "$<TARGET_FILE_NAME:${target}>.dbg"
			COMMAND "${CMAKE_STRIP}" --strip-debug --strip-unneeded "$<TARGET_FILE_NAME:${target}>"
			COMMAND "${CMAKE_OBJCOPY}" "--add-gnu-debuglink=$<TARGET_FILE_NAME:${target}>.dbg" "$<TARGET_FILE_NAME:${target}>"
			VERBATIM )
	endif()
endfunction()

function( vjolt_install target component )
	set_target_properties( ${target} PROPERTIES PREFIX "" )
	install( TARGETS ${target} COMPONENT ${component}
		LIBRARY DESTINATION "${VJOLT_BIN_DIR}"
		RUNTIME DESTINATION "${VJOLT_BIN_DIR}" )
	if ( WIN32 )
		install( FILES "$<TARGET_PDB_FILE:${target}>" COMPONENT ${component} DESTINATION "${VJOLT_BIN_DIR}" OPTIONAL )
	else()
		install( FILES "$<TARGET_FILE:${target}>.dbg" COMPONENT ${component} DESTINATION "${VJOLT_BIN_DIR}" OPTIONAL )
	endif()
endfunction()

#--------------------------------------------------------------------------------------------------
# Source SDK static libraries (tier1, mathlib, tier2), built from the SDK sources
#--------------------------------------------------------------------------------------------------

set( TIER1_FILES
	bitbuf.cpp byteswap.cpp characterset.cpp checksum_crc.cpp checksum_md5.cpp checksum_sha1.cpp
	commandbuffer.cpp convar.cpp datamanager.cpp diff.cpp generichash.cpp ilocalize.cpp interface.cpp
	KeyValues.cpp kvpacker.cpp mempool.cpp memstack.cpp NetAdr.cpp newbitbuf.cpp rangecheckedvar.cpp
	reliabletimer.cpp snappy-sinksource.cpp snappy-stubs-internal.cpp snappy.cpp sparsematrix.cpp
	splitstring.cpp stringpool.cpp strtools.cpp strtools_unicode.cpp tier1.cpp tokenreader.cpp
	uniqueid.cpp utlbinaryblock.cpp utlbuffer.cpp utlbufferutil.cpp utlstring.cpp utlsymbol.cpp
)
if ( WIN32 )
	list( APPEND TIER1_FILES processor_detect.cpp )
else()
	list( APPEND TIER1_FILES processor_detect_linux.cpp qsort_s.cpp pathmatch.cpp )
endif()
list( TRANSFORM TIER1_FILES PREPEND "${SDK}/tier1/" )
add_library( sdk_tier1 STATIC ${TIER1_FILES} )
target_link_libraries( sdk_tier1 PRIVATE vjolt_sdk_flags )
target_compile_definitions( sdk_tier1 PRIVATE TIER1_STATIC_LIB )

set( MATHLIB_FILES
	3dnow.cpp almostequal.cpp anorms.cpp bumpvects.cpp color_conversion.cpp halton.cpp IceKey.cpp
	imagequant.cpp lightdesc.cpp mathlib_base.cpp polyhedron.cpp powsse.cpp quantize.cpp randsse.cpp
	simdvectormatrix.cpp sparse_convolution_noise.cpp spherical.cpp sse.cpp sseconst.cpp ssenoise.cpp
	vector.cpp vmatrix.cpp
)
list( TRANSFORM MATHLIB_FILES PREPEND "${SDK}/mathlib/" )
add_library( sdk_mathlib STATIC ${MATHLIB_FILES} )
target_link_libraries( sdk_mathlib PRIVATE vjolt_sdk_flags )
target_include_directories( sdk_mathlib PRIVATE "${SDK}/public/mathlib" )
target_compile_definitions( sdk_mathlib PRIVATE MATHLIB_LIB )

add_library( sdk_tier2 STATIC "${SDK}/tier2/tier2.cpp" )
target_link_libraries( sdk_tier2 PRIVATE vjolt_sdk_flags )
target_compile_definitions( sdk_tier2 PRIVATE TIER2_STATIC_LIB )

#--------------------------------------------------------------------------------------------------
# Jolt, one static library per CPU level
#--------------------------------------------------------------------------------------------------

set( JOLT_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/joltphysics/src" )
if ( NOT EXISTS "${JOLT_ROOT}/Jolt/Jolt.cmake" )
	message( FATAL_ERROR "Jolt is missing, run: git submodule update --init" )
endif()

# Take the base source list from Jolt.cmake, so it follows upstream. The optional parts
# (ObjectStream, compute backends) are not used.
file( READ "${JOLT_ROOT}/Jolt/Jolt.cmake" JOLT_CMAKE )
string( REGEX MATCH "set\\(JOLT_PHYSICS_SRC_FILES([^)]*)\\)" _ "${JOLT_CMAKE}" )
string( REGEX MATCHALL "\\\${JOLT_PHYSICS_ROOT}/[^ \t\r\n]+\\.cpp" JOLT_SOURCES "${CMAKE_MATCH_1}" )
list( TRANSFORM JOLT_SOURCES REPLACE "\\\${JOLT_PHYSICS_ROOT}" "${JOLT_ROOT}/Jolt" )
list( LENGTH JOLT_SOURCES _count )
if ( _count LESS 100 )
	message( FATAL_ERROR "Could not read the source list from Jolt.cmake (got ${_count} files)" )
endif()

# Settings that must match between Jolt and everything that includes its headers
function( vjolt_level_flags target level visibility )
	target_include_directories( ${target} ${visibility} "${JOLT_ROOT}" )
	target_compile_definitions( ${target} ${visibility} JPH_DEBUG_RENDERER )
	# MSVC has no SSE4.2 switch; like the VPC build it relies on the JPH_USE_* defines there.
	if ( level STREQUAL "sse42" )
		target_compile_definitions( ${target} ${visibility} JPH_USE_SSE4_1 JPH_USE_SSE4_2 )
		if ( NOT MSVC )
			target_compile_options( ${target} ${visibility} -msse4.1 -msse4.2 -mpopcnt )
		endif()
	elseif ( level STREQUAL "avx2" )
		target_compile_definitions( ${target} ${visibility}
			JPH_USE_SSE4_1 JPH_USE_SSE4_2 JPH_USE_AVX JPH_USE_AVX2
			JPH_USE_LZCNT JPH_USE_TZCNT JPH_USE_F16C JPH_USE_FMADD )
		if ( MSVC )
			target_compile_options( ${target} ${visibility} /arch:AVX2 )
		else()
			target_compile_options( ${target} ${visibility}
				-msse4.1 -msse4.2 -mpopcnt -mavx -mavx2 -mbmi -mlzcnt -mf16c -mfma )
		endif()
	elseif ( NOT level STREQUAL "sse2" )
		message( FATAL_ERROR "Unknown CPU level ${level}" )
	endif()
endfunction()

#--------------------------------------------------------------------------------------------------
# Modules
#--------------------------------------------------------------------------------------------------

set( VJOLT_SOURCES
	vjolt_collide.cpp vjolt_collide_trace.cpp vjolt_constraints.cpp vjolt_controller_drag.cpp
	vjolt_controller_fluid.cpp vjolt_controller_motion.cpp vjolt_controller_player.cpp
	vjolt_controller_shadow.cpp vjolt_controller_vehicle.cpp vjolt_debugrender.cpp
	vjolt_environment.cpp vjolt_friction.cpp vjolt_interface.cpp vjolt_keyvalues_schema.cpp
	vjolt_listener_contact.cpp vjolt_object.cpp vjolt_objectpairhash.cpp vjolt_parse.cpp
	vjolt_querymodel.cpp vjolt_surfaceprops.cpp
)
list( TRANSFORM VJOLT_SOURCES PREPEND "${CMAKE_CURRENT_SOURCE_DIR}/vphysics_jolt/" )

set( SDK_MODULE_SOURCES
	"${SDK}/public/collisionutils.cpp"
	"${SDK}/public/filesystem_helpers.cpp"
	"${SDK}/public/tier0/memoverride.cpp"
)

foreach ( level IN LISTS VJOLT_LEVELS )
	add_library( jolt_${level} STATIC ${JOLT_SOURCES} )
	vjolt_level_flags( jolt_${level} ${level} PUBLIC )
	if ( NOT WIN32 AND NOT VJOLT_64 )
		target_compile_options( jolt_${level} PRIVATE -m32 -msse2 -mfpmath=sse )
	elseif ( WIN32 AND NOT VJOLT_64 AND NOT level STREQUAL "avx2" )
		target_compile_options( jolt_${level} PRIVATE /arch:SSE2 )
	endif()
	target_precompile_headers( jolt_${level} PRIVATE "${JOLT_ROOT}/Jolt/Jolt.h" )

	# Compiled once per level, linked once per variant
	add_library( vjolt_objects_${level} OBJECT ${VJOLT_SOURCES} ${SDK_MODULE_SOURCES} )
	target_link_libraries( vjolt_objects_${level} PRIVATE vjolt_sdk_flags jolt_${level} )
	target_compile_definitions( vjolt_objects_${level} PRIVATE DLLNAME=vphysics_jolt_${level} )
	target_include_directories( vjolt_objects_${level} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/vphysics_jolt" )
	target_precompile_headers( vjolt_objects_${level} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/vphysics_jolt/cbase.h" )
	set_source_files_properties( ${SDK_MODULE_SOURCES} TARGET_DIRECTORY vjolt_objects_${level}
		PROPERTIES SKIP_PRECOMPILE_HEADERS ON )
endforeach()

add_library( vjolt_wrapper_objects OBJECT
	"${CMAKE_CURRENT_SOURCE_DIR}/vphysics_wrapper/vphysics_wrapper.cpp"
	"${SDK}/public/tier0/memoverride.cpp" )
target_link_libraries( vjolt_wrapper_objects PRIVATE vjolt_sdk_flags )
target_compile_definitions( vjolt_wrapper_objects PRIVATE DLLNAME=vphysics )

foreach ( variant IN LISTS VJOLT_VARIANTS )
	if ( variant STREQUAL "default" )
		set( suffix "" )
	else()
		set( suffix "_${variant}" )
	endif()

	foreach ( level IN LISTS VJOLT_LEVELS )
		set( target vphysics_jolt_${level}${suffix} )
		add_library( ${target} SHARED $<TARGET_OBJECTS:vjolt_objects_${level}> )
		target_link_libraries( ${target} PRIVATE
			vjolt_sdk_flags vjolt_module_flags jolt_${level} sdk_tier2 sdk_mathlib sdk_tier1 )
		vjolt_link_engine_libs( ${target} "${suffix}" )
		vjolt_split_debug( ${target} )
		vjolt_install( ${target} ${variant} )
	endforeach()

	set( target vphysics${suffix} )
	add_library( ${target} SHARED $<TARGET_OBJECTS:vjolt_wrapper_objects> )
	target_link_libraries( ${target} PRIVATE vjolt_sdk_flags vjolt_module_flags sdk_tier1 )
	vjolt_link_engine_libs( ${target} "${suffix}" )
	vjolt_split_debug( ${target} )
	vjolt_install( ${target} ${variant} )
endforeach()
