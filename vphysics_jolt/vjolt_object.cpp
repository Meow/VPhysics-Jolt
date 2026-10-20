//=================================================================================================
//
// A physics object, implemented as a wrapper over JPH::Body
// Every tangible object in the game has one of these
//
//=================================================================================================

#include "cbase.h"

#include "vjolt_collide.h"
#include "vjolt_surfaceprops.h"
#include "vjolt_friction.h"
#include "vjolt_environment.h"
#include "vjolt_layers.h"
#include "vjolt_listener_contact.h"
#include "vjolt_controller_drag.h"
#include "vjolt_controller_shadow.h"

#include "vjolt_object.h"

// memdbgon must be the last include file in a .cpp file!!!
#include "tier0/memdbgon.h"

static ConVar vjolt_object_debug( "vjolt_object_debug", "0", FCVAR_NONE, "Log direct velocity/force API calls on physics objects." );

//-------------------------------------------------------------------------------------------------

#if GAME_GMOD
// RaphaelIT7: Gmod needs to keep track of which objects exists
// This is because Lua stores a pointer to the IPhysicsObject
// And it cannot invalidate all pointers since
// first - there can be multiple userdatas that keep a pointer stored
// secondly - an IPhysicsObject can be deleted outside of Lua

// NOTE: Perferably gmod will use the SetLuaReference function
// & use the IGModPhysicsObjectEvent::ObjectDestroyed callback to invalidate references instead
static std::unordered_set< IPhysicsObject * > g_pObjects;
static void RegisterPhysicsObject( IPhysicsObject *pObject )
{
	g_pObjects.insert( pObject );
}

static void UnregisterPhysicsObject( IPhysicsObject *pObject )
{
	g_pObjects.erase( pObject );
}

// Used by JoltPhysicsInterface::IsValidPhysicsObject. Compares pointers only, so pObject may be
// a dangling pointer.
bool IsRegisteredPhysicsObject( IPhysicsObject *pObject )
{
	return g_pObjects.find( pObject ) != g_pObjects.end();
}
#endif

JoltPhysicsObject::JoltPhysicsObject( JPH::Body *pBody, JoltPhysicsEnvironment *pEnvironment, bool bStatic, int nMaterialIndex, const objectparams_t *pParams )
	: m_pBody( pBody )
	, m_pEnvironment( pEnvironment )
	, m_pPhysicsSystem( pEnvironment->GetPhysicsSystem() )
	, m_bStatic( bStatic )
	, m_flCachedMass( pParams->mass )
	, m_flCachedInvMass( m_flCachedMass ? 1.0f / m_flCachedMass : 0.0f )
	, m_pGameData( pParams->pGameData )
	, m_materialIndex( Max( nMaterialIndex, 0 ) ) // Sometimes we get passed -1.
	, m_flVolume( pParams->volume )
	, m_pName( pParams->pName )
{
	// Josh:
	// Assert that m_pGameData is the first element, some games
	// decide to just read this directly by offsetting by the vtable size
	// instead of calling GetGameData().
	static_assert( offsetof( JoltPhysicsObject, m_pGameData ) == sizeof( void * ) );

	// Set the body's userdata as ourselves
	pBody->SetUserData( reinterpret_cast<uint64>( this ) );
	if ( !m_pBody->IsStatic() )
	{
		JPH::MotionProperties* pMotionProperties = m_pBody->GetMotionProperties();
		pMotionProperties->SetLinearDamping( pParams->damping );
		pMotionProperties->SetAngularDamping( pParams->rotdamping );
	}

	if ( !IsStatic() )
	{
		GetVelocity( &m_vLastVelocity, &m_vLastAngularVelocity );
		GetPosition( &m_vLastPosition, &m_qLastOrientation );

		RecomputeDrag();

		if ( pParams->dragCoefficient != 0.0f )
		{
			float flDrag = pParams->dragCoefficient;
			SetDragCoefficient( &flDrag, &flDrag );
			EnableDrag( true );
		}
	}

	UpdateMaterialProperties();
	m_pEnvironment->MarkObjectListDirty();

#if GAME_GMOD
	if ( m_pEnvironment->GetGModObjectEvents() )
		m_pEnvironment->GetGModObjectEvents()->ObjectCreated( this );

	RegisterPhysicsObject( this );
#endif
}

JoltPhysicsObject::JoltPhysicsObject( JPH::Body *pBody, JoltPhysicsEnvironment *pEnvironment, void *pGameData, JPH::StateRecorder &recorder )
	: m_pBody( pBody )
	, m_pEnvironment( pEnvironment )
	, m_pPhysicsSystem( pEnvironment->GetPhysicsSystem() )
	, m_pGameData( pGameData )
{
	RestoreObjectState( recorder );
	m_pEnvironment->MarkObjectListDirty();

#if GAME_GMOD
	if (m_pEnvironment->GetGModObjectEvents())
		m_pEnvironment->GetGModObjectEvents()->ObjectCreated( this );

	RegisterPhysicsObject( this );
#endif
}

JoltPhysicsObject::~JoltPhysicsObject()
{
#if GAME_GMOD
	UnregisterPhysicsObject( this );

	if (m_pEnvironment->GetGModObjectEvents())
		m_pEnvironment->GetGModObjectEvents()->ObjectDestroyed( this );
#endif

	RemoveShadowController();

	// Josh:
	// Iterate over this in reverse as we could remove a listener from inside this callback
	for ( int i = m_destroyedListeners.Count() - 1; i >= 0; i-- )
		m_destroyedListeners[ i ]->OnJoltPhysicsObjectDestroyed( this );

	m_pEnvironment->RemoveDirtyStaticBody( GetBodyID() );
	m_pEnvironment->MarkObjectListDirty();

	JPH::BodyInterface& bodyInterface = m_pPhysicsSystem->GetBodyInterfaceNoLock();
	bodyInterface.DestroyBody( GetBodyID() );
}

bool JoltPhysicsObject::IsStatic() const
{
	// Whether this a static body to VPhysics and not
	// to Jolt (static or motion disabled) itself.
	return m_bStatic;
}

bool JoltPhysicsObject::IsAsleep() const
{
	return !m_pBody->IsActive();
}

bool JoltPhysicsObject::IsTrigger() const
{
	return m_pBody->IsSensor();
}

bool JoltPhysicsObject::IsFluid() const
{
	return m_pFluidController != nullptr;
}

bool JoltPhysicsObject::IsHinged() const
{
	Log_Stub( LOG_VJolt );
	return false;
}

bool JoltPhysicsObject::IsCollisionEnabled() const
{
	return m_bCachedCollisionEnabled;
}

bool JoltPhysicsObject::IsGravityEnabled() const
{
	if ( !m_pBody->IsStatic() )
	{
		JPH::MotionProperties* pMotionProperties = m_pBody->GetMotionProperties();
		return pMotionProperties->GetGravityFactor() != 0.0f;
	}

	return false;
}

bool JoltPhysicsObject::IsDragEnabled() const
{
	return m_bDragEnabled;
}

bool JoltPhysicsObject::IsMotionEnabled() const
{
	return !m_bPinned;
}

bool JoltPhysicsObject::IsMoveable() const
{
	return IsMotionEnabled() && !IsStatic();
}

bool JoltPhysicsObject::IsAttachedToConstraint( bool bExternalOnly ) const
{
	for (JPH::Constraint *constraint : m_pPhysicsSystem->GetConstraints())
	{
		if ( constraint->GetType() != JPH::EConstraintType::TwoBodyConstraint )
			continue;

		JPH::TwoBodyConstraint *twoBody = static_cast<JPH::TwoBodyConstraint*>( constraint );
		if ( twoBody->GetBody1() == m_pBody )
		{
			// RaphaelIT7: (ToDo) Check if this might need to call TryGetBody to be safe?
			if ( bExternalOnly )
			{
				// RaphaelIT7: vphysics seems to compare bExternalOnly by comparing the GameData, so lets keep that behavior too
				JoltPhysicsObject *pObject = reinterpret_cast< JoltPhysicsObject * >( twoBody->GetBody2()->GetUserData() );
				if ( pObject && pObject->GetGameData() != GetGameData() )
					return true;
				continue;
			}

			return true;
		} else if ( twoBody->GetBody2() == m_pBody )
		{
			if ( bExternalOnly )
			{
				JoltPhysicsObject *pObject = reinterpret_cast< JoltPhysicsObject * >( twoBody->GetBody1()->GetUserData() );
				if ( pObject && pObject->GetGameData() != GetGameData() )
					return true;
				continue;
			}

			return true;
		}
	}

	return false;
}

//-------------------------------------------------------------------------------------------------

void JoltPhysicsObject::EnableCollisions( bool enable )
{
	// Josh:
	// When collisions are disabled, they disable for EVERYTHING, including against the world.
	m_bCachedCollisionEnabled = enable;
	UpdateLayer();
}

void JoltPhysicsObject::EnableGravity( bool enable )
{
	if ( !m_pBody->IsStatic() )
	{
		JPH::MotionProperties* pMotionProperties = m_pBody->GetMotionProperties();
		pMotionProperties->SetGravityFactor( enable ? 1.0f : 0.0f );
	}
}

void JoltPhysicsObject::EnableDrag( bool enable )
{
	if ( IsStatic() )
		return;

	if ( m_bDragEnabled == enable )
		return;

	m_bDragEnabled = enable;

	JoltPhysicsDragController *pDragController = m_pEnvironment->GetDragController();
	if ( enable )
		pDragController->RegisterObject( this );
	else
		pDragController->UnregisterObject( this );
}

void JoltPhysicsObject::EnableMotion( bool enable )
{
	if ( IsStatic() )
		return;

	const bool bPinned = !enable;

	if ( m_bPinned == bPinned )
		return;

	m_bPinned = bPinned;
	RecalculateFixedConstraintPartnerMovable(); // RaphaelIT7: For improved fixed constraints
	UpdateLayer();
}

//-------------------------------------------------------------------------------------------------

void JoltPhysicsObject::SetGameData( void *pGameData )
{
	m_pGameData = pGameData;
}

void *JoltPhysicsObject::GetGameData() const
{
	return m_pGameData;
}

void JoltPhysicsObject::SetGameFlags( unsigned short userFlags )
{
	m_gameFlags = userFlags;
}

unsigned short JoltPhysicsObject::GetGameFlags() const 
{
	return m_gameFlags;
}

void JoltPhysicsObject::SetGameIndex( unsigned short gameIndex )
{
	m_gameIndex = gameIndex;
}

unsigned short JoltPhysicsObject::GetGameIndex() const
{
	return m_gameIndex;
}

//-------------------------------------------------------------------------------------------------

void JoltPhysicsObject::SetCallbackFlags( unsigned short callbackflags )
{
	m_callbackFlags = callbackflags;
}

unsigned short JoltPhysicsObject::GetCallbackFlags() const
{
	return m_callbackFlags;
}

//-------------------------------------------------------------------------------------------------

void JoltPhysicsObject::Wake()
{
	if ( !m_pBody->IsStatic() )
	{
		JPH::BodyInterface& bodyInterface = m_pPhysicsSystem->GetBodyInterfaceNoLock();
		bodyInterface.ActivateBody( m_pBody->GetID() );
	}
	else
	{
		// See other comments in UpdateLayer.
		m_pEnvironment->AddDirtyStaticBody( m_pBody->GetID() );
	}
}

void JoltPhysicsObject::Sleep()
{
	if ( !m_pBody->IsStatic() )
	{
		JPH::BodyInterface& bodyInterface = m_pPhysicsSystem->GetBodyInterfaceNoLock();
		bodyInterface.DeactivateBody( m_pBody->GetID() );
	}
}

void JoltPhysicsObject::RecheckCollisionFilter()
{
	RecheckContactPoints();
}

void JoltPhysicsObject::RecheckContactPoints( bool bSearchForNewContacts /*= false*/ )
{
	JPH::BodyInterface& bodyInterface = m_pPhysicsSystem->GetBodyInterfaceNoLock();
	bodyInterface.InvalidateContactCache( GetBodyID() );

	GetJoltEnvironment()->GetContactListener()->InvalidShouldCollideCache( this );
}

//-------------------------------------------------------------------------------------------------

void JoltPhysicsObject::SetMass( float mass )
{
	mass = Clamp( mass, VPHYSICS_MIN_MASS, VPHYSICS_MAX_MASS );

	m_flCachedMass = mass;
	m_flCachedInvMass = mass ? 1.0f / mass : 0.0f;

	if ( !IsStatic() )
	{
		// Like IVP's change_mass, scale the current inertia with the mass instead of rebuilding it
		// from the shape, so inertia set with SetInertia survives. Mass is already in KG.
		JPH::MotionProperties *pMotionProperties = m_pBody->GetMotionProperties();
		if ( pMotionProperties->GetInverseMassUnchecked() > 0.0f )
			pMotionProperties->ScaleToMass( mass );
		else
		{
			JPH::MassProperties massProperties = m_pBody->GetShape()->GetMassProperties();
			massProperties.ScaleToMass( mass );
			massProperties.mInertia( 3, 3 ) = 1.0f;
			pMotionProperties->SetMassProperties( JPH::EAllowedDOFs::All, massProperties );
		}

		CalculateBuoyancy();
		RecomputeDrag();
	}
}

float JoltPhysicsObject::GetMass() const
{
	return m_flCachedMass;
}

float JoltPhysicsObject::GetInvMass() const
{
	return m_flCachedInvMass;
}

Vector JoltPhysicsObject::GetInertia() const
{
	if ( IsStatic() )
		return Vector( 1.0f, 1.0f, 1.0f );

	JPH::Vec3 joltInertiaTensor = m_pBody->GetMotionProperties()->GetInverseInertiaDiagonal().Reciprocal();
	return Abs( Vector( joltInertiaTensor.GetX(), joltInertiaTensor.GetY(), joltInertiaTensor.GetZ() ) );
}

Vector JoltPhysicsObject::GetInvInertia() const
{
	if ( IsStatic() )
		return Vector( 1.0f, 1.0f, 1.0f );

	JPH::Vec3 joltInertiaTensor = m_pBody->GetMotionProperties()->GetInverseInertiaDiagonal();
	return Abs( Vector( joltInertiaTensor.GetX(), joltInertiaTensor.GetY(), joltInertiaTensor.GetZ() ) );
}

void JoltPhysicsObject::SetInertia( const Vector &inertia )
{
	if ( IsStatic() )
		return;

	Vector ri = Abs( inertia );

	JPH::Vec3 joltInvInertia(
		ri.x > 1e-9f ? 1.0f / ri.x : 0.0f,
		ri.y > 1e-9f ? 1.0f / ri.y : 0.0f,
		ri.z > 1e-9f ? 1.0f / ri.z : 0.0f );

	m_pBody->GetMotionProperties()->SetInverseInertia( joltInvInertia, JPH::Quat::sIdentity() );
}

//-------------------------------------------------------------------------------------------------

void JoltPhysicsObject::SetDamping( const float *speed, const float *rot )
{
	if ( IsStatic() )
		return;

	JPH::MotionProperties* pMotionProperties = m_pBody->GetMotionProperties();
	if ( speed )
		pMotionProperties->SetLinearDamping( *speed );
	if ( rot )
		pMotionProperties->SetAngularDamping( *rot );
}

void JoltPhysicsObject::GetDamping( float *speed, float *rot ) const
{
	if ( IsStatic() )
		return;

	JPH::MotionProperties* pMotionProperties = m_pBody->GetMotionProperties();
	if ( speed )
		*speed = pMotionProperties->GetLinearDamping();
	if ( rot )
		*rot = pMotionProperties->GetAngularDamping();
}

//-------------------------------------------------------------------------------------------------

void JoltPhysicsObject::SetDragCoefficient( float *pDrag, float *pAngularDrag )
{
	if ( pDrag )
		m_flLinearDragCoefficient = *pDrag;

	if ( pAngularDrag )
		m_flAngularDragCoefficient = *pAngularDrag;

	// Source 1 behaviour...
	EnableDrag( m_flLinearDragCoefficient || m_flAngularDragCoefficient );
}

void JoltPhysicsObject::SetBuoyancyRatio( float ratio )
{
	m_flBuoyancyRatio = ratio;
}

//-------------------------------------------------------------------------------------------------

int JoltPhysicsObject::GetMaterialIndex() const
{
	return m_materialIndex;
}

void JoltPhysicsObject::SetMaterialIndex( int materialIndex )
{
	// Gotta clamp it, because we get -1 sometimes
	materialIndex = Max( 0, materialIndex );

	if ( m_materialIndex != materialIndex )
	{
		m_materialIndex = materialIndex;
		UpdateMaterialProperties();
	}
}

//-------------------------------------------------------------------------------------------------

unsigned int JoltPhysicsObject::GetContents() const
{
	return m_contents;
}

void JoltPhysicsObject::SetContents( unsigned int contents )
{
	m_contents = contents;
}

//-------------------------------------------------------------------------------------------------

float JoltPhysicsObject::GetSphereRadius() const
{
	if ( m_pBody->GetShape()->GetSubType() != JPH::EShapeSubType::Sphere )
		return 0.0f;

	const JPH::SphereShape *pSphereShape = static_cast< const JPH::SphereShape * >( m_pBody->GetShape() );
	return JoltToSource::Distance( pSphereShape->GetRadius() );
}

void JoltPhysicsObject::SetSphereRadius( float radius )
{
	if ( m_pBody->GetShape()->GetSubType() != JPH::EShapeSubType::Sphere )
		return;

	// Can't get this shape non-const... urg...
	Log_Stub( LOG_VJolt );
}

float JoltPhysicsObject::GetEnergy() const
{
	// 1/2 * mv^2
	const float flKineticEnergy = 0.5f * m_flCachedMass * m_pBody->GetLinearVelocity().LengthSq();
	// TODO(Josh): We need to factor in inertia or something here to get this right.
	// as this AngularVelocity is in rads/s...
	// I guess it's a good enough approximation for now.
	// Right now the equation is 1/2ww where we probably want 1/2wIw.
	// Intertia in Jolt is weird... Not sure what's going on with its matrix thingy.
	const float flAngularEnergy = 0.5f * m_flCachedMass * m_pBody->GetAngularVelocity().LengthSq();

	return JoltToSource::Energy( flKineticEnergy + flAngularEnergy );
}

Vector JoltPhysicsObject::GetMassCenterLocalSpace() const
{
	return JoltToSource::Distance( m_pBody->GetShape()->GetCenterOfMass() );
}

//-------------------------------------------------------------------------------------------------

void JoltPhysicsObject::SetPosition( const Vector &worldPosition, const QAngle &angles, bool isTeleport )
{
	if ( vjolt_object_debug.GetBool() )
	{
		Vector currentPos;
		GetPosition( &currentPos, nullptr );
		Vector delta = worldPosition - currentPos;
		Log_Msg( LOG_VJolt, "SetPosition: target=(%.1f,%.1f,%.1f) delta=%.2f teleport=%d\n",
			worldPosition.x, worldPosition.y, worldPosition.z, delta.Length(), int( isTeleport ) );
	}

	JPH::Vec3 joltPosition = SourceToJolt::Distance( worldPosition );
	JPH::Quat joltRotation = SourceToJolt::Angle( angles );

	JPH::BodyInterface &bodyInterface = m_pPhysicsSystem->GetBodyInterfaceNoLock();

	bodyInterface.SetPositionAndRotation( m_pBody->GetID(), joltPosition, joltRotation, JPH::EActivation::DontActivate );

	if ( isTeleport || IsStatic() )
	{
		m_vLastPosition = worldPosition;
		m_qLastOrientation = angles;
	}
}

void JoltPhysicsObject::SetPositionMatrix( const matrix3x4_t &matrix, bool isTeleport )
{
	SetPosition( GetColumn( matrix, MatrixAxis::Origin ), ToQAngle( matrix ), isTeleport );
}

//-------------------------------------------------------------------------------------------------

void JoltPhysicsObject::GetPosition( Vector *worldPosition, QAngle *angles ) const
{
	JPH::BodyInterface &bodyInterface = m_pPhysicsSystem->GetBodyInterfaceNoLock();

	JPH::Vec3 joltPosition;
	JPH::Quat joltRotation;
	bodyInterface.GetPositionAndRotation( m_pBody->GetID(), joltPosition, joltRotation );

	if ( worldPosition )
		*worldPosition = JoltToSource::Distance( joltPosition );

	if ( angles )
		*angles = JoltToSource::Angle( joltRotation );
}

void JoltPhysicsObject::GetPositionMatrix( matrix3x4_t *positionMatrix ) const
{
	matrix3x4_t matrix;
	SetIdentityMatrix( matrix );
	AngleMatrix( JoltToSource::Angle( m_pBody->GetRotation() ), JoltToSource::Distance( m_pBody->GetPosition() ), matrix );
	*positionMatrix = matrix;
}

void JoltPhysicsObject::SetVelocity( const Vector *velocity, const AngularImpulse *angularVelocity )
{
	if ( vjolt_object_debug.GetBool() )
	{
		Log_Msg( LOG_VJolt, "SetVelocity: v=%s(%.1f,%.1f,%.1f) w=%s(%.1f,%.1f,%.1f)\n",
			velocity ? "" : "NULL ",
			velocity ? velocity->x : 0.0f, velocity ? velocity->y : 0.0f, velocity ? velocity->z : 0.0f,
			angularVelocity ? "" : "NULL ",
			angularVelocity ? angularVelocity->x : 0.0f, angularVelocity ? angularVelocity->y : 0.0f, angularVelocity ? angularVelocity->z : 0.0f );
	}

	JPH::Vec3 joltLinearVelocity = velocity ? SourceToJolt::Distance( *velocity ) : JPH::Vec3{};
	JPH::Vec3 joltAngularVelocity = JPH::Vec3{};
	if ( angularVelocity )
	{
		Vector worldAng;
		LocalToWorldVector( &worldAng, *angularVelocity );
		joltAngularVelocity = SourceToJolt::AngularImpulse( worldAng );
	}

	JPH::BodyInterface &bodyInterface = m_pPhysicsSystem->GetBodyInterfaceNoLock();

	if ( velocity && angularVelocity )
		bodyInterface.SetLinearAndAngularVelocity( m_pBody->GetID(), joltLinearVelocity, joltAngularVelocity );
	else if ( velocity )
		bodyInterface.SetLinearVelocity( m_pBody->GetID(), joltLinearVelocity );
	else if ( angularVelocity )
		bodyInterface.SetAngularVelocity( m_pBody->GetID(), joltAngularVelocity );
}

//-------------------------------------------------------------------------------------------------

void JoltPhysicsObject::SetVelocityInstantaneous( const Vector *velocity, const AngularImpulse *angularVelocity )
{
	SetVelocity( velocity, angularVelocity );
}

//-------------------------------------------------------------------------------------------------

void JoltPhysicsObject::GetVelocity( Vector *velocity, AngularImpulse *angularVelocity ) const
{
	JPH::BodyInterface &bodyInterface = m_pPhysicsSystem->GetBodyInterfaceNoLock();

	JPH::Vec3 joltLinearVelocity;
	JPH::Vec3 joltAngularVelocity;
	bodyInterface.GetLinearAndAngularVelocity( m_pBody->GetID(), joltLinearVelocity, joltAngularVelocity );

	if ( velocity )
		*velocity = JoltToSource::Distance( joltLinearVelocity );

	if ( angularVelocity )
	{
		Vector worldAng = JoltToSource::AngularImpulse( joltAngularVelocity );
		WorldToLocalVector( angularVelocity, worldAng );
	}
}

//-------------------------------------------------------------------------------------------------

void JoltPhysicsObject::AddVelocity( const Vector *velocity, const AngularImpulse *angularVelocity )
{
	if ( !IsMoveable() )
		return;

	if ( vjolt_object_debug.GetBool() )
		Log_Msg( LOG_VJolt, "AddVelocity: |v|=%.1f |w|=%.1f\n",
			velocity ? velocity->Length() : 0.0f,
			angularVelocity ? angularVelocity->Length() : 0.0f );

	// Do this longer method do set velocity and angular velocity
	// in the same lock.
	const JPH::BodyLockInterfaceNoLock &bodyLockInterface = m_pPhysicsSystem->GetBodyLockInterfaceNoLock();

	JPH::BodyLockWrite lock( bodyLockInterface, m_pBody->GetID() );
	if ( lock.Succeeded() )
	{
		JPH::Body &body = lock.GetBody();

		if ( velocity )
			body.SetLinearVelocityClamped( body.GetLinearVelocity() + SourceToJolt::Distance( *velocity ) );

		if ( angularVelocity )
		{
			Vector worldAng;
			LocalToWorldVector( &worldAng, *angularVelocity );
			body.SetAngularVelocityClamped( body.GetAngularVelocity() + SourceToJolt::AngularImpulse( worldAng ) );
		}

		if ( !body.IsActive() && ( !body.GetLinearVelocity().IsNearZero() || !body.GetAngularVelocity().IsNearZero() ) )
			m_pPhysicsSystem->GetBodyInterfaceNoLock().ActivateBodies( &m_pBody->GetID(), 1 );
	}
}

void JoltPhysicsObject::GetVelocityAtPoint( const Vector &worldPosition, Vector *pVelocity ) const
{
	VJoltAssert( pVelocity );

	*pVelocity = JoltToSource::Distance( m_pPhysicsSystem->GetBodyInterfaceNoLock().GetPointVelocity( m_pBody->GetID(), SourceToJolt::Distance( worldPosition ) ) );
}

void JoltPhysicsObject::GetImplicitVelocity( Vector *velocity, AngularImpulse *angularVelocity ) const
{
	if ( velocity )
		*velocity = m_vLastVelocity;

	if ( angularVelocity )
		*angularVelocity = m_vLastAngularVelocity;
}

void JoltPhysicsObject::LocalToWorld( Vector *worldPosition, const Vector &localPosition ) const
{
	matrix3x4_t matrix;
	GetPositionMatrix( &matrix );
	// Copy in case src == dest
	VectorTransform( Vector( localPosition ), matrix, *worldPosition );
}

void JoltPhysicsObject::WorldToLocal( Vector *localPosition, const Vector &worldPosition ) const
{
	matrix3x4_t matrix;
	GetPositionMatrix( &matrix );
	// Copy in case src == dest
	VectorITransform( Vector( worldPosition ), matrix, *localPosition );
}

//-------------------------------------------------------------------------------------------------

void JoltPhysicsObject::LocalToWorldVector( Vector *worldVector, const Vector &localVector ) const
{
	matrix3x4_t matrix;
	GetPositionMatrix( &matrix );
	// Copy in case src == dest
	VectorRotate( Vector( localVector ), matrix, *worldVector );
}

void JoltPhysicsObject::WorldToLocalVector( Vector *localVector, const Vector &worldVector ) const
{
	matrix3x4_t matrix;
	GetPositionMatrix( &matrix );
	// Copy in case src == dest
	VectorIRotate( Vector( worldVector ), matrix, *localVector );
}

//-------------------------------------------------------------------------------------------------

void JoltPhysicsObject::ApplyForceCenter( const Vector &forceVector )
{
	if ( !IsMoveable() )
		return;

	if ( vjolt_object_debug.GetBool() )
		Log_Msg( LOG_VJolt, "ApplyForceCenter: |F|=%.1f mass=%.1f\n", forceVector.Length(), GetMass() );

	JPH::BodyInterface &bodyInterface = m_pPhysicsSystem->GetBodyInterfaceNoLock();
	bodyInterface.AddImpulse( m_pBody->GetID(), SourceToJolt::Distance( forceVector ) );
}

void JoltPhysicsObject::ApplyForceOffset( const Vector &forceVector, const Vector &worldPosition )
{
	if ( !IsMoveable() )
		return;

	if ( vjolt_object_debug.GetBool() )
		Log_Msg( LOG_VJolt, "ApplyForceOffset: |F|=%.1f mass=%.1f\n", forceVector.Length(), GetMass() );

	JPH::Vec3 impulse = SourceToJolt::Distance( forceVector );
	JPH::Vec3 point = SourceToJolt::Distance( worldPosition );

	JPH::BodyInterface &bodyInterface = m_pPhysicsSystem->GetBodyInterfaceNoLock();
	bodyInterface.AddImpulse( m_pBody->GetID(), impulse, point );
}

void JoltPhysicsObject::ApplyTorqueCenter( const AngularImpulse &torque )
{
	if ( !IsMoveable() )
		return;

	if ( vjolt_object_debug.GetBool() )
		Log_Msg( LOG_VJolt, "ApplyTorqueCenter: |T|=%.1f\n", torque.Length() );

	// Do this longer method do set velocity and angular velocity
	// in the same lock.
	const JPH::BodyLockInterfaceNoLock &bodyLockInterface = m_pPhysicsSystem->GetBodyLockInterfaceNoLock();

	JPH::BodyLockWrite lock( bodyLockInterface, m_pBody->GetID() );
	if ( lock.Succeeded() )
	{
		JPH::Body &body = lock.GetBody();

		body.AddAngularImpulse( SourceToJolt::AngularImpulse( torque ) );

		if ( !body.IsActive() && ( !body.GetAngularVelocity().IsNearZero() ) )
			m_pPhysicsSystem->GetBodyInterfaceNoLock().ActivateBodies( &m_pBody->GetID(), 1 );
	}
}

//-------------------------------------------------------------------------------------------------

void JoltPhysicsObject::CalculateForceOffset( const Vector &forceVector, const Vector &worldPosition, Vector *centerForce, AngularImpulse *centerTorque ) const
{
	JPH::Vec3 pos = SourceToJolt::Distance( worldPosition );
	JPH::Vec3 force = SourceToJolt::Distance( forceVector );

	JPH::Vec3 com = pos - m_pBody->GetCenterOfMassPosition();
	JPH::Vec3 cross = com.Cross( force );

	if ( centerForce )
		*centerForce = JoltToSource::Distance( force );

	if ( centerTorque )
	{
		Vector worldTorque = JoltToSource::AngularImpulse( cross );
		WorldToLocalVector( centerTorque, worldTorque );
	}
}

void JoltPhysicsObject::CalculateVelocityOffset( const Vector &forceVector, const Vector &worldPosition, Vector *centerVelocity, AngularImpulse *centerAngularVelocity ) const
{
	// Convert force to SI units to multiply by mass for impulse.
	JPH::Vec3 siForce = SourceToJolt::Distance( forceVector );

	if ( centerVelocity )
		*centerVelocity = JoltToSource::Distance( siForce * m_flCachedInvMass );

	if ( centerAngularVelocity )
	{
		JPH::Vec3 siPosition = SourceToJolt::Distance( worldPosition );

		JPH::Vec3 siRelativePosition = siPosition - m_pBody->GetCenterOfMassPosition();
		JPH::Vec3 cross = siRelativePosition.Cross( siForce );

		Vector worldAngVel = JoltToSource::AngularImpulse( m_pBody->GetInverseInertia() * cross );
		WorldToLocalVector( centerAngularVelocity, worldAngVel );
	}
}

float JoltPhysicsObject::CalculateLinearDrag( const Vector &unitDirection ) const
{
	return GetDragInDirection( SourceToJolt::Unitless( unitDirection ) );
}

float JoltPhysicsObject::CalculateAngularDrag( const Vector &objectSpaceRotationAxis ) const
{
	// Drag factor is per-radian internally; Source 1 callers expect per-degree.
	return GetAngularDragInDirection( SourceToJolt::Unitless( objectSpaceRotationAxis ) ) * DEG2RAD( 1.0f );
}

//-------------------------------------------------------------------------------------------------

bool JoltPhysicsObject::GetContactPoint( Vector *contactPoint, IPhysicsObject **contactObject ) const
{
	Log_Stub( LOG_VJolt );
	return false;
}

//-------------------------------------------------------------------------------------------------

void JoltPhysicsObject::SetShadow( float maxSpeed, float maxAngularSpeed, bool allowPhysicsMovement, bool allowPhysicsRotation )
{
	if ( m_pShadowController )
	{
		m_pShadowController->MaxSpeed( maxSpeed, maxAngularSpeed );
	}
	else
	{
		m_bShadowTemporarilyDisableGravity = false;

		m_pShadowController = static_cast<JoltPhysicsShadowController *>( m_pEnvironment->CreateShadowController( this, allowPhysicsMovement, allowPhysicsRotation ) );
		m_pShadowController->MaxSpeed( maxSpeed, maxAngularSpeed );
	}
}

void JoltPhysicsObject::UpdateShadow( const Vector &targetPosition, const QAngle &targetAngles, bool tempDisableGravity, float timeOffset )
{
	if ( m_pShadowController )
	{
		if ( tempDisableGravity != m_bShadowTemporarilyDisableGravity )
		{
			m_bShadowTemporarilyDisableGravity = tempDisableGravity;
			if ( m_pShadowController->AllowsTranslation() )
				EnableGravity( !m_bShadowTemporarilyDisableGravity );
		}
		m_pShadowController->Update( targetPosition, targetAngles, timeOffset );
	}
}

//-------------------------------------------------------------------------------------------------

int JoltPhysicsObject::GetShadowPosition( Vector *position, QAngle *angles ) const
{
	GetPosition( position, angles );
	return 1;
}

IPhysicsShadowController *JoltPhysicsObject::GetShadowController() const
{
	return m_pShadowController;
}

void JoltPhysicsObject::RemoveShadowController()
{
	if ( m_pShadowController )
	{
		m_pEnvironment->DestroyShadowController(m_pShadowController);
		m_pShadowController = nullptr;
	}
}

static void ComputeController( JPH::Vec3 &vecCurrentVelocity, const JPH::Vec3 &vecDeltaPos, float flMaxSpeed, float flMaxDampSpeed, float flScaleDelta, float flDamping, JPH::Vec3 *pOutImpulse = nullptr )
{
	float flCurrentSpeedSq = vecCurrentVelocity.LengthSq();
	if ( flCurrentSpeedSq < 1e-6f )
	{
		vecCurrentVelocity = JPH::Vec3::sZero();
	}
	else if ( flMaxDampSpeed > 0 )
	{
		JPH::Vec3 vecAccelDampening = vecCurrentVelocity * -flDamping;
		float flSpeed = sqrtf( flCurrentSpeedSq ) * fabsf( flDamping );
		if ( flSpeed > flMaxDampSpeed )
		{
			flSpeed = flMaxDampSpeed / flSpeed;
			vecAccelDampening *= flSpeed;
		}
		vecCurrentVelocity += vecAccelDampening;
	}

	JPH::Vec3 vecAcceleration = JPH::Vec3::sZero();
	if ( flMaxSpeed > 0.0f )
	{
		vecAcceleration = vecDeltaPos * flScaleDelta;
		float flSpeed = vecDeltaPos.Length() * flScaleDelta;
		if ( flSpeed > flMaxSpeed )
		{
			flSpeed = flMaxSpeed / flSpeed;
			vecAcceleration *= flSpeed;
		}
		vecCurrentVelocity += vecAcceleration;
	}

	if ( pOutImpulse )
		*pOutImpulse = vecAcceleration;
}

// hlshadowcontrol_params_t but in Jolt space.
struct JoltShadowControlParams
{
	JPH::Vec3			TargetPosition;
	JPH::Quat			TargetRotation;
	JPH::Vec3			LastPosition;
	JPH::Vec3			LastImpulse;
	float				MaxAngular;
	float				MaxDampAngular;
	float				MaxSpeed;
	float				MaxDampSpeed;
	float				DampFactor;
	float				TeleportDistance;
};

static float ComputeShadowController( JoltShadowControlParams &params, JPH::Vec3 &position, JPH::Quat &rotation, JPH::Vec3 &linearVelocity, JPH::Vec3& angularVelocity, float flSecondsToArrival, float flDeltaTime )
{
	const float flFraction = flSecondsToArrival > 0.0f
		? Min( flDeltaTime / flSecondsToArrival, 1.0f )
		: 1.0f;

	flSecondsToArrival = Max( flSecondsToArrival - flDeltaTime, 0.0f );

	if ( flFraction <= 0.0f )
		return flSecondsToArrival;

	JPH::Vec3 deltaPosition = params.TargetPosition - position;

	if ( params.TeleportDistance > 0.0f && deltaPosition.LengthSq() > Square( params.TeleportDistance ) )
	{
		position = params.TargetPosition;
		rotation = params.TargetRotation;
		deltaPosition = JPH::Vec3::sZero();
	}

	const float flInvDeltaTime = 1.0f / flDeltaTime;
	const float flFractionTime = flFraction * flInvDeltaTime;

	ComputeController( linearVelocity, deltaPosition, params.MaxSpeed, params.MaxDampSpeed, flFractionTime, params.DampFactor, &params.LastImpulse);

	params.LastPosition = position + linearVelocity * flDeltaTime;

	JPH::Quat deltaRotation = params.TargetRotation * rotation.Inversed();
	
	JPH::Vec3 axis;
	float angle;
	deltaRotation.GetAxisAngle( axis, angle );
	
	JPH::Vec3 deltaAngles = axis * angle;
	ComputeController( angularVelocity, deltaAngles, params.MaxAngular, params.MaxDampAngular, flFractionTime, params.DampFactor );

	return flSecondsToArrival;
}


namespace
{
	// A body touching (or within kContactDistance of) the query shape, and the direction from
	// the query shape towards it.
	struct ShadowContact
	{
		JPH::BodyID BodyID;
		JPH::Vec3 Normal;
		bool bStatic;
	};

	static constexpr float kShadowContactDistance = 0.025f; // ~1 inch
	static constexpr int kShadowMaxContacts = 32;
	// How many dynamic bodies a push may travel through before reaching the world
	static constexpr int kShadowMaxChainDepth = 4;
	// cos(45 degrees): how closely a support contact must line up with the push
	static constexpr float kShadowSupportCos = 0.7071f;

	class ShadowContactCollector final : public JPH::CollideShapeCollector
	{
	public:
		ShadowContactCollector( JPH::PhysicsSystem *pSystem, JoltPhysicsContactListener *pListener, JoltPhysicsObject *pSelf )
			: m_pSystem( pSystem ), m_pListener( pListener ), m_pSelf( pSelf ) {}

		void AddHit( const JPH::CollideShapeResult &inResult ) override
		{
			if ( m_nContacts >= kShadowMaxContacts )
			{
				ForceEarlyOut();
				return;
			}

			JPH::BodyLockRead lock( m_pSystem->GetBodyLockInterfaceNoLock(), inResult.mBodyID2 );
			if ( !lock.Succeeded() )
				return;

			const JPH::Body &body = lock.GetBody();
			if ( body.IsSensor() )
				return;

			// Honor the game-side collision filter so we don't clamp against bodies that the game
			// wouldn't actually collide with (e.g. NPC-clip brushes, which are static but pass
			// through physics props).
			JoltPhysicsObject *pOther = reinterpret_cast< JoltPhysicsObject * >( body.GetUserData() );
			if ( !pOther || pOther == m_pSelf )
				return;
			if ( m_pListener && !m_pListener->ShouldCollide( m_pSelf, pOther ) )
				return;

			m_Contacts[ m_nContacts++ ] = ShadowContact{
				inResult.mBodyID2,
				inResult.mPenetrationAxis.NormalizedOr( JPH::Vec3::sAxisZ() ),
				body.IsStatic() || body.GetMotionType() != JPH::EMotionType::Dynamic,
			};
		}

		ShadowContact m_Contacts[ kShadowMaxContacts ];
		int m_nContacts = 0;

	private:
		JPH::PhysicsSystem *m_pSystem;
		JoltPhysicsContactListener *m_pListener;
		JoltPhysicsObject *m_pSelf;
	};

	class ExcludeBodiesFilter final : public JPH::BodyFilter
	{
	public:
		ExcludeBodiesFilter( const JPH::BodyID *pExcluded, int nExcluded ) : m_pExcluded( pExcluded ), m_nExcluded( nExcluded ) {}

		bool ShouldCollide( const JPH::BodyID &inBodyID ) const override
		{
			for ( int i = 0; i < m_nExcluded; i++ )
			{
				if ( m_pExcluded[ i ] == inBodyID )
					return false;
			}
			return true;
		}

	private:
		const JPH::BodyID *m_pExcluded;
		int m_nExcluded;
	};

	// Collects the bodies touching a shape at the given transform.
	void CollectShadowContacts( JPH::PhysicsSystem *pSystem, JoltPhysicsObject *pSelf, const JPH::Shape *pShape,
		JPH::Mat44Arg matCOMTransform, JPH::Vec3Arg vMovementDirection,
		const JPH::BodyID *pExcluded, int nExcluded, ShadowContactCollector &collector )
	{
		JPH::CollideShapeSettings settings;
		settings.mActiveEdgeMode = JPH::EActiveEdgeMode::CollideOnlyWithActive;
		settings.mActiveEdgeMovementDirection = vMovementDirection;
		settings.mBackFaceMode = JPH::EBackFaceMode::IgnoreBackFaces;
		settings.mMaxSeparationDistance = kShadowContactDistance;

		ExcludeBodiesFilter filter( pExcluded, nExcluded );
		JPH::DefaultBroadPhaseLayerFilter bpFilter = pSystem->GetDefaultBroadPhaseLayerFilter( Layers::MOVING );
		JPH::DefaultObjectLayerFilter olFilter = pSystem->GetDefaultLayerFilter( Layers::MOVING );

		pSystem->GetNarrowPhaseQueryNoLock().CollideShape(
			pShape, JPH::Vec3::sReplicate( 1.0f ), matCOMTransform, settings, JPH::Vec3::sZero(),
			collector, bpFilter, olFilter, filter );
	}

	// Whether pushing this dynamic body along vDirection is resisted by static geometry, either
	// directly or through a chain of other dynamic bodies (a prop pressed onto a prop on the floor).
	// outSupportNormal is the normal of the static contact that holds the chain, pointing into it.
	bool IsBodyBlockedAlong( JPH::PhysicsSystem *pSystem, JoltPhysicsContactListener *pListener, const JPH::BodyID &bodyID,
		JPH::Vec3Arg vDirection, JPH::BodyID *pVisited, int nVisited, int nDepth, JPH::Vec3 &outSupportNormal )
	{
		JoltPhysicsObject *pObject = nullptr;
		JPH::RefConst< JPH::Shape > pShape;
		JPH::Mat44 matCOMTransform;
		{
			JPH::BodyLockRead lock( pSystem->GetBodyLockInterfaceNoLock(), bodyID );
			if ( !lock.Succeeded() )
				return false;
			pObject = reinterpret_cast< JoltPhysicsObject * >( lock.GetBody().GetUserData() );
			pShape = lock.GetBody().GetShape();
			matCOMTransform = lock.GetBody().GetCenterOfMassTransform();
		}
		if ( !pObject )
			return false;

		pVisited[ nVisited++ ] = bodyID;

		ShadowContactCollector collector( pSystem, pListener, pObject );
		CollectShadowContacts( pSystem, pObject, pShape, matCOMTransform, vDirection, pVisited, nVisited, collector );

		for ( int i = 0; i < collector.m_nContacts; i++ )
		{
			const ShadowContact &contact = collector.m_Contacts[ i ];
			if ( contact.Normal.Dot( vDirection ) < kShadowSupportCos )
				continue;

			if ( contact.bStatic )
			{
				outSupportNormal = contact.Normal;
				return true;
			}

			if ( nDepth > 0 && IsBodyBlockedAlong( pSystem, pListener, contact.BodyID, vDirection, pVisited, nVisited, nDepth - 1, outSupportNormal ) )
				return true;
		}

		return false;
	}
}

void JoltPhysicsObject::ClampShadowVelocityAgainstContacts( JPH::Vec3 &ioVelocity, const JPH::Vec3 &vBodyPos, const JPH::Quat &qBodyRot )
{
	JoltPhysicsContactListener *pListener = m_pEnvironment->GetContactListener();

	const JPH::Mat44 matCOMTransform = JPH::Mat44::sRotationTranslation(
		qBodyRot, vBodyPos + qBodyRot * m_pBody->GetShape()->GetCenterOfMass() );

	JPH::BodyID visited[ kShadowMaxChainDepth + 2 ];
	visited[ 0 ] = m_pBody->GetID();

	ShadowContactCollector collector( m_pPhysicsSystem, pListener, this );
	CollectShadowContacts( m_pPhysicsSystem, this, m_pBody->GetShape(), matCOMTransform, ioVelocity, visited, 1, collector );

	for ( int i = 0; i < collector.m_nContacts; i++ )
	{
		const ShadowContact &contact = collector.m_Contacts[ i ];
		const float flInto = ioVelocity.Dot( contact.Normal );
		if ( flInto <= 0.0f )
			continue;

		// Static contacts always block. Dynamic ones block when the body is pinned against the
		// world in this direction, otherwise we'd push it into (and eventually through) the world
		// with the controller's near-unlimited force. Pushing it is fine.
		if ( contact.bStatic )
		{
			ioVelocity -= contact.Normal * flInto;
			continue;
		}

		JPH::Vec3 vSupportNormal;
		if ( IsBodyBlockedAlong( m_pPhysicsSystem, pListener, contact.BodyID, contact.Normal, visited, 1, kShadowMaxChainDepth, vSupportNormal ) )
		{
			ioVelocity -= contact.Normal * flInto;

			// A slanted contact (pressing on the edge of a barrel) would still pass part of the
			// push on towards the world, so don't push towards the support either.
			const float flIntoSupport = ioVelocity.Dot( vSupportNormal );
			if ( flIntoSupport > 0.0f )
				ioVelocity -= vSupportNormal * flIntoSupport;
		}
	}
}

float JoltPhysicsObject::ComputeShadowControl( const hlshadowcontrol_params_t &params, float flSecondsToArrival, float flDeltaTime )
{

	JoltShadowControlParams joltParams =
	{
		.TargetPosition		= SourceToJolt::Distance( params.targetPosition ),
		.TargetRotation		= SourceToJolt::Angle( params.targetRotation ),
		.MaxAngular			= SourceToJolt::Angle( params.maxAngular ),
		.MaxDampAngular		= SourceToJolt::Angle( params.maxDampAngular ),
		.MaxSpeed			= SourceToJolt::Distance( params.maxSpeed ),
		.MaxDampSpeed		= SourceToJolt::Distance( params.maxDampSpeed ),
		.DampFactor			= params.dampFactor,
		.TeleportDistance	= SourceToJolt::Distance( params.teleportDistance ),
	};

	JPH::BodyInterface& bodyInterface = m_pPhysicsSystem->GetBodyInterfaceNoLock();

	JPH::Vec3 position;
	JPH::Quat rotation;
	bodyInterface.GetPositionAndRotation( m_pBody->GetID(), position, rotation );
	JPH::Vec3 linearVelocity;
	JPH::Vec3 angularVelocity;
	bodyInterface.GetLinearAndAngularVelocity( m_pBody->GetID(), linearVelocity, angularVelocity );

	JPH::Vec3 scratchPosition = position;
	JPH::Quat scratchRotation = rotation;
	JPH::Vec3 scratchLinearVelocity = linearVelocity;
	JPH::Vec3 scratchAngularVelocity = angularVelocity;
	float flNewSecondsToArrival =
		ComputeShadowController( joltParams, scratchPosition, scratchRotation, scratchLinearVelocity, scratchAngularVelocity, flSecondsToArrival, flDeltaTime );

	// IVP's shadow controller writes into pCore->speed which then participates in
	// IVP's run-to-convergence contact resolution, implicitly clamping the velocity
	// at any contact. Jolt's fixed-iteration solver can't fully clamp a hard-set
	// velocity vector that points into a wall under sustained drive, which lets the
	// physgun-held object (and anything it pushes) pass through walls. Pre-clamp the
	// target velocity along blocked-direction contact normals before SetLinearAndAngularVelocity
	// so the body isn't asked to move into a constrained direction in the first place.
	//
	// Static contacts always block. Dynamic contacts block if that body is itself pinned
	// against static geometry along the same direction, possibly through a few more bodies
	// (handles "push A down onto B sitting on the ground").
	if ( !scratchLinearVelocity.IsNearZero() )
	{
		ClampShadowVelocityAgainstContacts( scratchLinearVelocity, scratchPosition, scratchRotation );
	}

	if ( scratchPosition != position || scratchRotation != rotation )
		bodyInterface.SetPositionAndRotation( m_pBody->GetID(), scratchPosition, scratchRotation, JPH::EActivation::Activate );

	if ( scratchLinearVelocity != linearVelocity || scratchAngularVelocity != angularVelocity )
		bodyInterface.SetLinearAndAngularVelocity( m_pBody->GetID(), scratchLinearVelocity, scratchAngularVelocity );

	return flNewSecondsToArrival;
}

//-------------------------------------------------------------------------------------------------

const CPhysCollide *JoltPhysicsObject::GetCollide() const
{
	const CPhysCollide *pCollide = CPhysCollide::FromShape( m_pBody->GetShape() );
	return pCollide;
}

const char *JoltPhysicsObject::GetName() const
{
	return m_pName;
}

//-------------------------------------------------------------------------------------------------

void JoltPhysicsObject::BecomeTrigger()
{
	if ( IsTrigger() )
		return;

	EnableDrag( false );
	EnableGravity( false );

	m_pBody->SetIsSensor( true );
}

void JoltPhysicsObject::RemoveTrigger()
{
	if ( !IsTrigger() )
		return;

	// Note: Does not change drag/gravity state back, to mimick Source 1 behaviour.

	// Josh:
	// All this logic below is to trigger ObjectLeaveTrigger
	// when the trigger is deleted.
	IPhysicsCollisionEvent *pEventListener = m_pEnvironment->GetContactListener()->GetGameListener();
	if ( pEventListener && IsTrigger() )
	{
		class SourceTriggerCollector : public JPH::CollideShapeCollector
		{
		public:
			SourceTriggerCollector( JPH::PhysicsSystem *pPhysicsSystem, IPhysicsCollisionEvent *pGameListener, JoltPhysicsObject *pTrigger )
				: m_pPhysicsSystem( pPhysicsSystem )
				, m_pGameListener ( pGameListener )
				, m_pTrigger      ( pTrigger ) {}

			void AddHit( const ResultType &inResult ) override
			{
				const JPH::BodyID inBodyID = inResult.mBodyID2;

				JPH::BodyLockWrite lock( m_pPhysicsSystem->GetBodyLockInterface(), inBodyID );
				JPH::Body &body = lock.GetBody();
				JoltPhysicsObject *pObject = reinterpret_cast<JoltPhysicsObject *>( body.GetUserData() );

				if ( !pObject )
					return;

				m_pGameListener->ObjectLeaveTrigger( m_pTrigger, pObject );
			}

		private:
			JPH::PhysicsSystem     *m_pPhysicsSystem;
			IPhysicsCollisionEvent *m_pGameListener;
			JoltPhysicsObject      *m_pTrigger;
		};

		SourceTriggerCollector collector( m_pPhysicsSystem, pEventListener, this );

		JPH::IgnoreSingleBodyFilter body_filter( GetBodyID() );

		JPH::CollideShapeSettings collideSettings;
		collideSettings.mActiveEdgeMode = JPH::EActiveEdgeMode::CollideWithAll;

		JPH::BodyInterface &bodyInterface = m_pPhysicsSystem->GetBodyInterfaceNoLock();
		JPH::Mat44 queryTransform = bodyInterface.GetCenterOfMassTransform( GetBodyID() );

		const JPH::Shape *pShape = GetCollide()->ToShape();

		m_pPhysicsSystem->GetNarrowPhaseQueryNoLock().CollideShape(
			pShape, JPH::Vec3::sReplicate( 1.0f ), queryTransform, collideSettings, JPH::Vec3::sZero(), collector,
			JPH::SpecifiedBroadPhaseLayerFilter( BroadPhaseLayers::MOVING ), JPH::SpecifiedObjectLayerFilter( Layers::MOVING ), body_filter );
	}

	m_pBody->SetIsSensor( false );
}

//-------------------------------------------------------------------------------------------------

void JoltPhysicsObject::BecomeHinged( int localAxis )
{
	Log_Stub( LOG_VJolt );
}

void JoltPhysicsObject::RemoveHinged()
{
	Log_Stub( LOG_VJolt );
}

//-------------------------------------------------------------------------------------------------

IPhysicsFrictionSnapshot *JoltPhysicsObject::CreateFrictionSnapshot()
{
	return new JoltPhysicsFrictionSnapshot( this );
}

void JoltPhysicsObject::DestroyFrictionSnapshot( IPhysicsFrictionSnapshot *pSnapshot )
{
	delete pSnapshot;
}

//-------------------------------------------------------------------------------------------------

namespace
{
	std::atomic< uint32 > g_nContactImpulseTick{ 0 };
}

void JoltPhysicsObject::AdvanceContactImpulseTick()
{
	g_nContactImpulseTick.fetch_add( 1, std::memory_order_relaxed );
}

// Caller MUST hold m_LastContactImpulsesLock.
static inline void TickStampedClear( uint32 &nLastTick, ankerl::unordered_dense::map< JoltPhysicsObject *, JoltPhysicsObject::ContactImpulse > &map )
{
	const uint32 nNow = g_nContactImpulseTick.load( std::memory_order_relaxed );
	if ( nLastTick != nNow )
	{
		map.clear();
		nLastTick = nNow;
	}
}

void JoltPhysicsObject::AccumulateContactNormalImpulse( JoltPhysicsObject *pOther, float flImpulse )
{
	std::lock_guard< std::mutex > lock( m_LastContactImpulsesLock );
	TickStampedClear( m_nLastImpulseTick, m_LastContactImpulses );
	m_LastContactImpulses[ pOther ].flNormalImpulse += flImpulse;
}

void JoltPhysicsObject::AccumulateContactFrictionEnergy( JoltPhysicsObject *pOther, float flEnergy )
{
	std::lock_guard< std::mutex > lock( m_LastContactImpulsesLock );
	TickStampedClear( m_nLastImpulseTick, m_LastContactImpulses );
	m_LastContactImpulses[ pOther ].flFrictionEnergy += flEnergy;
}

float JoltPhysicsObject::GetLastContactNormalImpulse( JoltPhysicsObject *pOther ) const
{
	std::lock_guard< std::mutex > lock( m_LastContactImpulsesLock );
	// Stale-tick entries don't get cleared on read (to avoid race with concurrent
	// accumulators), but they're also never queried for currently-in-contact bodies
	// (those are refreshed each tick). For non-current bodies the value is irrelevant.
	auto it = m_LastContactImpulses.find( pOther );
	return it != m_LastContactImpulses.end() ? it->second.flNormalImpulse : 0.0f;
}

float JoltPhysicsObject::GetLastContactFrictionEnergy( JoltPhysicsObject *pOther ) const
{
	std::lock_guard< std::mutex > lock( m_LastContactImpulsesLock );
	auto it = m_LastContactImpulses.find( pOther );
	return it != m_LastContactImpulses.end() ? it->second.flFrictionEnergy : 0.0f;
}

void JoltPhysicsObject::ClearContactImpulsesFor( JoltPhysicsObject *pOther )
{
	std::lock_guard< std::mutex > lock( m_LastContactImpulsesLock );
	m_LastContactImpulses.erase( pOther );
}

//-------------------------------------------------------------------------------------------------

void JoltPhysicsObject::OutputDebugInfo() const
{
	Log_Stub( LOG_VJolt );
}

//-------------------------------------------------------------------------------------------------

void JoltPhysicsObject::SetUseAlternateGravity( bool bSet )
{
	Log_Stub( LOG_VJolt );
}

void JoltPhysicsObject::SetCollisionHints( uint32 collisionHints )
{
	m_collisionHints = collisionHints;
	UpdateLayer();
}

uint32 JoltPhysicsObject::GetCollisionHints() const
{
	return m_collisionHints;
}

//-------------------------------------------------------------------------------------------------

IPredictedPhysicsObject *JoltPhysicsObject::GetPredictedInterface() const
{
	Log_Stub( LOG_VJolt );
	return nullptr;
}

void JoltPhysicsObject::SyncWith( IPhysicsObject *pOther )
{
	if ( this->IsCollisionEnabled() != pOther->IsCollisionEnabled() )
		EnableCollisions( pOther->IsCollisionEnabled() );

	if ( this->IsGravityEnabled() != pOther->IsGravityEnabled() )
		EnableGravity( pOther->IsGravityEnabled() );

	if ( this->IsDragEnabled() != pOther->IsDragEnabled() )
		EnableDrag(pOther->IsDragEnabled() );

	if ( this->IsMotionEnabled() != pOther->IsMotionEnabled() )
		EnableMotion(pOther->IsMotionEnabled() );
}

//-------------------------------------------------------------------------------------------------

void JoltPhysicsObject::UpdateEnvironment( JoltPhysicsEnvironment *pEnvironment )
{
	m_pEnvironment = pEnvironment;
	m_pPhysicsSystem = pEnvironment->GetPhysicsSystem();
}

void JoltPhysicsObject::AddDestroyedListener( IJoltObjectDestroyedListener *pListener )
{
	m_destroyedListeners.AddToTail( pListener );
}

void JoltPhysicsObject::RemoveDestroyedListener( IJoltObjectDestroyedListener *pListener )
{
	m_destroyedListeners.FindAndRemove( pListener );
}

void JoltPhysicsObject::AddToPosition( JPH::Vec3Arg addPos )
{
	const JPH::BodyLockInterfaceNoLock &bodyLockInterface = m_pPhysicsSystem->GetBodyLockInterfaceNoLock();

	JPH::BodyLockWrite lock( bodyLockInterface, m_pBody->GetID() );
	if ( lock.Succeeded() )
	{
		JPH::Body &body = lock.GetBody();

		JPH::BodyInterface &bodyInterface = m_pPhysicsSystem->GetBodyInterfaceNoLock();
		bodyInterface.SetPosition( m_pBody->GetID(), body.GetPosition() + addPos, JPH::EActivation::DontActivate );
	}
}

void JoltPhysicsObject::SetPosition( const Vector &worldPosition )
{
	JPH::Vec3 joltPosition = SourceToJolt::Distance( worldPosition );

	JPH::BodyInterface &bodyInterface = m_pPhysicsSystem->GetBodyInterfaceNoLock();

	bodyInterface.SetPosition( m_pBody->GetID(), joltPosition, JPH::EActivation::DontActivate );
}

void JoltPhysicsObject::AddVelocity( const Vector &worldPosition )
{
	JPH::BodyInterface &bodyInterface = m_pPhysicsSystem->GetBodyInterfaceNoLock();
	bodyInterface.AddLinearVelocity( m_pBody->GetID(), SourceToJolt::Distance( worldPosition ) );
}

Vector JoltPhysicsObject::GetVelocity()
{
	JPH::BodyInterface &bodyInterface = m_pPhysicsSystem->GetBodyInterfaceNoLock();
	return JoltToSource::Distance( bodyInterface.GetLinearVelocity( m_pBody->GetID() ) );
}

void JoltPhysicsObject::CalculateBuoyancy()
{
	if ( m_flVolume != 0.0f )
	{
		float flVolume = SourceToJolt::Volume( Max( m_flVolume, 5.0f ) );
		float flDensity = m_flCachedMass / flVolume;
		m_flBuoyancyRatio = flDensity / m_flMaterialDensity;
	}
	else
	{
		m_flBuoyancyRatio = 1.0f;
	}
}

float JoltPhysicsObject::GetMaterialDensity() const
{
	return m_flMaterialDensity;
}

float JoltPhysicsObject::GetBuoyancyRatio() const
{
	return m_flBuoyancyRatio; 
}

bool JoltPhysicsObject::IsControlledByGame() const
{
	if ( m_pShadowController && !m_pShadowController->IsPhysicallyControlled() )
		return true;

	if ( m_callbackFlags & CALLBACK_IS_PLAYER_CONTROLLER )
		return true;

	return false;
}

void JoltPhysicsObject::SaveObjectState( JPH::StateRecorder &recorder )
{
	m_pBody->SaveState( recorder );

	// Josh: Do not write m_pGameData, as this is passed in, in UnserializeObjectFromBuffer.
	//recorder.Write( m_pGameData );
	recorder.Write( m_gameFlags );
	recorder.Write( m_gameIndex );
	recorder.Write( m_callbackFlags );
	recorder.Write( m_bStatic );
	recorder.Write( m_bPinned );
	recorder.Write( m_materialIndex );
	recorder.Write( m_contents );
	recorder.Write( m_flCachedMass );
	recorder.Write( m_flCachedInvMass );
	recorder.Write( m_bCachedCollisionEnabled );
	recorder.Write( m_flMaterialDensity );
	recorder.Write( m_flBuoyancyRatio );
	recorder.Write( m_flVolume );
	recorder.Write( m_GameMaterial );

	// Josh:
	// In regular VPhysics, shadows are serialized but then forced to never be read.
	// Lets just not bother serializing these.
}

void JoltPhysicsObject::RestoreObjectState( JPH::StateRecorder &recorder )
{
	// Restore the body's state.
	m_pBody->RestoreState( recorder );

	// Set the body's userdata as ourselves
	m_pBody->SetUserData( reinterpret_cast<uint64>( this ) );

	// Josh: Do not read m_pGameData, as this is not serialized.
	//recorder.Read( m_pGameData );
	recorder.Read( m_gameFlags );
	recorder.Read( m_gameIndex );
	recorder.Read( m_callbackFlags );
	recorder.Read( m_bStatic );
	recorder.Read( m_bPinned );
	recorder.Read( m_materialIndex );
	recorder.Read( m_contents );
	recorder.Read( m_flCachedMass );
	recorder.Read( m_flCachedInvMass );
	recorder.Read( m_bCachedCollisionEnabled );
	recorder.Read( m_flMaterialDensity );
	recorder.Read( m_flBuoyancyRatio );
	recorder.Read( m_flVolume );
	recorder.Read( m_GameMaterial );

	// Recompute states.
	UpdateMaterialProperties();
	UpdateLayer();
}

//-------------------------------------------------------------------------------------------------

void JoltPhysicsObject::PostSimulation( float flTimestep )
{
	Vector vCurrentPos, vCurrentVel;
	AngularImpulse vAngularImpulse;
	QAngle qCurrentOrientation;

	GetPosition( &vCurrentPos, &qCurrentOrientation );
	GetVelocity( &vCurrentVel, &vAngularImpulse );

	m_vLastVelocity = ( vCurrentPos - m_vLastPosition ) / flTimestep;
	m_vLastPosition = vCurrentPos;

	Vector vGlobalAngleVelocity;
	QAngleToAngularImpulse( ( qCurrentOrientation - m_qLastOrientation ) / flTimestep, vGlobalAngleVelocity );

	m_vLastAngularVelocity = JoltToSource::Unitless( m_pBody->GetWorldTransform().Multiply3x3Transposed( SourceToJolt::Unitless( vGlobalAngleVelocity ) ) );

	m_qLastOrientation = qCurrentOrientation;
}

//-------------------------------------------------------------------------------------------------

void JoltPhysicsObject::UpdateMaterialProperties()
{
	const surfacedata_t *pSurface = JoltPhysicsSurfaceProps::GetInstance().GetSurfaceData( m_materialIndex );

	m_pBody->SetRestitution( pSurface->physics.elasticity );
	m_pBody->SetFriction( pSurface->physics.friction );
	m_flMaterialDensity = pSurface->physics.density;
	m_GameMaterial = pSurface->game.material;
	CalculateBuoyancy();
}

void JoltPhysicsObject::UpdateLayer()
{
	JPH::BodyInterface &bodyInterface = m_pPhysicsSystem->GetBodyInterfaceNoLock();

	const bool bCollisionsEnabled = m_bCachedCollisionEnabled;
	const bool bStatic = IsStatic();
	const bool bPinned = m_bPinned;
	const bool bConstraintPinned = m_bConstraintPinned;
	const bool bDebris = m_collisionHints & COLLISION_HINT_DEBRIS;
	const bool bStaticSolid = m_collisionHints & COLLISION_HINT_STATICSOLID;

	// Update motion type if not made as a complete solid.
	if ( !bStatic && !IsControlledByGame() )
	{
		bool bStaticMotionType = bStaticSolid || bPinned || bConstraintPinned;

		// If we are transfering to being static, and we were active
		// add us to a list of bodies on the environment so we can be included in
		// GetActiveObjects for the next step.
		// This way the game can correctly update the transforms on the game side
		// when move -> wake -> become pinned happens.
		if ( bStaticMotionType && m_pBody->IsActive() )
			m_pEnvironment->AddDirtyStaticBody( m_pBody->GetID() );
		else if ( !bStaticMotionType )
			m_pEnvironment->RemoveDirtyStaticBody( m_pBody->GetID() );

		bodyInterface.SetMotionType( m_pBody->GetID(), bStaticMotionType ? JPH::EMotionType::Static : JPH::EMotionType::Dynamic, JPH::EActivation::Activate );
	}

	// Update layer
	uint8 layer = Layers::MOVING;

	if ( bDebris )
		layer = Layers::DEBRIS;

	if ( bStatic || bStaticSolid )
		layer = Layers::NON_MOVING_WORLD;
	else if ( bPinned || bConstraintPinned )
		layer = Layers::NON_MOVING_OBJECT;

	if ( !bCollisionsEnabled )
		layer = Layers::NO_COLLIDE;

	// Player Controller becomes a dummy object.
	if ( m_callbackFlags & CALLBACK_IS_PLAYER_CONTROLLER )
		layer = Layers::NO_COLLIDE;

	bodyInterface.SetObjectLayer( m_pBody->GetID(), layer );
}

// Integral of each differential drag area's torque over the OBB along one face pair.
// l, w, h are HALF widths -- l along the rotation axis, w perpendicular in the face
// plane, h perpendicular to the face. Sum two such integrals (one per face pair) to
// get the basis for one rotation axis.
static float AngDragIntegral( float flInvInertia, float l, float w, float h )
{
	const float w2 = w * w;
	const float l2 = l * l;
	const float h2 = h * h;

	return flInvInertia * ( ( 1.0f / 3.0f ) * w2 * l * l2 + 0.5f * w2 * w2 * l + l * w2 * h2 );
}

void JoltPhysicsObject::RecomputeDrag()
{
	if ( IsStatic() || !GetCollide() )
		return;

	// AABB in Source space; convert delta to Jolt space (m). Area fractions are unitless,
	// in [0,1], populated from compactsurfaceheader_t::dragAxisAreas at .phy load time.
	// CollideGetOrthographicAreas guarantees (1,1,1) when no precomputed value exists.
	Vector vDragMins, vDragMaxs;
	JoltPhysicsCollision::GetInstance().CollideGetAABB( &vDragMins, &vDragMaxs, GetCollide(), vec3_origin, vec3_angle );

	const Vector vAreaFractions = JoltPhysicsCollision::GetInstance().CollideGetOrthographicAreas( GetCollide() );

	JPH::Vec3 vDelta = SourceToJolt::Distance( vDragMaxs - vDragMins ).Abs();

	const float flInvMass = m_flCachedInvMass;

	// Linear basis: each component is the area of the face perpendicular to that axis,
	// scaled by inverse mass. Units: m^2/kg.
	m_vDragBasis = JPH::Vec3(
		vDelta.GetY() * vDelta.GetZ() * vAreaFractions.x,
		vDelta.GetX() * vDelta.GetZ() * vAreaFractions.y,
		vDelta.GetX() * vDelta.GetY() * vAreaFractions.z ) * flInvMass;

	// Angular basis needs HALF widths.
	vDelta *= 0.5f;
	const JPH::Vec3 vInvInertia = m_pBody->GetMotionProperties()->GetInverseInertiaDiagonal();

	m_vAngDragBasis = JPH::Vec3(
		vAreaFractions.z * AngDragIntegral( vInvInertia.GetX(), vDelta.GetX(), vDelta.GetY(), vDelta.GetZ() )
			+ vAreaFractions.y * AngDragIntegral( vInvInertia.GetX(), vDelta.GetX(), vDelta.GetZ(), vDelta.GetY() ),
		vAreaFractions.z * AngDragIntegral( vInvInertia.GetY(), vDelta.GetY(), vDelta.GetX(), vDelta.GetZ() )
			+ vAreaFractions.x * AngDragIntegral( vInvInertia.GetY(), vDelta.GetY(), vDelta.GetZ(), vDelta.GetX() ),
		vAreaFractions.y * AngDragIntegral( vInvInertia.GetZ(), vDelta.GetZ(), vDelta.GetX(), vDelta.GetY() )
			+ vAreaFractions.x * AngDragIntegral( vInvInertia.GetZ(), vDelta.GetZ(), vDelta.GetY(), vDelta.GetX() ) );
}

float JoltPhysicsObject::GetDragInDirection( JPH::Vec3Arg vWorldVelocity ) const
{
	// Transform the world-space velocity into the body's local frame, then dot with
	// the local-space drag basis. Mirrors IVP's m_world_f_core->vimult3.
	const JPH::Vec3 vLocal = m_pBody->GetRotation().Conjugated() * vWorldVelocity;

	// NB: operator-precedence matches IVP/Source -- the linear coefficient applies
	// only to the X term. Preserved verbatim to match upstream drag intensity.
	return m_flLinearDragCoefficient * fabsf( vLocal.GetX() * m_vDragBasis.GetX() )
		+ fabsf( vLocal.GetY() * m_vDragBasis.GetY() )
		+ fabsf( vLocal.GetZ() * m_vDragBasis.GetZ() );
}

float JoltPhysicsObject::GetAngularDragInDirection( JPH::Vec3Arg vLocalAngularVelocity ) const
{
	return m_flAngularDragCoefficient * fabsf( vLocalAngularVelocity.GetX() * m_vAngDragBasis.GetX() )
		+ fabsf( vLocalAngularVelocity.GetY() * m_vAngDragBasis.GetY() )
		+ fabsf( vLocalAngularVelocity.GetZ() * m_vAngDragBasis.GetZ() );
}

#if GAME_GMOD
IPhysicsEnvironment *JoltPhysicsObject::GetEnvironment()
{
	return m_pEnvironment;
}
#endif

// RaphaelIT7: To improve fixed constraints by making objects kinda static/unmovable when their partner is freezed.
// This is needed since Jolt cannot guarantee perfectly fixed constraints - they could always move/rotate a bit which we don't want.
void JoltPhysicsObject::RecalculateFixedConstraintPartnerMovable()
{
	// Reset our own constraint-pinned state; we'll re-evaluate below.
	m_bConstraintPinned = false;

	for (JPH::Constraint *constraint : m_pPhysicsSystem->GetConstraints())
	{
		if ( constraint->GetType() != JPH::EConstraintType::TwoBodyConstraint )
			continue;

		// Only apply the fixed-constraint optimisation to FixedConstraint (weld).
		// Other constraint types (hinge, rope, ballsocket, etc.) must never force
		// the dynamic partner to Static -- that would freeze it in place.
		if ( constraint->GetSubType() != JPH::EConstraintSubType::Fixed )
			continue;

		JPH::TwoBodyConstraint *twoBody = static_cast<JPH::TwoBodyConstraint*>( constraint );
		JoltPhysicsObject *pObject = nullptr;
		if ( twoBody->GetBody1() == m_pBody )
			pObject = reinterpret_cast< JoltPhysicsObject * >( twoBody->GetBody2()->GetUserData() );
		else if ( twoBody->GetBody2() == m_pBody )
			pObject = reinterpret_cast< JoltPhysicsObject * >( twoBody->GetBody1()->GetUserData() );

		if ( pObject )
		{
			// Only pin BOTH sides when BOTH are already non-moveable (e.g. two frozen props welded
			// together). If one side is dynamic, leave it dynamic so it can still be simulated.
			const bool bothNonMoveable = !IsMoveable() && !pObject->IsMoveable();
			if ( bothNonMoveable )
			{
				m_bConstraintPinned = true;
				pObject->m_bConstraintPinned = true;
			}
			else
			{
				// Dynamic partner must stay dynamic -- do not set ConstraintPinned.
				pObject->m_bConstraintPinned = false;
			}
			pObject->UpdateLayer();
		}
	}
}