# Windows with MSVC under Wine (msvc-wine, see docker/windows.Dockerfile).
# Set VJOLT_WIN_ARCH to x86 or x64. MSVC_DIR defaults to /opt/msvc.
set( CMAKE_SYSTEM_NAME Windows )
set( CMAKE_SYSTEM_VERSION 10.0 )

if ( NOT VJOLT_WIN_ARCH )
	set( VJOLT_WIN_ARCH "$ENV{VJOLT_WIN_ARCH}" )
endif()
if ( VJOLT_WIN_ARCH STREQUAL "x86" )
	set( CMAKE_SYSTEM_PROCESSOR X86 )
elseif ( VJOLT_WIN_ARCH STREQUAL "x64" )
	set( CMAKE_SYSTEM_PROCESSOR AMD64 )
else()
	message( FATAL_ERROR "Set VJOLT_WIN_ARCH to x86 or x64" )
endif()
list( APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES VJOLT_WIN_ARCH MSVC_DIR )

if ( NOT MSVC_DIR )
	set( MSVC_DIR /opt/msvc )
endif()

set( _bin "${MSVC_DIR}/bin/${VJOLT_WIN_ARCH}" )
set( CMAKE_C_COMPILER "${_bin}/cl" )
set( CMAKE_CXX_COMPILER "${_bin}/cl" )
set( CMAKE_LINKER "${_bin}/link" )
set( CMAKE_AR "${_bin}/lib" )
set( CMAKE_RC_COMPILER "${_bin}/rc" )
set( CMAKE_MT "${_bin}/mt" )

# /Z7: debug info in the objects, so the compiler doesn't need mspdbsrv. The linker still writes a PDB.
set( CMAKE_MSVC_DEBUG_INFORMATION_FORMAT Embedded )
set( CMAKE_MSVC_RUNTIME_LIBRARY MultiThreaded )

set( CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER )
