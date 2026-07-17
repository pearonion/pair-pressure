#include "PairPressure/PPPhysicalStateComponent.h"

#include "Animation/AnimInstance.h"
#include "Animation/AnimSequence.h"
#include "Components/CapsuleComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/DataTable.h"
#include "Engine/EngineTypes.h"
#include "Engine/World.h"
#include "GameFramework/Character.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Net/UnrealNetwork.h"
#include "PairPressure/PPGameplayTypes.h"
#include "PairPressure/PPGrabberComponent.h"
#include "PhysicsEngine/PhysicsAsset.h"
#include "PhysicsEngine/BodyInstance.h"
#include "TimerManager.h"

namespace
{
const FName PPStablePenguinRagdollBones[] = {
	FName(TEXT("hips")), FName(TEXT("chest"))
};

// These match the held dummy's charged throw after its 0.70 launch multiplier:
// a slow spinner behaves as a half throw, while a fast one can reach a full throw.
constexpr float PPPhysicalSpinnerMinimumSeverity = 30.0f;
constexpr float PPPhysicalSpinnerFullThrowSeverity = 100.0f;
constexpr float PPPhysicalSpinnerHalfThrowSpeed = 790.0f;
constexpr float PPPhysicalSpinnerFullThrowSpeed = 1580.0f;
constexpr float PPPhysicalMaxRagdollAngularSpeedDegrees = 120.0f;

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

	if (!GetOwner() || !GetOwner()->HasAuthority())
	{
		return;
	}

	if (IsRagdolled())
	{
		ACharacter* OwnerCharacter = Cast<ACharacter>(GetOwner());
		USkeletalMeshComponent* CharacterMesh = OwnerCharacter ? OwnerCharacter->GetMesh() : nullptr;
		const FName RagdollRootBone = ResolvePPPhysicalRagdollRootBone(CharacterMesh);
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

			// Constraints can retain angular energy after the launch. Clamp the live
			// hips velocity as well as the initial throw spin so the whole penguin
			// tumbles naturally instead of pinwheeling while ragdolled.
			const FVector RootAngularVelocity = CharacterMesh->GetPhysicsAngularVelocityInDegrees(RagdollRootBone);
			if (RootAngularVelocity.SizeSquared() > FMath::Square(PPPhysicalMaxRagdollAngularSpeedDegrees))
			{
				CharacterMesh->SetPhysicsAngularVelocityInDegrees(
					RootAngularVelocity.GetClampedToMaxSize(PPPhysicalMaxRagdollAngularSpeedDegrees),
					false,
					RagdollRootBone);
			}
		}
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
		FVector2D(0.85f, 1.6f),
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
	return RequestedCarrier && RequestedCarrier != GetOwner() && IsRagdolled();
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

void UPPPhysicalStateComponent::RequestThrownRagdoll(const FVector& InitialVelocity, const FVector& InitialAngularVelocity)
{
	if (!GetOwner() || !GetOwner()->HasAuthority())
	{
		return;
	}

	const float ThrowRagdollSeconds = FMath::GetMappedRangeValueClamped(
		FVector2D(500.0f, 2600.0f),
		FVector2D(1.0f, 3.0f),
		InitialVelocity.Size());
	SetPhysicalStateAuthoritative(EPPPhysicalState::Ragdolled, ThrowRagdollSeconds);
	ACharacter* OwnerCharacter = Cast<ACharacter>(GetOwner());
	USkeletalMeshComponent* CharacterMesh = OwnerCharacter ? OwnerCharacter->GetMesh() : nullptr;
	if (!CharacterMesh || !CharacterMesh->IsAnySimulatingPhysics())
	{
		return;
	}

	const FName RagdollRootBone = ResolvePPPhysicalRagdollRootBone(CharacterMesh);
	CharacterMesh->SetPhysicsLinearVelocity(InitialVelocity, false, RagdollRootBone);
	CharacterMesh->SetPhysicsAngularVelocityInDegrees(
		InitialAngularVelocity.GetClampedToMaxSize(90.0f),
		false,
		RagdollRootBone);
	CharacterMesh->WakeAllRigidBodies();
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
		EnterRagdollVisualState();
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

void UPPPhysicalStateComponent::ApplyImpactAuthoritative(const FPPImpactData& ImpactData)
{
	// The spinner can overlap again while the body is still tumbling. Do not
	// stack repeated launch velocities or restart the ragdoll timer mid-fall.
	if (IsRagdolled())
	{
		return;
	}

	float EffectiveSeverity = FMath::Clamp(ImpactData.Severity, 0.0f, 150.0f);
	const bool bSpinnerImpact = ImpactData.InstigatorActor
		&& (ImpactData.InstigatorActor->ActorHasTag(TEXT("PP_SpinnerObstacle"))
			|| ImpactData.InstigatorActor->GetName().Contains(TEXT("Spinner_V2"), ESearchCase::IgnoreCase));
	const double CurrentTime = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.0;
	if (CurrentTime - LastKnockdownTimeSeconds < 1.5)
	{
		EffectiveSeverity *= 0.55f;
	}
	if (bSpinnerImpact)
	{
		// Timeline-driven rotating meshes can report a small/zero normal impulse even
		// at high tangential speed, so preserve measured severity but guarantee that
		// a confirmed spinner contact crosses the ragdoll threshold.
		EffectiveSeverity = FMath::Max(EffectiveSeverity, PPPhysicalSpinnerMinimumSeverity);
		AddDazeAuthoritative(FMath::GetMappedRangeValueClamped(
			FVector2D(PPPhysicalSpinnerMinimumSeverity, 150.0f),
			FVector2D(6.0f, 25.0f),
			EffectiveSeverity));
		LastKnockdownTimeSeconds = CurrentTime;
		const float SpinnerRagdollSeconds = FMath::GetMappedRangeValueClamped(
			FVector2D(PPPhysicalSpinnerMinimumSeverity, 150.0f),
			FVector2D(0.85f, 2.25f),
			EffectiveSeverity);
		SetPhysicalStateAuthoritative(EPPPhysicalState::Ragdolled, SpinnerRagdollSeconds);
		ApplyRagdollPropulsion(ImpactData, EffectiveSeverity);
		return;
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
			FVector2D(1.0f, 3.0f),
			EffectiveSeverity);
		SetPhysicalStateAuthoritative(
			Daze >= MaxDaze ? EPPPhysicalState::Unconscious : EPPPhysicalState::Ragdolled,
			ImpactRagdollSeconds);
	}
}

void UPPPhysicalStateComponent::SetPhysicalStateAuthoritative(EPPPhysicalState NewState, float StateDurationSeconds)
{
	if (!GetOwner() || !GetOwner()->HasAuthority())
	{
		return;
	}

	if (NewState == EPPPhysicalState::Ragdolled || NewState == EPPPhysicalState::Unconscious)
	{
		// Every ragdoll source gets a bounded launch/recovery window. This also
		// protects debug and future callers that omit a duration.
		StateDurationSeconds = FMath::Clamp(
			StateDurationSeconds > 0.0f ? StateDurationSeconds : 1.25f,
			0.85f,
			3.0f);
	}

	PhysicalState = NewState;
	ReviveProgressSeconds = 0.0f;
	OnRep_PhysicalState();

	if (GetWorld())
	{
		GetWorld()->GetTimerManager().ClearTimer(RecoveryTimerHandle);
		if (StateDurationSeconds > 0.0f)
		{
			GetWorld()->GetTimerManager().SetTimer(
				RecoveryTimerHandle,
				this,
				&UPPPhysicalStateComponent::RecoverFromCurrentState,
				StateDurationSeconds,
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
	if (OwnerCharacter->HasAuthority())
	{
		if (UPPGrabberComponent* GrabberComponent = OwnerCharacter->FindComponentByClass<UPPGrabberComponent>();
			GrabberComponent && GrabberComponent->GetGrabState_Implementation() != EPPGrabState::None)
		{
			GrabberComponent->ReleaseGrab_Implementation();
		}
	}

	USkeletalMeshComponent* CharacterMesh = OwnerCharacter->GetMesh();
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
		CharacterMesh->bPauseAnims = true;
		const FName RagdollRootBone = ResolvePPPhysicalRagdollRootBone(CharacterMesh);
		if (!bUseStablePenguinRagdoll && !RagdollRootBone.IsNone())
		{
			CharacterMesh->SetAllBodiesBelowSimulatePhysics(RagdollRootBone, true, true);
			CharacterMesh->SetAllBodiesBelowPhysicsBlendWeight(RagdollRootBone, 1.0f, false, true);
		}
		ConfigurePPStablePenguinRagdoll(CharacterMesh);
		CharacterMesh->WakeAllRigidBodies();
	}
}

void UPPPhysicalStateComponent::ExitRagdollVisualState()
{
	ACharacter* OwnerCharacter = Cast<ACharacter>(GetOwner());
	if (!OwnerCharacter)
	{
		return;
	}

	USkeletalMeshComponent* CharacterMesh = OwnerCharacter->GetMesh();
	UCapsuleComponent* Capsule = OwnerCharacter->GetCapsuleComponent();
	if (CharacterMesh && CharacterMesh->IsAnySimulatingPhysics())
	{
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
			CharacterMesh->SetAllBodiesBelowPhysicsBlendWeight(RagdollRootBone, 0.0f, false, true);
			CharacterMesh->SetAllBodiesBelowSimulatePhysics(RagdollRootBone, false, true);
		}
		CharacterMesh->SetAllBodiesSimulatePhysics(false);
		CharacterMesh->SetAllUseCCD(false);
		CharacterMesh->SetEnablePhysicsBlending(false);
		CharacterMesh->bPauseAnims = false;
		CharacterMesh->SetCollisionProfileName(TEXT("CharacterMesh"));
		CharacterMesh->SetRelativeTransform(InitialMeshRelativeTransform);
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
		OwnerCharacter->SetActorLocation(RecoveryLocation, false, nullptr, ETeleportType::TeleportPhysics);
	}
	if (Capsule)
	{
		Capsule->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
	}
	if (UCharacterMovementComponent* MovementComponent = OwnerCharacter->GetCharacterMovement())
	{
		MovementComponent->SetMovementMode(MOVE_Walking);
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
			*GroundedSeconds += 0.1f;
			if (*GroundedSeconds >= RequiredGroundedSeconds)
			{
				PhysicalStateComponent->RecoverFromCurrentState();
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
		0.1f,
		true);
}

bool UPPPhysicalStateComponent::IsRagdollRestingOnGround() const
{
	ACharacter* OwnerCharacter = Cast<ACharacter>(GetOwner());
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

void UPPPhysicalStateComponent::ApplyRagdollPropulsion(const FPPImpactData& ImpactData, float EffectiveSeverity)
{
	ACharacter* OwnerCharacter = Cast<ACharacter>(GetOwner());
	USkeletalMeshComponent* CharacterMesh = OwnerCharacter ? OwnerCharacter->GetMesh() : nullptr;
	if (!CharacterMesh || !CharacterMesh->IsAnySimulatingPhysics())
	{
		return;
	}

	FVector PropelDirection = ImpactData.ImpactDirection.GetSafeNormal();
	if (PropelDirection.IsNearlyZero())
	{
		PropelDirection = ImpactData.InstigatorActor
			? (OwnerCharacter->GetActorLocation() - ImpactData.InstigatorActor->GetActorLocation()).GetSafeNormal()
			: OwnerCharacter->GetActorForwardVector();
	}
	// Spinner normals can contain a large vertical impulse. Keep all knockback
	// predominantly lateral so a rotating obstacle behaves like a throw, not a launcher.
	PropelDirection.Z = FMath::Clamp(PropelDirection.Z, 0.08f, 0.20f);
	PropelDirection.Normalize();
	const float SpinnerThrowStrength = FMath::GetMappedRangeValueClamped(
		FVector2D(PPPhysicalSpinnerMinimumSeverity, PPPhysicalSpinnerFullThrowSeverity),
		FVector2D(0.0f, 1.0f),
		EffectiveSeverity);
	const float PropelSpeed = FMath::Lerp(
		PPPhysicalSpinnerHalfThrowSpeed,
		PPPhysicalSpinnerFullThrowSpeed,
		SpinnerThrowStrength);
	const FName RagdollRootBone = ResolvePPPhysicalRagdollRootBone(CharacterMesh);
	CharacterMesh->SetPhysicsLinearVelocity(PropelDirection * PropelSpeed, false, RagdollRootBone);
	CharacterMesh->WakeAllRigidBodies();
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
		if (const FPPMascotAnimationRow* PenguinRow = MascotAnimationTable->FindRow<FPPMascotAnimationRow>(
			FName(TEXT("Penguin")),
			TEXT("Physical-state get-up"),
			false))
		{
			const float SideUpAmount = LastRecoveryBodyRight.Z;
			if (FMath::Abs(SideUpAmount) >= 0.45f)
			{
				GetUpAnimation = (SideUpAmount > 0.0f ? PenguinRow->GetUpLeft : PenguinRow->GetUpRight).LoadSynchronous();
			}
			else
			{
				// The table has a front recovery for both belly/back landings and
				// side-specific recovery clips for lateral landings.
				GetUpAnimation = PenguinRow->GetUpFront.LoadSynchronous();
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

	if (IsRagdolled() && !IsRagdollRestingOnGround())
	{
		// The duration controls when recovery may begin; movement remains locked
		// until the body has actually fallen and settled onto a traced surface.
		BeginGroundedRecovery(0.1f);
		return;
	}
	if (PhysicalState == EPPPhysicalState::Unconscious)
	{
		Daze = FMath::Min(Daze, MaxDaze * 0.8f);
		OnDazeChanged.Broadcast(GetDazeNormalized_Implementation());
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
