#include "PairPressure/PPPhysicalStateComponent.h"

#include "Animation/AnimInstance.h"
#include "Animation/AnimSequence.h"
#include "Components/CapsuleComponent.h"
#include "Components/PrimitiveComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/DataTable.h"
#include "Engine/EngineTypes.h"
#include "Engine/World.h"
#include "GameFramework/Character.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/GameStateBase.h"
#include "GameFramework/SpringArmComponent.h"
#include "Net/UnrealNetwork.h"
#include "PairPressure/PPGameplayTypes.h"
#include "PairPressure/PPGrabberComponent.h"
#include "PairPressure/Interfaces/PPMascotSelectionInterface.h"
#include "PhysicsEngine/PhysicsAsset.h"
#include "PhysicsEngine/BodyInstance.h"
#include "TimerManager.h"
#include "VNHShopperCharacter.h"

namespace
{
const FName PPStablePenguinRagdollBones[] = {
	FName(TEXT("hips")), FName(TEXT("chest"))
};

constexpr float PPPhysicalCourseKnockdownDurationSeconds = 1.50f;
constexpr float PPPhysicalCourseRecoveryBlendSeconds = 0.60f;
constexpr float PPPhysicalMaxRagdollAngularSpeedDegrees = 120.0f;
constexpr float PPPhysicalRagdollRecoveryBlendSeconds = 0.28f;
constexpr float PPPhysicalRagdollGroundedStabilitySeconds = 0.20f;
constexpr float PPPhysicalRagdollGroundedSampleIntervalSeconds = 0.05f;
constexpr float PPPhysicalMinimumRagdollDurationSeconds = 0.75f;
constexpr float PPPhysicalMaximumRagdollDurationSeconds = 2.20f;
constexpr float PPPhysicalRagdollNetworkPublishIntervalSeconds = 0.05f;
constexpr float PPPhysicalRagdollNetworkMaxExtrapolationSeconds = 0.20f;
constexpr float PPPhysicalRagdollNetworkSnapDistance = 250.0f;
constexpr float PPPhysicalRagdollNetworkPositionCorrectionSpeed = 7.0f;
constexpr float PPPhysicalRagdollNetworkRotationCorrectionSpeed = 5.0f;
constexpr float PPPhysicalRagdollNetworkMaxCorrectionSpeed = 450.0f;

bool IsPPPhysicalCourseRagdollSequence(uint16 Sequence)
{
	return (Sequence & 1u) != 0u;
}

uint16 AdvancePPPhysicalRagdollSequence(uint16 CurrentSequence, bool bCourseRagdoll)
{
	uint16 NextSequence = static_cast<uint16>((CurrentSequence & 0xFFFEu) + 2u);
	if ((NextSequence & 0xFFFEu) == 0u)
	{
		NextSequence = 2u;
	}
	return static_cast<uint16>((NextSequence & 0xFFFEu) | (bCourseRagdoll ? 1u : 0u));
}

FName ResolvePPPhysicalRagdollRootBone(const USkeletalMeshComponent* CharacterMesh)
{
	if (!CharacterMesh)
	{
		return NAME_None;
	}

	const UPhysicsAsset* PhysicsAsset = CharacterMesh->GetPhysicsAsset();
	for (const FName Candidate : {
		FName(TEXT("hips")), FName(TEXT("pelvis")), FName(TEXT("chest")),
		FName(TEXT("spine_03")), FName(TEXT("root"))})
	{
		if (CharacterMesh->GetBoneIndex(Candidate) != INDEX_NONE
			&& (!PhysicsAsset || PhysicsAsset->FindBodyIndex(Candidate) != INDEX_NONE))
		{
			return Candidate;
		}
	}
	return NAME_None;
}

bool UsesPPStablePenguinRagdoll(const USkeletalMeshComponent* CharacterMesh)
{
	if (!CharacterMesh)
	{
		return false;
	}

	for (const FName RagdollBone : PPStablePenguinRagdollBones)
	{
		if (!CharacterMesh->GetBodyInstance(RagdollBone))
		{
			return false;
		}
	}
	return true;
}

void ConfigurePPStablePenguinRagdoll(USkeletalMeshComponent* CharacterMesh)
{
	if (!UsesPPStablePenguinRagdoll(CharacterMesh))
	{
		return;
	}

	// A small fully simulated core lets the mascot topple, while the tight
	// hips-to-chest joint keeps the body silhouette intact without a loose head.
	CharacterMesh->SetAllBodiesSimulatePhysics(false);
	for (const FName RagdollBone : PPStablePenguinRagdollBones)
	{
		CharacterMesh->SetBodySimulatePhysics(RagdollBone, true);
		CharacterMesh->SetAllBodiesBelowPhysicsBlendWeight(RagdollBone, 1.0f, false, true);
	}
}

void ClampPPPhysicalRagdollAngularVelocity(
	USkeletalMeshComponent* CharacterMesh,
	const FName& RagdollRootBone)
{
	if (!CharacterMesh || RagdollRootBone.IsNone())
	{
		return;
	}

	// Run the identical bound on authority and proxies. Otherwise a client's
	// locally simulated chest can keep feeding angular energy back into corrected
	// hips even while the server's compact ragdoll remains stable.
	if (UsesPPStablePenguinRagdoll(CharacterMesh))
	{
		for (const FName RagdollBone : PPStablePenguinRagdollBones)
		{
			const FVector BodyAngularVelocity =
				CharacterMesh->GetPhysicsAngularVelocityInDegrees(RagdollBone);
			if (BodyAngularVelocity.SizeSquared() > FMath::Square(PPPhysicalMaxRagdollAngularSpeedDegrees))
			{
				CharacterMesh->SetPhysicsAngularVelocityInDegrees(
					BodyAngularVelocity.GetClampedToMaxSize(PPPhysicalMaxRagdollAngularSpeedDegrees),
					false,
					RagdollBone);
			}
		}
		return;
	}

	const FVector RootAngularVelocity =
		CharacterMesh->GetPhysicsAngularVelocityInDegrees(RagdollRootBone);
	if (RootAngularVelocity.SizeSquared() > FMath::Square(PPPhysicalMaxRagdollAngularSpeedDegrees))
	{
		CharacterMesh->SetPhysicsAngularVelocityInDegrees(
			RootAngularVelocity.GetClampedToMaxSize(PPPhysicalMaxRagdollAngularSpeedDegrees),
			false,
			RagdollRootBone);
	}
}
}

UPPPhysicalStateComponent::UPPPhysicalStateComponent()
{
	SetIsReplicatedByDefault(true);
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.bStartWithTickEnabled = false;
	PrimaryComponentTick.TickInterval = 0.1f;
}

void UPPPhysicalStateComponent::BeginPlay()
{
	Super::BeginPlay();

	if (!GetWorld() || (!GetWorld()->GetMapName().Contains(TEXT("PP_"))
		&& !GetWorld()->GetMapName().Contains(TEXT("Lobby"))))
	{
		return;
	}

	SetComponentTickEnabled(true);

	if (const ACharacter* OwnerCharacter = Cast<ACharacter>(GetOwner()))
	{
		if (const USkeletalMeshComponent* CharacterMesh = OwnerCharacter->GetMesh())
		{
			InitialMeshRelativeTransform = CharacterMesh->GetRelativeTransform();
		}
	}
}

void UPPPhysicalStateComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
	UpdateRagdollRecoveryBlend(DeltaTime);

	AActor* OwnerActor = GetOwner();
	if (!OwnerActor)
	{
		return;
	}

	if (IsRagdolled())
	{
		ACharacter* OwnerCharacter = Cast<ACharacter>(OwnerActor);
		if (IsCourseObstacleKnockdown())
		{
			// Course knockdowns never hand movement to Chaos. Keep the capsule in
			// CharacterMovement so gravity, floor detection, prediction, and root
			// replication remain identical on host and client. While the fall is
			// locked, discard any residual obstacle/input travel and all upward lift.
			if (UCharacterMovementComponent* MovementComponent = OwnerCharacter
				? OwnerCharacter->GetCharacterMovement()
				: nullptr)
			{
				FVector ConstrainedVelocity = MovementComponent->Velocity;
				ConstrainedVelocity.X = 0.0f;
				ConstrainedVelocity.Y = 0.0f;
				ConstrainedVelocity.Z = FMath::Min(ConstrainedVelocity.Z, 0.0f);
				MovementComponent->Velocity = ConstrainedVelocity;
				MovementComponent->ClearAccumulatedForces();
			}
			return;
		}

		USkeletalMeshComponent* CharacterMesh = OwnerCharacter ? OwnerCharacter->GetMesh() : nullptr;
		const FName RagdollRootBone = ResolvePPPhysicalRagdollRootBone(CharacterMesh);
		if (!OwnerActor->HasAuthority())
		{
			UpdateRemoteRagdollNetworkState(DeltaTime);
			ClampPPPhysicalRagdollAngularVelocity(CharacterMesh, RagdollRootBone);
			return;
		}

		if (CharacterMesh && !RagdollRootBone.IsNone())
		{
			FVector RootVelocity = CharacterMesh->GetBoneLinearVelocity(RagdollRootBone);
			if (RootVelocity.Z > 850.0f)
			{
				// A positive runaway value means a malformed constraint is injecting
				// energy. Cancel it with a small downward bias instead of allowing a
				// capped-but-still-infinite climb.
				RootVelocity.Z = -150.0f;
				CharacterMesh->SetPhysicsLinearVelocity(RootVelocity, false, RagdollRootBone);
			}
		}
		ClampPPPhysicalRagdollAngularVelocity(CharacterMesh, RagdollRootBone);

		const double CurrentWorldTimeSeconds = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.0;
		if (CurrentWorldTimeSeconds - LastRagdollNetworkPublishTimeSeconds
			>= PPPhysicalRagdollNetworkPublishIntervalSeconds)
		{
			PublishRagdollNetworkState(true);
			LastRagdollNetworkPublishTimeSeconds = CurrentWorldTimeSeconds;
		}
	}
	else if (!OwnerActor->HasAuthority())
	{
		return;
	}

	if ((PhysicalState == EPPPhysicalState::Grounded || PhysicalState == EPPPhysicalState::Reactive) && Daze > 0.0f)
	{
		const float PreviousDaze = Daze;
		Daze = FMath::Max(0.0f, Daze - SafeDazeDecayPerSecond * DeltaTime);
		if (!FMath::IsNearlyEqual(PreviousDaze, Daze, 0.1f))
		{
			OnDazeChanged.Broadcast(GetDazeNormalized_Implementation());
		}
	}
}

void UPPPhysicalStateComponent::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(UPPPhysicalStateComponent, PhysicalState);
	DOREPLIFETIME(UPPPhysicalStateComponent, Daze);
	DOREPLIFETIME(UPPPhysicalStateComponent, RagdollNetworkState);
}

float UPPPhysicalStateComponent::GetDazeNormalized_Implementation() const
{
	return MaxDaze > KINDA_SMALL_NUMBER ? FMath::Clamp(Daze / MaxDaze, 0.0f, 1.0f) : 0.0f;
}

void UPPPhysicalStateComponent::RequestRagdoll_Implementation(float DazeAmount)
{
	if (!GetOwner() || IsRagdolled())
	{
		return;
	}

	if (!GetOwner()->HasAuthority())
	{
		ServerRequestRagdoll(DazeAmount);
		return;
	}

	AddDazeAuthoritative(FMath::Max(0.0f, DazeAmount));
	const float RequestedRagdollSeconds = FMath::GetMappedRangeValueClamped(
		FVector2D(0.0f, 10.0f),
		FVector2D(0.75f, 1.35f),
		DazeAmount);
	if (Daze >= MaxDaze)
	{
		SetPhysicalStateAuthoritative(EPPPhysicalState::Unconscious, RequestedRagdollSeconds);
	}
	else
	{
		SetPhysicalStateAuthoritative(EPPPhysicalState::Ragdolled, RequestedRagdollSeconds);
	}
}

void UPPPhysicalStateComponent::RequestRecovery_Implementation()
{
	if (!GetOwner())
	{
		return;
	}

	if (!GetOwner()->HasAuthority())
	{
		ServerRequestRecovery();
		return;
	}

	if (PhysicalState != EPPPhysicalState::Unconscious)
	{
		RecoverFromCurrentState();
	}
}

void UPPPhysicalStateComponent::ReceiveImpactData_Implementation(const FPPImpactData& ImpactData)
{
	if (!GetOwner())
	{
		return;
	}

	if (!GetOwner()->HasAuthority())
	{
		return;
	}

	ApplyImpactAuthoritative(ImpactData);
}

bool UPPPhysicalStateComponent::CanCarry_Implementation(AActor* RequestedCarrier) const
{
	return RequestedCarrier && RequestedCarrier != GetOwner() && IsRagdolled()
		&& !IsCourseObstacleKnockdown();
}

void UPPPhysicalStateComponent::OnCarryStarted_Implementation(AActor* NewCarrier)
{
	ReviveProgressSeconds = 0.0f;
}

void UPPPhysicalStateComponent::OnCarryEnded_Implementation(AActor* PreviousCarrier)
{
	ReviveProgressSeconds = 0.0f;
}

bool UPPPhysicalStateComponent::IsRagdolled() const
{
	return PhysicalState == EPPPhysicalState::Ragdolled || PhysicalState == EPPPhysicalState::Unconscious;
}

bool UPPPhysicalStateComponent::IsCourseObstacleKnockdown() const
{
	return IsRagdolled() && IsPPPhysicalCourseRagdollSequence(RagdollNetworkState.Sequence);
}

float UPPPhysicalStateComponent::GetSynchronizedServerTimeSeconds() const
{
	const UWorld* World = GetWorld();
	if (!World)
	{
		return 0.0f;
	}
	if (const AGameStateBase* GameState = World->GetGameState())
	{
		return GameState->GetServerWorldTimeSeconds();
	}
	return World->GetTimeSeconds();
}

float UPPPhysicalStateComponent::GetRagdollRecoveryBlendAlpha() const
{
	const float LinearAlpha = FMath::Clamp(
		RagdollRecoveryBlendElapsedSeconds / PPPhysicalRagdollRecoveryBlendSeconds,
		0.0f,
		1.0f);
	return FMath::SmoothStep(0.0f, 1.0f, LinearAlpha);
}

void UPPPhysicalStateComponent::AddReviveProgress(float DeltaSeconds, AActor* Reviver)
{
	if (!GetOwner() || !GetOwner()->HasAuthority() || PhysicalState != EPPPhysicalState::Unconscious || !Reviver)
	{
		return;
	}

	ReviveProgressSeconds += FMath::Max(0.0f, DeltaSeconds);
	if (ReviveProgressSeconds >= TeammateReviveSeconds)
	{
		Daze = FMath::Min(Daze, MaxDaze * 0.7f);
		OnDazeChanged.Broadcast(GetDazeNormalized_Implementation());
		RecoverFromCurrentState();
	}
}

void UPPPhysicalStateComponent::RequestDebugRagdoll()
{
	if (!GetOwner())
	{
		return;
	}

	if (!GetOwner()->HasAuthority())
	{
		ServerRequestDebugRagdoll();
		return;
	}

	SetPhysicalStateAuthoritative(EPPPhysicalState::Ragdolled, 1.25f);
}

void UPPPhysicalStateComponent::RequestDebugRecovery()
{
	if (!GetOwner())
	{
		return;
	}

	if (!GetOwner()->HasAuthority())
	{
		ServerRequestDebugRecovery();
		return;
	}

	RecoverFromCurrentState();
}

void UPPPhysicalStateComponent::RequestThrownRagdoll(
	const FVector& InitialVelocity,
	const FVector& InitialAngularVelocity,
	bool bClearExistingBodyVelocities)
{
	if (!GetOwner() || !GetOwner()->HasAuthority())
	{
		return;
	}

	const float ThrowRagdollSeconds = FMath::GetMappedRangeValueClamped(
		FVector2D(500.0f, 2600.0f),
		FVector2D(PPPhysicalMinimumRagdollDurationSeconds, PPPhysicalMaximumRagdollDurationSeconds),
		InitialVelocity.Size());
	SetPhysicalStateAuthoritative(
		EPPPhysicalState::Ragdolled,
		ThrowRagdollSeconds,
		true);
	ACharacter* OwnerCharacter = Cast<ACharacter>(GetOwner());
	USkeletalMeshComponent* CharacterMesh = OwnerCharacter ? OwnerCharacter->GetMesh() : nullptr;
	if (!CharacterMesh || !CharacterMesh->IsAnySimulatingPhysics())
	{
		PublishRagdollNetworkState(true);
		GetOwner()->ForceNetUpdate();
		return;
	}

	if (bClearExistingBodyVelocities)
	{
		// Timeline-driven hazards can inject collision energy into the chest before
		// this callback starts the authored throw. Clear that residual energy so the
		// hips receive the same isolated launch used by the carried penguin dummy.
		CharacterMesh->SetAllPhysicsLinearVelocity(FVector::ZeroVector);
		CharacterMesh->SetAllPhysicsAngularVelocityInDegrees(FVector::ZeroVector);
	}

	const FName RagdollRootBone = ResolvePPPhysicalRagdollRootBone(CharacterMesh);
	if (RagdollRootBone.IsNone())
	{
		PublishRagdollNetworkState(true);
		GetOwner()->ForceNetUpdate();
		return;
	}
	CharacterMesh->SetPhysicsLinearVelocity(InitialVelocity, false, RagdollRootBone);
	CharacterMesh->SetPhysicsAngularVelocityInDegrees(
		InitialAngularVelocity.GetClampedToMaxSize(90.0f),
		false,
		RagdollRootBone);
	CharacterMesh->WakeAllRigidBodies();
	PublishRagdollNetworkState(true);
	GetOwner()->ForceNetUpdate();
}

void UPPPhysicalStateComponent::RequestDummyThrowProfileRagdoll(
	const FVector& ThrowDirection,
	float ChargeAlpha,
	const FVector& InheritedVelocity,
	float BaseThrowSpeed,
	bool bClearExistingBodyVelocities)
{
	AActor* OwnerActor = GetOwner();
	if (!OwnerActor || !OwnerActor->HasAuthority())
	{
		return;
	}

	FVector ValidatedThrowDirection = ThrowDirection.GetSafeNormal();
	if (ValidatedThrowDirection.IsNearlyZero())
	{
		ValidatedThrowDirection = (OwnerActor->GetActorForwardVector().GetSafeNormal2D()
			+ FVector::UpVector * 0.18f).GetSafeNormal();
	}

	const FVector ClampedInheritedVelocity(
		InheritedVelocity.X,
		InheritedVelocity.Y,
		FMath::Clamp(InheritedVelocity.Z, -120.0f, 120.0f));
	const float ClampedChargeAlpha = FMath::Clamp(ChargeAlpha, 0.0f, 1.0f);
	const float ThrowSpeedBaseline = FMath::Max(0.0f, BaseThrowSpeed);
	const float ProfileThrowSpeed = FMath::Lerp(
		ThrowSpeedBaseline * 0.55f,
		ThrowSpeedBaseline * 2.15f,
		ClampedChargeAlpha);
	const FVector ProfileThrowVelocity = ClampedInheritedVelocity
		+ ValidatedThrowDirection * (ProfileThrowSpeed * 0.70f);
	const FVector RagdollForward = OwnerActor->GetActorForwardVector().GetSafeNormal2D();
	const FVector ThrowRollAxis = FVector::CrossProduct(ValidatedThrowDirection, FVector::UpVector).GetSafeNormal();
	const float FacingThrowDot = FVector::DotProduct(RagdollForward, ValidatedThrowDirection.GetSafeNormal2D());
	const FVector ProfileAngularVelocity = ThrowRollAxis * (FacingThrowDot >= 0.0f ? 75.0f : -75.0f);
	RequestThrownRagdoll(
		ProfileThrowVelocity,
		ProfileAngularVelocity,
		bClearExistingBodyVelocities);
}

void UPPPhysicalStateComponent::RequestCourseObstacleRagdoll(
	const FVector& ImpactDirection,
	float /*AuthoredObstacleSpeed*/,
	const AActor* ObstacleActor,
	UPrimitiveComponent* ObstacleComponent,
	const FVector& ImpactPoint)
{
	AActor* OwnerActor = GetOwner();
	if (!OwnerActor || !OwnerActor->HasAuthority() || IsRagdolled())
	{
		return;
	}

	FVector CourseThrowDirection = ImpactDirection.GetSafeNormal2D();
	if (CourseThrowDirection.IsNearlyZero() && ObstacleActor)
	{
		CourseThrowDirection = (OwnerActor->GetActorLocation()
			- ObstacleActor->GetActorLocation()).GetSafeNormal2D();
	}
	if (CourseThrowDirection.IsNearlyZero())
	{
		CourseThrowDirection = OwnerActor->GetActorForwardVector().GetSafeNormal2D();
	}

	const FVector OwnerForward = OwnerActor->GetActorForwardVector().GetSafeNormal2D();
	const FVector OwnerRight = OwnerActor->GetActorRightVector().GetSafeNormal2D();
	const float ForwardAmount = FVector::DotProduct(OwnerForward, CourseThrowDirection);
	const float RightAmount = FVector::DotProduct(OwnerRight, CourseThrowDirection);
	if (FMath::Abs(ForwardAmount) >= FMath::Abs(RightAmount))
	{
		RagdollNetworkState.CourseFallDirection = ForwardAmount >= 0.0f
			? EPPObstacleFallDirection::Forward
			: EPPObstacleFallDirection::Backward;
	}
	else
	{
		RagdollNetworkState.CourseFallDirection = RightAmount >= 0.0f
			? EPPObstacleFallDirection::Right
			: EPPObstacleFallDirection::Left;
	}
	RagdollNetworkState.CourseStartServerTimeSeconds = GetSynchronizedServerTimeSeconds();
	RagdollNetworkState.CourseRecoveryStartServerTimeSeconds = -1.0f;

	// Timeline obstacles commonly move without a swept transform. Move the intact
	// capsule out of the exact source component before starting presentation, then
	// ignore that component until recovery so it cannot keep driving the capsule.
	if (ACharacter* OwnerCharacter = Cast<ACharacter>(OwnerActor))
	{
		if (UCapsuleComponent* OwnerCapsule = OwnerCharacter->GetCapsuleComponent();
			OwnerCapsule && ObstacleComponent)
		{
			IgnoredCourseObstacleComponent = ObstacleComponent;
			OwnerCapsule->IgnoreComponentWhenMoving(ObstacleComponent, true);
			ObstacleComponent->IgnoreActorWhenMoving(OwnerCharacter, true);
			FMTDResult MinimumTranslation;
			const bool bHasPenetration = ObstacleComponent->ComputePenetration(
				MinimumTranslation,
				OwnerCapsule->GetCollisionShape(),
				OwnerCapsule->GetComponentLocation(),
				OwnerCapsule->GetComponentQuat());
			FVector SeparationDirection = bHasPenetration
				? MinimumTranslation.Direction.GetSafeNormal2D()
				: CourseThrowDirection;
			if (!ImpactPoint.IsNearlyZero())
			{
				const FVector AwayFromImpact =
					(OwnerCharacter->GetActorLocation() - ImpactPoint).GetSafeNormal2D();
				if (!AwayFromImpact.IsNearlyZero() && !bHasPenetration)
				{
					SeparationDirection = AwayFromImpact;
				}
			}
			if (SeparationDirection.IsNearlyZero())
			{
				SeparationDirection = CourseThrowDirection;
			}
			const float SeparationDistance = bHasPenetration
				? FMath::Clamp(MinimumTranslation.Distance + 4.0f, 8.0f, 18.0f)
				: 12.0f;
			const FVector SeparationDelta =
				SeparationDirection.GetSafeNormal2D() * SeparationDistance;
			if (!SeparationDelta.IsNearlyZero())
			{
				FHitResult SeparationHit;
				if (UCharacterMovementComponent* MovementComponent = OwnerCharacter->GetCharacterMovement())
				{
					MovementComponent->SafeMoveUpdatedComponent(
						SeparationDelta,
						OwnerCharacter->GetActorQuat(),
						true,
						SeparationHit);
				}
				else
				{
					OwnerCharacter->SetActorLocation(
						OwnerCharacter->GetActorLocation() + SeparationDelta,
						true,
						&SeparationHit,
						ETeleportType::None);
				}
			}
		}
	}
	LastKnockdownTimeSeconds = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.0;
	// Obstacle speed is now eligibility-only. The capsule remains the sole
	// authoritative body and gravity brings an airborne hit down normally; the
	// odd sequence marks this as animation-driven rather than a physical throw.
	SetPhysicalStateAuthoritative(
		EPPPhysicalState::Ragdolled,
		PPPhysicalCourseKnockdownDurationSeconds,
		false,
		true);
}

UPPPhysicalStateComponent* UPPPhysicalStateComponent::FindPhysicalStateComponent(const AActor* Actor)
{
	return Actor ? Actor->FindComponentByClass<UPPPhysicalStateComponent>() : nullptr;
}

void UPPPhysicalStateComponent::ServerRequestRagdoll_Implementation(float DazeAmount)
{
	RequestRagdoll_Implementation(FMath::Clamp(DazeAmount, 0.0f, 10.0f));
}

void UPPPhysicalStateComponent::ServerRequestRecovery_Implementation()
{
	RequestRecovery_Implementation();
}

void UPPPhysicalStateComponent::ServerRequestDebugRagdoll_Implementation()
{
	RequestDebugRagdoll();
}

void UPPPhysicalStateComponent::ServerRequestDebugRecovery_Implementation()
{
	RequestDebugRecovery();
}

void UPPPhysicalStateComponent::OnRep_PhysicalState()
{
	if (IsRagdolled())
	{
		// A joining client must not start unseeded local Chaos while the active
		// launch snapshot is still in flight. Either property may notify first; the
		// second notification completes the idempotent entry.
		if ((GetOwner() && GetOwner()->HasAuthority()) || RagdollNetworkState.bActive)
		{
			EnterRagdollVisualState();
		}
	}
	else
	{
		ExitRagdollVisualState();
	}
	OnPhysicalStateChanged.Broadcast(PhysicalState, GetDazeNormalized_Implementation());
}

void UPPPhysicalStateComponent::OnRep_Daze()
{
	OnDazeChanged.Broadcast(GetDazeNormalized_Implementation());
}

void UPPPhysicalStateComponent::OnRep_RagdollNetworkState()
{
	LastRagdollNetworkReceiptTimeSeconds = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.0;
	if (!GetOwner() || GetOwner()->HasAuthority())
	{
		return;
	}
	if (RagdollNetworkState.bActive && IsRagdolled())
	{
		EnterRagdollVisualState();
		if (!IsCourseObstacleKnockdown())
		{
			const bool bNewSequence = !bHasAppliedRagdollNetworkSequence
				|| LastAppliedRagdollNetworkSequence != RagdollNetworkState.Sequence;
			ApplyRemoteRagdollNetworkState(bNewSequence);
		}
	}
}

void UPPPhysicalStateComponent::PublishRagdollNetworkState(bool bActive)
{
	ACharacter* OwnerCharacter = Cast<ACharacter>(GetOwner());
	USkeletalMeshComponent* CharacterMesh = OwnerCharacter ? OwnerCharacter->GetMesh() : nullptr;
	if (!OwnerCharacter || !OwnerCharacter->HasAuthority())
	{
		return;
	}

	RagdollNetworkState.bActive = bActive;
	if (!bActive || !CharacterMesh)
	{
		RagdollNetworkState.RootLocation = OwnerCharacter->GetActorLocation();
		RagdollNetworkState.RootRotation = OwnerCharacter->GetActorRotation();
		RagdollNetworkState.LinearVelocity = FVector::ZeroVector;
		RagdollNetworkState.AngularVelocityDegrees = FVector::ZeroVector;
		return;
	}
	if (IsCourseObstacleKnockdown())
	{
		// Course presentation follows ordinary replicated CharacterMovement. Do
		// not sample or publish a skeletal body that is intentionally not simulating.
		RagdollNetworkState.RootLocation = OwnerCharacter->GetActorLocation();
		RagdollNetworkState.RootRotation = OwnerCharacter->GetActorRotation();
		RagdollNetworkState.LinearVelocity = FVector::ZeroVector;
		RagdollNetworkState.AngularVelocityDegrees = FVector::ZeroVector;
		return;
	}

	const FName RagdollRootBone = ResolvePPPhysicalRagdollRootBone(CharacterMesh);
	FBodyInstance* RootBodyInstance = RagdollRootBone.IsNone()
		? nullptr
		: CharacterMesh->GetBodyInstance(RagdollRootBone);
	if (!RootBodyInstance || !RootBodyInstance->IsValidBodyInstance())
	{
		// Never carry a previous throw's payload into a new active sequence. This
		// fallback keeps a malformed/missing body local to the current actor instead
		// of hard-snapping proxies to an old ragdoll transform.
		RagdollNetworkState.RootLocation = OwnerCharacter->GetActorLocation();
		RagdollNetworkState.RootRotation = OwnerCharacter->GetActorRotation();
		RagdollNetworkState.LinearVelocity = FVector::ZeroVector;
		RagdollNetworkState.AngularVelocityDegrees = FVector::ZeroVector;
		return;
	}

	const FTransform RootBodyTransform = RootBodyInstance->GetUnrealWorldTransform();
	RagdollNetworkState.RootLocation = RootBodyTransform.GetLocation();
	RagdollNetworkState.RootRotation = RootBodyTransform.Rotator();
	RagdollNetworkState.LinearVelocity = CharacterMesh->GetBoneLinearVelocity(RagdollRootBone);
	RagdollNetworkState.AngularVelocityDegrees =
		CharacterMesh->GetPhysicsAngularVelocityInDegrees(RagdollRootBone);
}

void UPPPhysicalStateComponent::ApplyRemoteRagdollNetworkState(bool bSnapToAuthoritativeState)
{
	ACharacter* OwnerCharacter = Cast<ACharacter>(GetOwner());
	USkeletalMeshComponent* CharacterMesh = OwnerCharacter ? OwnerCharacter->GetMesh() : nullptr;
	if (!OwnerCharacter || OwnerCharacter->HasAuthority() || !CharacterMesh
		|| !RagdollNetworkState.bActive || !IsRagdolled()
		|| IsCourseObstacleKnockdown())
	{
		return;
	}

	const FName RagdollRootBone = ResolvePPPhysicalRagdollRootBone(CharacterMesh);
	FBodyInstance* RootBodyInstance = RagdollRootBone.IsNone()
		? nullptr
		: CharacterMesh->GetBodyInstance(RagdollRootBone);
	if (!RootBodyInstance || !RootBodyInstance->IsValidBodyInstance())
	{
		return;
	}

	const FTransform CurrentBodyTransform = RootBodyInstance->GetUnrealWorldTransform();
	const FVector AuthoritativeLocation = FVector(RagdollNetworkState.RootLocation);
	const bool bNewRagdollSequence = !bHasAppliedRagdollNetworkSequence
		|| LastAppliedRagdollNetworkSequence != RagdollNetworkState.Sequence;
	const bool bLargeCorrection = FVector::DistSquared(
		CurrentBodyTransform.GetLocation(),
		AuthoritativeLocation) > FMath::Square(PPPhysicalRagdollNetworkSnapDistance);
	if (!bSnapToAuthoritativeState && !bLargeCorrection)
	{
		return;
	}

	const FTransform AuthoritativeRootTransform(
		RagdollNetworkState.RootRotation,
		AuthoritativeLocation);
	const FVector AuthoritativeLinearVelocity = FVector(RagdollNetworkState.LinearVelocity);
	const FVector AuthoritativeAngularVelocityDegrees =
		FVector(RagdollNetworkState.AngularVelocityDegrees)
		.GetClampedToMaxSize(PPPhysicalMaxRagdollAngularSpeedDegrees);
	FBodyInstance* SecondaryBodyInstance = nullptr;
	FTransform CurrentSecondaryTransform = FTransform::Identity;
	if (UsesPPStablePenguinRagdoll(CharacterMesh)
		&& RagdollRootBone != FName(TEXT("chest")))
	{
		SecondaryBodyInstance = CharacterMesh->GetBodyInstance(FName(TEXT("chest")));
		if (SecondaryBodyInstance && SecondaryBodyInstance->IsValidBodyInstance())
		{
			CurrentSecondaryTransform = SecondaryBodyInstance->GetUnrealWorldTransform();
		}
		else
		{
			SecondaryBodyInstance = nullptr;
		}
	}

	RootBodyInstance->SetBodyTransform(AuthoritativeRootTransform, ETeleportType::TeleportPhysics);
	if (SecondaryBodyInstance)
	{
		// Apply the authoritative hips correction as one rigid delta to chest too.
		// Snapping hips alone stretches the tight constraint and injects exactly the
		// orbital torque this compact two-body setup is intended to avoid.
		FQuat RigidRotationDelta = AuthoritativeRootTransform.GetRotation()
			* CurrentBodyTransform.GetRotation().Inverse();
		RigidRotationDelta.Normalize();
		const FVector CorrectedSecondaryLocation = AuthoritativeRootTransform.GetLocation()
			+ RigidRotationDelta.RotateVector(
				CurrentSecondaryTransform.GetLocation() - CurrentBodyTransform.GetLocation());
		SecondaryBodyInstance->SetBodyTransform(
			FTransform(
				RigidRotationDelta * CurrentSecondaryTransform.GetRotation(),
				CorrectedSecondaryLocation),
			ETeleportType::TeleportPhysics);

		if (bNewRagdollSequence)
		{
			// The proven dummy launch deliberately starts with authored velocity on
			// hips only after clearing all bodies. Mirror that exact initial condition
			// on proxies instead of changing the server/reference throw profile.
			CharacterMesh->SetPhysicsLinearVelocity(FVector::ZeroVector, false, FName(TEXT("chest")));
			CharacterMesh->SetPhysicsAngularVelocityInDegrees(FVector::ZeroVector, false, FName(TEXT("chest")));
		}
		else
		{
			// A later hard correction is different: forcing an already-moving chest
			// stationary against corrected hips creates a large constraint impulse.
			// Move it with the rigid core so the correction cannot inject an orbit.
			const FVector AuthoritativeAngularVelocityRadians =
				AuthoritativeAngularVelocityDegrees * (PI / 180.0f);
			const FVector CorrectedSecondaryVelocity = AuthoritativeLinearVelocity
				+ FVector::CrossProduct(
					AuthoritativeAngularVelocityRadians,
					CorrectedSecondaryLocation - AuthoritativeRootTransform.GetLocation());
			CharacterMesh->SetPhysicsLinearVelocity(
				CorrectedSecondaryVelocity,
				false,
				FName(TEXT("chest")));
			CharacterMesh->SetPhysicsAngularVelocityInDegrees(
				AuthoritativeAngularVelocityDegrees,
				false,
				FName(TEXT("chest")));
		}
	}
	CharacterMesh->SetPhysicsLinearVelocity(
		AuthoritativeLinearVelocity,
		false,
		RagdollRootBone);
	CharacterMesh->SetPhysicsAngularVelocityInDegrees(
		AuthoritativeAngularVelocityDegrees,
		false,
		RagdollRootBone);
	CharacterMesh->WakeAllRigidBodies();
	bHasAppliedRagdollNetworkSequence = true;
	LastAppliedRagdollNetworkSequence = RagdollNetworkState.Sequence;
}

void UPPPhysicalStateComponent::UpdateRemoteRagdollNetworkState(float DeltaTime)
{
	ACharacter* OwnerCharacter = Cast<ACharacter>(GetOwner());
	USkeletalMeshComponent* CharacterMesh = OwnerCharacter ? OwnerCharacter->GetMesh() : nullptr;
	if (!OwnerCharacter || OwnerCharacter->HasAuthority() || !CharacterMesh
		|| !RagdollNetworkState.bActive || !IsRagdolled()
		|| IsCourseObstacleKnockdown())
	{
		return;
	}

	const FName RagdollRootBone = ResolvePPPhysicalRagdollRootBone(CharacterMesh);
	FBodyInstance* RootBodyInstance = RagdollRootBone.IsNone()
		? nullptr
		: CharacterMesh->GetBodyInstance(RagdollRootBone);
	if (!RootBodyInstance || !RootBodyInstance->IsValidBodyInstance())
	{
		return;
	}

	const FTransform CurrentBodyTransform = RootBodyInstance->GetUnrealWorldTransform();
	const UWorld* World = GetWorld();
	const float SnapshotAgeSeconds = World
		? FMath::Clamp(
			static_cast<float>(World->GetTimeSeconds() - LastRagdollNetworkReceiptTimeSeconds),
			0.0f,
			PPPhysicalRagdollNetworkMaxExtrapolationSeconds)
		: 0.0f;
	const FVector SnapshotLinearVelocity = FVector(RagdollNetworkState.LinearVelocity);
	const FVector GravityAcceleration(0.0f, 0.0f, World ? World->GetGravityZ() : -980.0f);
	// Predict the authoritative Chaos body ballistically between 20 Hz snapshots.
	// Reusing the last upward velocity as a constant target made remote throws
	// hover until the next packet delivered the server's gravity-integrated state.
	const FVector TargetLocation = FVector(RagdollNetworkState.RootLocation)
		+ SnapshotLinearVelocity * SnapshotAgeSeconds
		+ GravityAcceleration * (0.5f * FMath::Square(SnapshotAgeSeconds));
	const FVector PredictedLinearVelocity = SnapshotLinearVelocity
		+ GravityAcceleration * SnapshotAgeSeconds;
	const float LocationErrorSquared = FVector::DistSquared(CurrentBodyTransform.GetLocation(), TargetLocation);
	if (LocationErrorSquared > FMath::Square(PPPhysicalRagdollNetworkSnapDistance))
	{
		ApplyRemoteRagdollNetworkState(true);
		return;
	}

	// Keep the constrained two-body ragdoll physically continuous between
	// snapshots. Teleporting hips every render tick while also applying the
	// authoritative velocity injected energy into the chest constraint and made
	// remote players orbit/jitter. A bounded velocity correction converges without
	// fighting Chaos; only a genuinely large error uses the snap path above.
	const FVector PositionCorrectionVelocity =
		((TargetLocation - CurrentBodyTransform.GetLocation())
			* PPPhysicalRagdollNetworkPositionCorrectionSpeed)
		.GetClampedToMaxSize(PPPhysicalRagdollNetworkMaxCorrectionSpeed);
	const FVector DesiredLinearVelocity =
		PredictedLinearVelocity + PositionCorrectionVelocity;
	const FVector CurrentLinearVelocity = CharacterMesh->GetBoneLinearVelocity(RagdollRootBone);
	const FVector SmoothedLinearVelocity = FMath::VInterpTo(
		CurrentLinearVelocity,
		DesiredLinearVelocity,
		DeltaTime,
		12.0f);
	CharacterMesh->SetPhysicsLinearVelocity(SmoothedLinearVelocity, false, RagdollRootBone);

	FQuat RotationDelta = RagdollNetworkState.RootRotation.Quaternion()
		* CurrentBodyTransform.GetRotation().Inverse();
	RotationDelta.Normalize();
	FVector RotationAxis = FVector::UpVector;
	float RotationAngleRadians = 0.0f;
	RotationDelta.ToAxisAndAngle(RotationAxis, RotationAngleRadians);
	if (RotationAngleRadians > PI)
	{
		RotationAngleRadians -= 2.0f * PI;
	}
	const FVector RotationCorrectionVelocity = RotationAxis
		* FMath::RadiansToDegrees(RotationAngleRadians)
		* PPPhysicalRagdollNetworkRotationCorrectionSpeed;
	const FVector DesiredAngularVelocity =
		(FVector(RagdollNetworkState.AngularVelocityDegrees) + RotationCorrectionVelocity)
		.GetClampedToMaxSize(PPPhysicalMaxRagdollAngularSpeedDegrees);
	const FVector CurrentAngularVelocity =
		CharacterMesh->GetPhysicsAngularVelocityInDegrees(RagdollRootBone);
	CharacterMesh->SetPhysicsAngularVelocityInDegrees(
		FMath::VInterpTo(CurrentAngularVelocity, DesiredAngularVelocity, DeltaTime, 10.0f),
		false,
		RagdollRootBone);
}

void UPPPhysicalStateComponent::ApplyImpactAuthoritative(const FPPImpactData& ImpactData)
{
	// The spinner can overlap again while the body is still tumbling. Do not
	// stack repeated launch velocities or restart the ragdoll timer mid-fall.
	if (IsRagdolled())
	{
		return;
	}

	float EffectiveSeverity = FMath::Clamp(ImpactData.Severity, 0.0f, 150.0f);
	const double CurrentTime = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.0;
	if (CurrentTime - LastKnockdownTimeSeconds < 1.5)
	{
		EffectiveSeverity *= 0.55f;
	}

	if (EffectiveSeverity <= 25.0f)
	{
		AddDazeAuthoritative(2.0f);
		SetPhysicalStateAuthoritative(EPPPhysicalState::Reactive, 0.35f);
	}
	else if (EffectiveSeverity <= 50.0f)
	{
		AddDazeAuthoritative(5.0f);
		SetPhysicalStateAuthoritative(EPPPhysicalState::Stumbling, 0.75f);
	}
	else
	{
		const float DazeGain = EffectiveSeverity > 80.0f ? (ImpactData.bHeavyObstacle ? 50.0f : 40.0f) : 25.0f;
		AddDazeAuthoritative(DazeGain);
		LastKnockdownTimeSeconds = CurrentTime;
		const float ImpactRagdollSeconds = FMath::GetMappedRangeValueClamped(
			FVector2D(50.0f, 150.0f),
			FVector2D(0.80f, PPPhysicalMaximumRagdollDurationSeconds),
			EffectiveSeverity);
		SetPhysicalStateAuthoritative(
			Daze >= MaxDaze ? EPPPhysicalState::Unconscious : EPPPhysicalState::Ragdolled,
			ImpactRagdollSeconds);
	}
}

void UPPPhysicalStateComponent::SetPhysicalStateAuthoritative(
	EPPPhysicalState NewState,
	float StateDurationSeconds,
	bool bDeferInitialNetworkPublish,
	bool bCourseRagdoll)
{
	if (!GetOwner() || !GetOwner()->HasAuthority())
	{
		return;
	}

	const bool bWasRagdolled = IsRagdolled();
	const bool bTimedRagdollState = NewState == EPPPhysicalState::Ragdolled
		|| NewState == EPPPhysicalState::Unconscious;
	if (bTimedRagdollState)
	{
		// Every ragdoll source gets a bounded launch/recovery window. This also
		// protects debug and future callers that omit a duration.
		StateDurationSeconds = FMath::Clamp(
			StateDurationSeconds > 0.0f ? StateDurationSeconds : 1.10f,
			PPPhysicalMinimumRagdollDurationSeconds,
			PPPhysicalMaximumRagdollDurationSeconds);
	}

	PhysicalState = NewState;
	if (bTimedRagdollState && (!bWasRagdolled || bDeferInitialNetworkPublish))
	{
		RagdollNetworkState.Sequence = AdvancePPPhysicalRagdollSequence(
			RagdollNetworkState.Sequence,
			bCourseRagdoll);
		if (bCourseRagdoll)
		{
			if (RagdollNetworkState.CourseStartServerTimeSeconds < 0.0f)
			{
				RagdollNetworkState.CourseStartServerTimeSeconds =
					GetSynchronizedServerTimeSeconds();
			}
			RagdollNetworkState.CourseRecoveryStartServerTimeSeconds = -1.0f;
			bCourseObstacleRecoveryActive = false;
		}
		else
		{
			RagdollNetworkState.CourseStartServerTimeSeconds = -1.0f;
			RagdollNetworkState.CourseRecoveryStartServerTimeSeconds = -1.0f;
		}
		bHasAppliedRagdollNetworkSequence = false;
		LastRagdollNetworkPublishTimeSeconds = -100.0;
	}
	else if (!bTimedRagdollState && bWasRagdolled)
	{
		if (UPPGrabberComponent* GrabberComponent = GetOwner()->FindComponentByClass<UPPGrabberComponent>())
		{
			GrabberComponent->FinishIncomingCarryRagdoll();
		}
	}
	ReviveProgressSeconds = 0.0f;
	OnRep_PhysicalState();
	if (!bDeferInitialNetworkPublish)
	{
		PublishRagdollNetworkState(bTimedRagdollState);
		GetOwner()->ForceNetUpdate();
	}

	if (GetWorld())
	{
		GetWorld()->GetTimerManager().ClearTimer(RecoveryTimerHandle);
		GetWorld()->GetTimerManager().ClearTimer(CourseRecoveryTimerHandle);
		if (StateDurationSeconds > 0.0f)
		{
			// Begin checking for continuous ground stability early enough that both
			// that confirmation and the standing blend finish at the authored minimum
			// duration. Airtime naturally extends the recovery instead of adding a
			// second fixed punishment after an early landing.
			const float RecoveryTimerDelay = bTimedRagdollState
				? FMath::Max(
					PPPhysicalRagdollGroundedSampleIntervalSeconds,
					StateDurationSeconds
						- PPPhysicalRagdollGroundedStabilitySeconds
						- (bCourseRagdoll
							? PPPhysicalCourseRecoveryBlendSeconds
							: PPPhysicalRagdollRecoveryBlendSeconds))
				: StateDurationSeconds;
			GetWorld()->GetTimerManager().SetTimer(
				RecoveryTimerHandle,
				this,
				&UPPPhysicalStateComponent::RecoverFromCurrentState,
				RecoveryTimerDelay,
				false);
		}
	}
}

void UPPPhysicalStateComponent::EnterRagdollVisualState()
{
	ACharacter* OwnerCharacter = Cast<ACharacter>(GetOwner());
	if (!OwnerCharacter)
	{
		return;
	}
	if (IsCourseObstacleKnockdown())
	{
		EnterCourseObstacleKnockdownVisualState();
		return;
	}
	// While ragdolled, the replicated hips snapshot is the single authoritative
	// movement source. Leaving ACharacter root movement replication enabled lets
	// an older capsule packet pull a remote player or dummy against the simulated
	// mesh, which presents as a short float or a client-authored throw not moving.
	OwnerCharacter->SetReplicateMovement(false);
	UPPGrabberComponent* GrabberComponent = OwnerCharacter->FindComponentByClass<UPPGrabberComponent>();
	if (GrabberComponent)
	{
		GrabberComponent->PrepareIncomingCarryForRagdoll();
	}
	USkeletalMeshComponent* CharacterMesh = OwnerCharacter->GetMesh();
	if (CharacterMesh && CharacterMesh->IsAnySimulatingPhysics() && !bRagdollRecoveryBlendActive)
	{
		if (!OwnerCharacter->HasAuthority() && RagdollNetworkState.bActive)
		{
			const bool bNewSequence = !bHasAppliedRagdollNetworkSequence
				|| LastAppliedRagdollNetworkSequence != RagdollNetworkState.Sequence;
			ApplyRemoteRagdollNetworkState(bNewSequence);
		}
		return;
	}
	bRagdollRecoveryBlendActive = false;
	RagdollRecoveryBlendElapsedSeconds = 0.0f;
	SetComponentTickInterval(0.0f);
	if (OwnerCharacter->HasAuthority())
	{
		if (GrabberComponent && GrabberComponent->GetGrabState_Implementation() != EPPGrabState::None)
		{
			GrabberComponent->ReleaseGrab_Implementation();
		}
	}

	const bool bUseStablePenguinRagdoll = UsesPPStablePenguinRagdoll(CharacterMesh);
	if (UCharacterMovementComponent* MovementComponent = OwnerCharacter->GetCharacterMovement())
	{
		MovementComponent->StopMovementImmediately();
		MovementComponent->DisableMovement();
	}
	if (UCapsuleComponent* Capsule = OwnerCharacter->GetCapsuleComponent())
	{
		Capsule->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	}
	if (CharacterMesh)
	{
		CharacterMesh->SetCollisionProfileName(TEXT("Ragdoll"));
		CharacterMesh->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
		CharacterMesh->SetCollisionObjectType(ECC_PhysicsBody);
		CharacterMesh->SetCollisionResponseToAllChannels(ECR_Block);
		CharacterMesh->SetCollisionResponseToChannel(ECC_Pawn, ECR_Ignore);
		CharacterMesh->SetCollisionResponseToChannel(ECC_Camera, ECR_Ignore);
		CharacterMesh->SetAllUseCCD(true);
		CharacterMesh->SetEnableGravity(true);
		for (FBodyInstance* BodyInstance : CharacterMesh->Bodies)
		{
			if (BodyInstance)
			{
				BodyInstance->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
				if (!bUseStablePenguinRagdoll)
				{
					BodyInstance->LinearDamping = 0.35f;
					BodyInstance->AngularDamping = 1.15f;
					BodyInstance->UpdateDampingProperties();
				}
			}
		}
		CharacterMesh->SetAllMotorsAngularPositionDrive(false, false, false);
		CharacterMesh->SetAllMotorsAngularVelocityDrive(false, false, false);
		CharacterMesh->SetAllMotorsAngularDriveParams(0.0f, 0.0f, 0.0f, false);
		CharacterMesh->SetEnablePhysicsBlending(true);
		// Keep pose evaluation running during the hybrid two-body ragdoll. Physics
		// fully overrides hips/chest, while the evaluated bodyless child bones inherit
		// those simulated parents. Pausing animation or setting its rate to zero leaves
		// feet and other bodyless parts behind at the disabled capsule.
		CharacterMesh->bPauseAnims = false;
		CharacterMesh->GlobalAnimRateScale = 1.0f;
		const FName RagdollRootBone = ResolvePPPhysicalRagdollRootBone(CharacterMesh);
		if (!bUseStablePenguinRagdoll && !RagdollRootBone.IsNone())
		{
			CharacterMesh->SetAllBodiesBelowSimulatePhysics(RagdollRootBone, true, true);
			CharacterMesh->SetAllBodiesBelowPhysicsBlendWeight(RagdollRootBone, 1.0f, false, true);
		}
		ConfigurePPStablePenguinRagdoll(CharacterMesh);
		CharacterMesh->WakeAllRigidBodies();
	}
	if (!OwnerCharacter->HasAuthority() && RagdollNetworkState.bActive)
	{
		const bool bNewSequence = !bHasAppliedRagdollNetworkSequence
			|| LastAppliedRagdollNetworkSequence != RagdollNetworkState.Sequence;
		ApplyRemoteRagdollNetworkState(bNewSequence);
	}
}

void UPPPhysicalStateComponent::EnterCourseObstacleKnockdownVisualState()
{
	ACharacter* OwnerCharacter = Cast<ACharacter>(GetOwner());
	if (!OwnerCharacter || !IsCourseObstacleKnockdown())
	{
		return;
	}

	const bool bNewCourseSequence = !bHasAppliedRagdollNetworkSequence
		|| LastAppliedRagdollNetworkSequence != RagdollNetworkState.Sequence;
	if (bNewCourseSequence)
	{
		bHasAppliedRagdollNetworkSequence = true;
		LastAppliedRagdollNetworkSequence = RagdollNetworkState.Sequence;
		bCourseObstacleRecoveryActive = false;
		bCourseObstacleKnockdownVisualActive = true;
		bRagdollRecoveryBlendActive = false;
		RagdollRecoveryBlendElapsedSeconds = 0.0f;
		SetComponentTickInterval(0.0f);
		SetComponentTickEnabled(true);
		OwnerCharacter->SetReplicateMovement(true);

		UPPGrabberComponent* GrabberComponent =
			OwnerCharacter->FindComponentByClass<UPPGrabberComponent>();
		if (GrabberComponent)
		{
			GrabberComponent->PrepareIncomingCarryForRagdoll();
			if (OwnerCharacter->HasAuthority()
				&& GrabberComponent->GetGrabState_Implementation() != EPPGrabState::None)
			{
				GrabberComponent->ReleaseGrab_Implementation();
			}
		}

		if (USkeletalMeshComponent* CharacterMesh = OwnerCharacter->GetMesh())
		{
			// A course fall must never inherit a half-finished Chaos state. The mesh
			// stays on its AnimBP and the capsule remains the only collision body.
			CharacterMesh->SetAllBodiesSimulatePhysics(false);
			CharacterMesh->SetEnablePhysicsBlending(false);
			CharacterMesh->SetEnableGravity(true);
			CharacterMesh->SetAllUseCCD(false);
			CharacterMesh->SetCollisionProfileName(TEXT("CharacterMesh"));
			CharacterMesh->SetRelativeTransform(InitialMeshRelativeTransform);
			CharacterMesh->bPauseAnims = false;
			CharacterMesh->GlobalAnimRateScale = 1.0f;
		}
		if (UCapsuleComponent* Capsule = OwnerCharacter->GetCapsuleComponent())
		{
			Capsule->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
		}
		if (UCharacterMovementComponent* MovementComponent = OwnerCharacter->GetCharacterMovement())
		{
			const bool bWasGrounded = MovementComponent->IsMovingOnGround();
			const float PreviousVerticalVelocity = MovementComponent->Velocity.Z;
			MovementComponent->StopMovementImmediately();
			MovementComponent->ClearAccumulatedForces();
			// A dive recovery, ledge hold, or carried state may have left movement in
			// MOVE_None. Always restore a gravity-capable mode here; otherwise an
			// interrupted course hit can remain suspended forever. An uncertain floor
			// state deliberately falls for one frame and lets CharacterMovement perform
			// its normal authoritative landing test.
			MovementComponent->SetMovementMode(bWasGrounded ? MOVE_Walking : MOVE_Falling);
			MovementComponent->Velocity.Z = bWasGrounded
				? 0.0f
				: FMath::Min(PreviousVerticalVelocity, 0.0f);
		}

		if (AVNHShopperCharacter* ShopperCharacter = Cast<AVNHShopperCharacter>(OwnerCharacter))
		{
			const float PresentationElapsedSeconds = FMath::Max(
				0.0f,
				GetSynchronizedServerTimeSeconds()
					- RagdollNetworkState.CourseStartServerTimeSeconds);
			ShopperCharacter->BeginPairPressureObstacleFallPresentation(
				RagdollNetworkState.CourseFallDirection,
				PresentationElapsedSeconds);
		}
	}

	if (!bCourseObstacleRecoveryActive
		&& RagdollNetworkState.CourseRecoveryStartServerTimeSeconds >= 0.0f)
	{
		bCourseObstacleRecoveryActive = true;
		if (AVNHShopperCharacter* ShopperCharacter = Cast<AVNHShopperCharacter>(OwnerCharacter))
		{
			const float RecoveryElapsedSeconds = FMath::Max(
				0.0f,
				GetSynchronizedServerTimeSeconds()
					- RagdollNetworkState.CourseRecoveryStartServerTimeSeconds);
			ShopperCharacter->BeginPairPressureObstacleFallRecoveryPresentation(
				RecoveryElapsedSeconds,
				PPPhysicalCourseRecoveryBlendSeconds);
		}
	}
}

void UPPPhysicalStateComponent::ExitRagdollVisualState()
{
	ACharacter* OwnerCharacter = Cast<ACharacter>(GetOwner());
	if (!OwnerCharacter)
	{
		return;
	}
	if (bCourseObstacleKnockdownVisualActive)
	{
		ExitCourseObstacleKnockdownVisualState();
		return;
	}

	USkeletalMeshComponent* CharacterMesh = OwnerCharacter->GetMesh();
	UCapsuleComponent* Capsule = OwnerCharacter->GetCapsuleComponent();
	FVector RecoveryForward = OwnerCharacter->GetActorForwardVector().GetSafeNormal2D();
	if (CharacterMesh && CharacterMesh->IsAnySimulatingPhysics())
	{
		const FTransform RagdollMeshWorldTransform = CharacterMesh->GetComponentTransform();
		FVector RecoveryLocation = CharacterMesh->GetComponentLocation();
		const FName RagdollRootBone = ResolvePPPhysicalRagdollRootBone(CharacterMesh);
		if (!RagdollRootBone.IsNone())
		{
			RecoveryLocation = CharacterMesh->GetBoneLocation(RagdollRootBone);
		}
		if (Capsule)
		{
			RecoveryLocation.Z += Capsule->GetScaledCapsuleHalfHeight();
		}
		if (!RagdollRootBone.IsNone())
		{
			const FQuat RecoveryBodyRotation = CharacterMesh->GetBoneQuaternion(RagdollRootBone);
			LastRecoveryBodyUp = RecoveryBodyRotation.GetUpVector();
			LastRecoveryBodyRight = RecoveryBodyRotation.GetRightVector();
			RecoveryForward = RecoveryBodyRotation.GetForwardVector().GetSafeNormal2D();
		}
		CharacterMesh->SetAllPhysicsLinearVelocity(FVector::ZeroVector);
		CharacterMesh->SetAllPhysicsAngularVelocityInDegrees(FVector::ZeroVector);
		CharacterMesh->SetEnableGravity(false);
		// Frozen ragdoll bodies must not keep receiving pushes while their visible
		// pose is blending into the standing capsule target.
		CharacterMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		CharacterMesh->SetEnablePhysicsBlending(true);
		CharacterMesh->bPauseAnims = false;
		CharacterMesh->GlobalAnimRateScale = 1.0f;
		if (UWorld* World = GetWorld(); World && Capsule)
		{
			FCollisionQueryParams QueryParams(SCENE_QUERY_STAT(PPRagdollRecoveryGround), false, OwnerCharacter);
			FHitResult GroundHit;
			const float CapsuleHalfHeight = Capsule->GetScaledCapsuleHalfHeight();
			if (World->LineTraceSingleByChannel(
				GroundHit,
				RecoveryLocation + FVector(0.0f, 0.0f, CapsuleHalfHeight + 100.0f),
				RecoveryLocation - FVector(0.0f, 0.0f, CapsuleHalfHeight + 250.0f),
				ECC_Visibility,
				QueryParams))
			{
				RecoveryLocation = GroundHit.ImpactPoint + FVector(0.0f, 0.0f, CapsuleHalfHeight);
			}
		}
		AVNHShopperCharacter* ShopperCharacter = Cast<AVNHShopperCharacter>(OwnerCharacter);
		USpringArmComponent* RecoveryCameraBoom = ShopperCharacter
			? ShopperCharacter->GetFollowCameraBoom()
			: nullptr;
		const bool bPreserveLocalCameraFocus = ShopperCharacter
			&& ShopperCharacter->IsLocallyControlled()
			&& RecoveryCameraBoom;
		const FTransform PreservedCameraBoomWorldTransform = bPreserveLocalCameraFocus
			? RecoveryCameraBoom->GetComponentTransform()
			: FTransform::Identity;
		if (!RecoveryForward.IsNearlyZero())
		{
			OwnerCharacter->SetActorRotation(RecoveryForward.Rotation(), ETeleportType::TeleportPhysics);
		}
		OwnerCharacter->SetActorLocation(RecoveryLocation, false, nullptr, ETeleportType::TeleportPhysics);
		if (bPreserveLocalCameraFocus)
		{
			ShopperCharacter->StabilizePairPressureRecoveryCamera(PreservedCameraBoomWorldTransform);
		}
		// Move the collision capsule to its safe standing target while keeping the
		// rendered ragdoll exactly where physics left it. The mesh then blends into
		// its authored standing transform instead of visibly teleporting upward.
		CharacterMesh->SetWorldTransform(RagdollMeshWorldTransform, false, nullptr, ETeleportType::TeleportPhysics);
		RagdollRecoveryBlendStartTransform = CharacterMesh->GetRelativeTransform();
		RagdollRecoveryBlendElapsedSeconds = 0.0f;
		bRagdollRecoveryBlendActive = true;
		SetComponentTickInterval(0.0f);
		SetComponentTickEnabled(true);
	}
	else
	{
		if (CharacterMesh)
		{
			CharacterMesh->SetRelativeTransform(InitialMeshRelativeTransform);
		}
		if (Capsule)
		{
			Capsule->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
		}
		if (UCharacterMovementComponent* MovementComponent = OwnerCharacter->GetCharacterMovement())
		{
			MovementComponent->SetMovementMode(MOVE_Walking);
		}
		OwnerCharacter->SetReplicateMovement(true);
		if (OwnerCharacter->HasAuthority())
		{
			OwnerCharacter->ForceNetUpdate();
		}
	}
	if (UCharacterMovementComponent* MovementComponent = OwnerCharacter->GetCharacterMovement();
		MovementComponent && bRagdollRecoveryBlendActive)
	{
		MovementComponent->DisableMovement();
	}
}

void UPPPhysicalStateComponent::ExitCourseObstacleKnockdownVisualState()
{
	ACharacter* OwnerCharacter = Cast<ACharacter>(GetOwner());
	if (!OwnerCharacter)
	{
		return;
	}

	if (AVNHShopperCharacter* ShopperCharacter = Cast<AVNHShopperCharacter>(OwnerCharacter))
	{
		ShopperCharacter->EndPairPressureObstacleFallPresentation(
			PPPhysicalCourseRecoveryBlendSeconds);
	}
	if (UCapsuleComponent* Capsule = OwnerCharacter->GetCapsuleComponent())
	{
		Capsule->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
		if (UPrimitiveComponent* ObstacleComponent = IgnoredCourseObstacleComponent.Get())
		{
			Capsule->IgnoreComponentWhenMoving(ObstacleComponent, false);
			ObstacleComponent->IgnoreActorWhenMoving(OwnerCharacter, false);
		}
	}
	IgnoredCourseObstacleComponent.Reset();
	if (USkeletalMeshComponent* CharacterMesh = OwnerCharacter->GetMesh())
	{
		CharacterMesh->SetAllBodiesSimulatePhysics(false);
		CharacterMesh->SetEnablePhysicsBlending(false);
		CharacterMesh->SetEnableGravity(true);
		CharacterMesh->SetAllUseCCD(false);
		CharacterMesh->SetCollisionProfileName(TEXT("CharacterMesh"));
		CharacterMesh->SetRelativeTransform(InitialMeshRelativeTransform);
		CharacterMesh->bPauseAnims = false;
		CharacterMesh->GlobalAnimRateScale = 1.0f;
	}
	if (UCharacterMovementComponent* MovementComponent = OwnerCharacter->GetCharacterMovement())
	{
		MovementComponent->ClearAccumulatedForces();
		if (MovementComponent->MovementMode == MOVE_None)
		{
			MovementComponent->SetMovementMode(MOVE_Walking);
		}
	}
	OwnerCharacter->SetReplicateMovement(true);
	if (OwnerCharacter->HasAuthority())
	{
		OwnerCharacter->ForceNetUpdate();
	}
	bCourseObstacleKnockdownVisualActive = false;
	bCourseObstacleRecoveryActive = false;
	SetComponentTickInterval(0.1f);
}

void UPPPhysicalStateComponent::BeginCourseObstacleRecovery()
{
	ACharacter* OwnerCharacter = Cast<ACharacter>(GetOwner());
	if (!OwnerCharacter || !OwnerCharacter->HasAuthority() || !GetWorld()
		|| !IsCourseObstacleKnockdown() || bCourseObstacleRecoveryActive)
	{
		return;
	}

	bCourseObstacleRecoveryActive = true;
	RagdollNetworkState.CourseRecoveryStartServerTimeSeconds =
		GetSynchronizedServerTimeSeconds();
	if (AVNHShopperCharacter* ShopperCharacter = Cast<AVNHShopperCharacter>(OwnerCharacter))
	{
		ShopperCharacter->BeginPairPressureObstacleFallRecoveryPresentation(
			0.0f,
			PPPhysicalCourseRecoveryBlendSeconds);
	}
	PublishRagdollNetworkState(true);
	OwnerCharacter->ForceNetUpdate();
	GetWorld()->GetTimerManager().ClearTimer(RecoveryTimerHandle);
	GetWorld()->GetTimerManager().SetTimer(
		CourseRecoveryTimerHandle,
		this,
		&UPPPhysicalStateComponent::CompleteCourseObstacleRecovery,
		PPPhysicalCourseRecoveryBlendSeconds,
		false);
}

void UPPPhysicalStateComponent::CompleteCourseObstacleRecovery()
{
	if (!GetOwner() || !GetOwner()->HasAuthority() || !IsCourseObstacleKnockdown())
	{
		return;
	}
	SetPhysicalStateAuthoritative(EPPPhysicalState::Grounded);
}

void UPPPhysicalStateComponent::UpdateRagdollRecoveryBlend(float DeltaTime)
{
	if (!bRagdollRecoveryBlendActive)
	{
		return;
	}

	ACharacter* OwnerCharacter = Cast<ACharacter>(GetOwner());
	USkeletalMeshComponent* CharacterMesh = OwnerCharacter ? OwnerCharacter->GetMesh() : nullptr;
	if (!OwnerCharacter || !CharacterMesh || IsRagdolled())
	{
		bRagdollRecoveryBlendActive = false;
		SetComponentTickInterval(0.1f);
		return;
	}

	RagdollRecoveryBlendElapsedSeconds += FMath::Max(DeltaTime, 0.0f);
	const float LinearAlpha = FMath::Clamp(
		RagdollRecoveryBlendElapsedSeconds / PPPhysicalRagdollRecoveryBlendSeconds,
		0.0f,
		1.0f);
	const float BlendAlpha = GetRagdollRecoveryBlendAlpha();
	FTransform BlendedRelativeTransform;
	BlendedRelativeTransform.Blend(
		RagdollRecoveryBlendStartTransform,
		InitialMeshRelativeTransform,
		BlendAlpha);
	CharacterMesh->SetRelativeTransform(BlendedRelativeTransform);

	// Recovery intentionally disables the mesh's physics collision before this
	// blend begins. The bone-scoped API rejects that invalid physics state and
	// emits one PIE warning per frame; the all-body API safely updates the two
	// authored core bodies without requiring a live collision state.
	CharacterMesh->SetAllBodiesPhysicsBlendWeight(1.0f - BlendAlpha, false);
	if (LinearAlpha < 1.0f)
	{
		return;
	}

	bRagdollRecoveryBlendActive = false;
	RagdollRecoveryBlendElapsedSeconds = 0.0f;
	SetComponentTickInterval(0.1f);
	CharacterMesh->SetAllBodiesSimulatePhysics(false);
	CharacterMesh->SetAllUseCCD(false);
	CharacterMesh->SetEnableGravity(true);
	CharacterMesh->SetEnablePhysicsBlending(false);
	CharacterMesh->GlobalAnimRateScale = 1.0f;
	CharacterMesh->SetCollisionProfileName(TEXT("CharacterMesh"));
	CharacterMesh->SetRelativeTransform(InitialMeshRelativeTransform);
	if (UCapsuleComponent* Capsule = OwnerCharacter->GetCapsuleComponent())
	{
		Capsule->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
	}
	if (UCharacterMovementComponent* MovementComponent = OwnerCharacter->GetCharacterMovement())
	{
		MovementComponent->SetMovementMode(MOVE_Walking);
	}
	// Resume ordinary character movement replication only after the visible mesh
	// has finished blending back to its capsule. The authority then sends the
	// final standing root as the next movement baseline for every connection.
	OwnerCharacter->SetReplicateMovement(true);
	if (OwnerCharacter->HasAuthority())
	{
		OwnerCharacter->ForceNetUpdate();
	}
}

void UPPPhysicalStateComponent::BeginGroundedRecovery(float RequiredGroundedSeconds)
{
	if (!GetOwner() || !GetOwner()->HasAuthority() || !GetWorld())
	{
		return;
	}
	TWeakObjectPtr<UPPPhysicalStateComponent> WeakPhysicalState(this);
	const TSharedRef<float> GroundedSeconds = MakeShared<float>(0.0f);
	FTimerDelegate GroundedRecoveryDelegate;
	GroundedRecoveryDelegate.BindLambda([WeakPhysicalState, GroundedSeconds, RequiredGroundedSeconds]()
	{
		UPPPhysicalStateComponent* PhysicalStateComponent = WeakPhysicalState.Get();
		if (!PhysicalStateComponent || !PhysicalStateComponent->GetWorld() || !PhysicalStateComponent->IsRagdolled())
		{
			return;
		}

		if (PhysicalStateComponent->IsRagdollRestingOnGround())
		{
			*GroundedSeconds += PPPhysicalRagdollGroundedSampleIntervalSeconds;
			if (*GroundedSeconds >= RequiredGroundedSeconds)
			{
				if (PhysicalStateComponent->IsCourseObstacleKnockdown())
				{
					PhysicalStateComponent->BeginCourseObstacleRecovery();
					return;
				}
				if (PhysicalStateComponent->PhysicalState == EPPPhysicalState::Unconscious)
				{
					PhysicalStateComponent->Daze = FMath::Min(
						PhysicalStateComponent->Daze,
						PhysicalStateComponent->MaxDaze * 0.8f);
					PhysicalStateComponent->OnDazeChanged.Broadcast(
						PhysicalStateComponent->GetDazeNormalized_Implementation());
				}
				PhysicalStateComponent->SetPhysicalStateAuthoritative(EPPPhysicalState::Grounded);
			}
		}
		else
		{
			*GroundedSeconds = 0.0f;
		}
	});
	GetWorld()->GetTimerManager().SetTimer(
		RecoveryTimerHandle,
		GroundedRecoveryDelegate,
		PPPhysicalRagdollGroundedSampleIntervalSeconds,
		true);
}

bool UPPPhysicalStateComponent::IsRagdollRestingOnGround() const
{
	ACharacter* OwnerCharacter = Cast<ACharacter>(GetOwner());
	if (IsCourseObstacleKnockdown())
	{
		const UCharacterMovementComponent* MovementComponent = OwnerCharacter
			? OwnerCharacter->GetCharacterMovement()
			: nullptr;
		return MovementComponent && MovementComponent->IsMovingOnGround()
			&& MovementComponent->Velocity.SizeSquared2D() <= FMath::Square(5.0f)
			&& MovementComponent->Velocity.Z <= 1.0f;
	}
	USkeletalMeshComponent* CharacterMesh = OwnerCharacter ? OwnerCharacter->GetMesh() : nullptr;
	UWorld* World = GetWorld();
	if (!CharacterMesh || !World)
	{
		return false;
	}

	const FName RagdollRootBone = ResolvePPPhysicalRagdollRootBone(CharacterMesh);
	const FVector RootLocation = RagdollRootBone.IsNone()
		? CharacterMesh->GetComponentLocation()
		: CharacterMesh->GetBoneLocation(RagdollRootBone);
	const FVector RootVelocity = RagdollRootBone.IsNone()
		? CharacterMesh->GetComponentVelocity()
		: CharacterMesh->GetBoneLinearVelocity(RagdollRootBone);
	if (RootVelocity.Size() > 180.0f)
	{
		return false;
	}

	FCollisionQueryParams QueryParams(SCENE_QUERY_STAT(PPSpinnerGroundedRecovery), false, GetOwner());
	FHitResult GroundHit;
	return World->LineTraceSingleByChannel(
		GroundHit,
		RootLocation + FVector(0.0f, 0.0f, 15.0f),
		RootLocation - FVector(0.0f, 0.0f, 55.0f),
		ECC_Visibility,
		QueryParams);
}

void UPPPhysicalStateComponent::PlayGetUpAnimation()
{
	ACharacter* OwnerCharacter = Cast<ACharacter>(GetOwner());
	USkeletalMeshComponent* CharacterMesh = OwnerCharacter ? OwnerCharacter->GetMesh() : nullptr;
	if (!OwnerCharacter || !CharacterMesh || CharacterMesh->IsAnySimulatingPhysics() || !GetWorld())
	{
		return;
	}

	UAnimSequence* GetUpAnimation = nullptr;
	if (UDataTable* MascotAnimationTable = LoadObject<UDataTable>(
		nullptr,
		TEXT("/Game/PairPressure/Data/DT_MascotAnimations.DT_MascotAnimations")))
	{
		const FName MascotRowName = OwnerCharacter->GetClass()->ImplementsInterface(UPPMascotSelectionInterface::StaticClass())
			? IPPMascotSelectionInterface::Execute_GetSelectedMascotRowName(OwnerCharacter)
			: FName(TEXT("Penguin"));
		if (const FPPMascotAnimationRow* MascotRow = MascotAnimationTable->FindRow<FPPMascotAnimationRow>(
			MascotRowName,
			TEXT("Physical-state get-up"),
			false))
		{
			const float SideUpAmount = LastRecoveryBodyRight.Z;
			if (FMath::Abs(SideUpAmount) >= 0.45f)
			{
				GetUpAnimation = (SideUpAmount > 0.0f ? MascotRow->GetUpLeft : MascotRow->GetUpRight).LoadSynchronous();
			}
			else
			{
				// The table has a front recovery for both belly/back landings and
				// side-specific recovery clips for lateral landings.
				GetUpAnimation = MascotRow->GetUpFront.LoadSynchronous();
			}
		}
	}
	if (!GetUpAnimation)
	{
		GetUpAnimation = LoadObject<UAnimSequence>(
			nullptr,
			TEXT("/Game/CuteChubbyPenguin/Penguin/Animations/AS_Penguin_UE_Anim_wakesup1.AS_Penguin_UE_Anim_wakesup1"));
	}
	if (!GetUpAnimation)
	{
		return;
	}

	UClass* LocomotionAnimClass = CharacterMesh->GetAnimClass();
	CharacterMesh->SetAnimationMode(EAnimationMode::AnimationSingleNode);
	CharacterMesh->SetAnimation(GetUpAnimation);
	CharacterMesh->Play(false);
	if (UCharacterMovementComponent* MovementComponent = OwnerCharacter->GetCharacterMovement())
	{
		MovementComponent->DisableMovement();
	}

	const float AnimationDuration = FMath::Max(0.1f, GetUpAnimation->GetPlayLength());
	TWeakObjectPtr<ACharacter> WeakOwnerCharacter(OwnerCharacter);
	TWeakObjectPtr<USkeletalMeshComponent> WeakCharacterMesh(CharacterMesh);
	FTimerHandle GetUpTimerHandle;
	GetWorld()->GetTimerManager().SetTimer(
		GetUpTimerHandle,
		[WeakOwnerCharacter, WeakCharacterMesh, LocomotionAnimClass]()
		{
			ACharacter* RecoveringCharacter = WeakOwnerCharacter.Get();
			USkeletalMeshComponent* RecoveringMesh = WeakCharacterMesh.Get();
			if (!RecoveringCharacter || !RecoveringMesh || RecoveringMesh->IsAnySimulatingPhysics())
			{
				return;
			}
			if (const UPPPhysicalStateComponent* PhysicalState = UPPPhysicalStateComponent::FindPhysicalStateComponent(RecoveringCharacter);
				PhysicalState && PhysicalState->IsRagdolled())
			{
				return;
			}
			RecoveringMesh->SetAnimationMode(EAnimationMode::AnimationBlueprint);
			if (LocomotionAnimClass)
			{
				RecoveringMesh->SetAnimInstanceClass(LocomotionAnimClass);
			}
			if (UCharacterMovementComponent* MovementComponent = RecoveringCharacter->GetCharacterMovement())
			{
				MovementComponent->SetMovementMode(MOVE_Walking);
			}
		},
		AnimationDuration,
		false);
}

void UPPPhysicalStateComponent::RecoverFromCurrentState()
{
	if (!GetOwner() || !GetOwner()->HasAuthority())
	{
		return;
	}

	if (IsRagdolled())
	{
		// A single low-velocity trace is not enough after a hard landing. Require a
		// short continuous stable-ground window before starting the existing visual
		// blend; an airborne body keeps polling and can never stand in midair.
		BeginGroundedRecovery(PPPhysicalRagdollGroundedStabilitySeconds);
		return;
	}
	SetPhysicalStateAuthoritative(EPPPhysicalState::Grounded);
}

void UPPPhysicalStateComponent::AddDazeAuthoritative(float DazeAmount)
{
	const float PreviousDaze = Daze;
	Daze = FMath::Clamp(Daze + DazeAmount, 0.0f, MaxDaze);
	if (!FMath::IsNearlyEqual(PreviousDaze, Daze))
	{
		OnDazeChanged.Broadcast(GetDazeNormalized_Implementation());
	}
}
