//=================================================================================================
//
// The base physics DLL interface
//
// This is a thin CPU-agnostic wrapper for the actual Volt modules, which are named
// vphysics_jolt_sse2, vphysics_jolt_sse42 and vphysics_jolt_avx2.
//
// The module is loaded from the directory the wrapper itself lives in. If the wrapper has a suffix
// (vphysics_srv.so, vphysics_client.so), the same suffix is appended to the module name, so that
// differently-linked builds can live side by side.
//
//=================================================================================================

#include "tier0/basetypes.h"
#include "tier1/interface.h"
#include "vphysics_interface.h"

#include <cstdio>
#include <cstring>

#ifdef _WIN32
#include <intrin.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <cpuid.h>
#include <dlfcn.h>
#endif

// memdbgon must be the last include file in a .cpp file!!!
#include "tier0/memdbgon.h"

//-------------------------------------------------------------------------------------------------

class PhysicsWrapper final : public CBaseAppSystem<IPhysics>
{
public:
	bool Connect( CreateInterfaceFn factory ) override;
	void Disconnect() override;

	InitReturnVal_t Init() override;
	void Shutdown() override;
	void *QueryInterface( const char *pInterfaceName ) override;

#if defined( GAME_GMOD ) && PLATFORM_64BITS
	const AppSystemInfo_t *GetDependencies() override;
	AppSystemTier_t GetTier() override;
	void Reconnect( CreateInterfaceFn factory, const char *pInterfaceName ) override;
#endif

	IPhysicsEnvironment *CreateEnvironment() override;
	void DestroyEnvironment( IPhysicsEnvironment *pEnvironment ) override;
	IPhysicsEnvironment *GetActiveEnvironmentByIndex( int index ) override;

	IPhysicsObjectPairHash *CreateObjectPairHash() override;
	void DestroyObjectPairHash( IPhysicsObjectPairHash *pHash ) override;

	IPhysicsCollisionSet *FindOrCreateCollisionSet( unsigned int id, int maxElementCount ) override;
	IPhysicsCollisionSet *FindCollisionSet( unsigned int id ) override;
	void DestroyAllCollisionSets() override;

#if GAME_GMOD
	bool IsValidPhysicsObject( IPhysicsObject *pObject ) override;
#endif

public:
	static PhysicsWrapper &GetInstance() { return s_PhysicsInterface; }

private:
	bool InitWrapper();

	// Native module handle (HMODULE or dlopen handle). We don't use tier1's Sys_LoadModule: some
	// SDKs define HMODULE as uint32 on POSIX, which truncates 64-bit dlopen handles.
	void *m_pActualPhysicsModule = nullptr;
	IPhysics *m_pActualPhysicsInterface = nullptr;

	static PhysicsWrapper s_PhysicsInterface;
};

PhysicsWrapper PhysicsWrapper::s_PhysicsInterface;
EXPOSE_SINGLE_INTERFACE_GLOBALVAR( PhysicsWrapper, IPhysics, VPHYSICS_INTERFACE_VERSION, PhysicsWrapper::GetInstance() );

#ifdef VPHYSICS_INTERFACE_VERSION_GMOD
typedef PhysicsWrapper PhysicsWrapperGMod; // To avoid g_Create[...]_reg collision
EXPOSE_SINGLE_INTERFACE_GLOBALVAR( PhysicsWrapperGMod, IPhysics, VPHYSICS_INTERFACE_VERSION_GMOD, PhysicsWrapper::GetInstance() );
#endif

//-------------------------------------------------------------------------------------------------

enum CPULevel_t
{
	CPU_HAS_SSE2,
	CPU_HAS_SSE42,
	CPU_HAS_AVX2,
};

static void GetCPUID( int *pInfo, int func, int subfunc )
{
#ifdef _WIN32
	__cpuidex( pInfo, func, subfunc );
#else
	__cpuid_count( func, subfunc, pInfo[0], pInfo[1], pInfo[2], pInfo[3] );
#endif
}

// Whether the OS saves the AVX (YMM) register state on context switches.
static bool OSSupportsAVX()
{
#ifdef _WIN32
	const unsigned long long xcr0 = _xgetbv( 0 );
#else
	unsigned int eax, edx;
	__asm__ __volatile__( "xgetbv" : "=a"( eax ), "=d"( edx ) : "c"( 0 ) );
	const unsigned long long xcr0 = ( (unsigned long long)edx << 32 ) | eax;
#endif
	return ( xcr0 & 0x6 ) == 0x6; // XMM and YMM state
}

static CPULevel_t GetCPULevel()
{
	int cpuInfo[4];

	GetCPUID( cpuInfo, 0, 0 );
	const int numFuncs = cpuInfo[0];
	if ( numFuncs < 1 )
		return CPU_HAS_SSE2;

	GetCPUID( cpuInfo, 1, 0 );
	const bool hasSSE42   = cpuInfo[2] & ( 1 << 20 );
	const bool hasFMA     = cpuInfo[2] & ( 1 << 12 );
	const bool hasOSXSAVE = cpuInfo[2] & ( 1 << 27 );
	const bool hasAVX     = cpuInfo[2] & ( 1 << 28 );
	const bool hasF16C    = cpuInfo[2] & ( 1 << 29 );

	// The AVX2 build also uses FMA, F16C, BMI1 and LZCNT, so require all of them.
	if ( numFuncs >= 7 && hasAVX && hasFMA && hasF16C && hasOSXSAVE && OSSupportsAVX() )
	{
		GetCPUID( cpuInfo, 7, 0 );
		const bool hasAVX2 = cpuInfo[1] & ( 1 << 5 );
		const bool hasBMI1 = cpuInfo[1] & ( 1 << 3 );

		GetCPUID( cpuInfo, 0x80000000, 0 );
		bool hasLZCNT = false;
		if ( (unsigned int)cpuInfo[0] >= 0x80000001 )
		{
			GetCPUID( cpuInfo, 0x80000001, 0 );
			hasLZCNT = cpuInfo[2] & ( 1 << 5 );
		}

		if ( hasAVX2 && hasBMI1 && hasLZCNT )
			return CPU_HAS_AVX2;
	}

	return hasSSE42 ? CPU_HAS_SSE42 : CPU_HAS_SSE2;
}

static const char *GetModuleBaseName( CPULevel_t level )
{
	switch ( level )
	{
		case CPU_HAS_AVX2:		return "vphysics_jolt_avx2";
		case CPU_HAS_SSE42:		return "vphysics_jolt_sse42";
		default:				return "vphysics_jolt_sse2";
	}
}

//-------------------------------------------------------------------------------------------------

// Full path of the module that contains this function, or false.
static bool GetOwnModulePath( char *pOut, size_t outSize )
{
#ifdef _WIN32
	HMODULE hSelf = nullptr;
	if ( !GetModuleHandleExA( GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
		reinterpret_cast<LPCSTR>( &GetOwnModulePath ), &hSelf ) )
		return false;

	const DWORD len = GetModuleFileNameA( hSelf, pOut, (DWORD)outSize );
	return len > 0 && len < outSize;
#else
	Dl_info info;
	if ( !dladdr( reinterpret_cast<void *>( &GetOwnModulePath ), &info ) || !info.dli_fname )
		return false;

	const size_t len = strlen( info.dli_fname );
	if ( len >= outSize )
		return false;

	memcpy( pOut, info.dli_fname, len + 1 );
	return true;
#endif
}

static void *LoadNativeModule( const char *pPath )
{
#ifdef _WIN32
	return LoadLibraryExA( pPath, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH );
#else
	void *pHandle = dlopen( pPath, RTLD_NOW | RTLD_LOCAL );
	if ( !pHandle )
		Warning( "VPhysics Jolt: %s\n", dlerror() );
	return pHandle;
#endif
}

static void *GetNativeSymbol( void *pModule, const char *pName )
{
#ifdef _WIN32
	return reinterpret_cast<void *>( GetProcAddress( static_cast<HMODULE>( pModule ), pName ) );
#else
	return dlsym( pModule, pName );
#endif
}

static void UnloadNativeModule( void *pModule )
{
#ifdef _WIN32
	FreeLibrary( static_cast<HMODULE>( pModule ) );
#else
	dlclose( pModule );
#endif
}

// Builds "<dir of this module>/<baseName><suffix of this module><DLL_EXT>"
static bool BuildModulePath( const char *pBaseName, char *pOut, size_t outSize )
{
	char selfPath[4096];
	if ( !GetOwnModulePath( selfPath, sizeof( selfPath ) ) )
		return false;

	// Split into directory and file name.
	char *pFileName = selfPath;
	for ( char *p = selfPath; *p; p++ )
	{
		if ( *p == '/' || *p == '\\' )
			pFileName = p + 1;
	}

	// The suffix is whatever follows "vphysics" in our own name, minus the extension:
	// "vphysics_srv.so" -> "_srv", "vphysics.dll" -> "".
	const char *pSuffix = "";
	char suffix[64] = {};
	const char kPrefix[] = "vphysics";
	if ( strncmp( pFileName, kPrefix, sizeof( kPrefix ) - 1 ) == 0 )
	{
		const char *pStart = pFileName + sizeof( kPrefix ) - 1;
		const char *pDot = strchr( pStart, '.' );
		const size_t len = pDot ? (size_t)( pDot - pStart ) : strlen( pStart );
		if ( len < sizeof( suffix ) )
		{
			memcpy( suffix, pStart, len );
			pSuffix = suffix;
		}
	}

	*pFileName = '\0';
	const int written = snprintf( pOut, outSize, "%s%s%s%s", selfPath, pBaseName, pSuffix, DLL_EXT_STRING );
	return written > 0 && (size_t)written < outSize;
}

// Tries to load the actual vphysics module
bool PhysicsWrapper::InitWrapper()
{
	if ( m_pActualPhysicsInterface )
		return true;

	const char *pBaseName = GetModuleBaseName( GetCPULevel() );

	char modulePath[4096];
	void *pModule = nullptr;
	if ( BuildModulePath( pBaseName, modulePath, sizeof( modulePath ) ) )
		pModule = LoadNativeModule( modulePath );

	if ( !pModule )
	{
		// Fall back to the library search path.
		snprintf( modulePath, sizeof( modulePath ), "%s%s", pBaseName, DLL_EXT_STRING );
		pModule = LoadNativeModule( modulePath );
	}

	if ( !pModule )
	{
		Warning( "VPhysics Jolt: failed to load %s\n", modulePath );
		return false;
	}

	CreateInterfaceFn factory = reinterpret_cast<CreateInterfaceFn>( GetNativeSymbol( pModule, CREATEINTERFACE_PROCNAME ) );
	IPhysics *pInterface = factory ? static_cast<IPhysics *>( factory( VPHYSICS_INTERFACE_VERSION, nullptr ) ) : nullptr;
	if ( !pInterface )
	{
		Warning( "VPhysics Jolt: %s does not expose " VPHYSICS_INTERFACE_VERSION "\n", modulePath );
		UnloadNativeModule( pModule );
		return false;
	}

	m_pActualPhysicsModule = pModule;
	m_pActualPhysicsInterface = pInterface;
	return true;
}

bool PhysicsWrapper::Connect( CreateInterfaceFn factory )
{
	if ( !InitWrapper() )
		return false;

	return m_pActualPhysicsInterface->Connect( factory );
}

void PhysicsWrapper::Disconnect()
{
	if ( !m_pActualPhysicsInterface )
		return;

	m_pActualPhysicsInterface->Disconnect();
	m_pActualPhysicsInterface = nullptr;

	UnloadNativeModule( m_pActualPhysicsModule );
	m_pActualPhysicsModule = nullptr;
}

//-------------------------------------------------------------------------------------------------

InitReturnVal_t PhysicsWrapper::Init()
{
	return m_pActualPhysicsInterface->Init();
}

void PhysicsWrapper::Shutdown()
{
	m_pActualPhysicsInterface->Shutdown();
}

void *PhysicsWrapper::QueryInterface( const char *pInterfaceName )
{
	// This function can be called before Connect, so try and load the real module early
	if ( !InitWrapper() )
		return nullptr;

	return m_pActualPhysicsInterface->QueryInterface( pInterfaceName );
}

#if defined( GAME_GMOD ) && PLATFORM_64BITS
const AppSystemInfo_t *PhysicsWrapper::GetDependencies()
{
	return InitWrapper() ? m_pActualPhysicsInterface->GetDependencies() : nullptr;
}

AppSystemTier_t PhysicsWrapper::GetTier()
{
	return InitWrapper() ? m_pActualPhysicsInterface->GetTier() : APP_SYSTEM_TIER_OTHER;
}

void PhysicsWrapper::Reconnect( CreateInterfaceFn factory, const char *pInterfaceName )
{
	if ( InitWrapper() )
		m_pActualPhysicsInterface->Reconnect( factory, pInterfaceName );
}
#endif

//-------------------------------------------------------------------------------------------------

IPhysicsEnvironment *PhysicsWrapper::CreateEnvironment()
{
	return m_pActualPhysicsInterface->CreateEnvironment();
}

void PhysicsWrapper::DestroyEnvironment( IPhysicsEnvironment *pEnvironment )
{
	m_pActualPhysicsInterface->DestroyEnvironment( pEnvironment );
}

IPhysicsEnvironment *PhysicsWrapper::GetActiveEnvironmentByIndex( int index )
{
	return m_pActualPhysicsInterface->GetActiveEnvironmentByIndex( index );
}

//-------------------------------------------------------------------------------------------------

IPhysicsObjectPairHash *PhysicsWrapper::CreateObjectPairHash()
{
	return m_pActualPhysicsInterface->CreateObjectPairHash();
}

void PhysicsWrapper::DestroyObjectPairHash( IPhysicsObjectPairHash *pHash )
{
	m_pActualPhysicsInterface->DestroyObjectPairHash( pHash );
}

//-------------------------------------------------------------------------------------------------

IPhysicsCollisionSet *PhysicsWrapper::FindOrCreateCollisionSet( unsigned int id, int maxElementCount )
{
	return m_pActualPhysicsInterface->FindOrCreateCollisionSet( id, maxElementCount );
}

IPhysicsCollisionSet *PhysicsWrapper::FindCollisionSet( unsigned int id )
{
	return m_pActualPhysicsInterface->FindCollisionSet( id );
}

void PhysicsWrapper::DestroyAllCollisionSets()
{
	m_pActualPhysicsInterface->DestroyAllCollisionSets();
}

#if GAME_GMOD
bool PhysicsWrapper::IsValidPhysicsObject( IPhysicsObject *pObject )
{
	return m_pActualPhysicsInterface->IsValidPhysicsObject( pObject );
}
#endif
