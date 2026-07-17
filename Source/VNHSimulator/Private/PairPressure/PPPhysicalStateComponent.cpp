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
#include "PhysicsEngine/PhysicsAsset.h"
#include "TimerManager.h"

namespace
{
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
	if (Daze >= MaxDaze)
	{
		SetPhysicalStateAuthoritative(EPPPhysicalState::Unconscious, NaturalUnconsciousSeconds);
	}
	else
	{
		SetPhysicalStateAuthoritative(EPPPhysicalState::Ragdolled, NormalRagdollSeconds);
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

	SetPhysicalStateAuthoritative(EPPPhysicalState::Ragdolled);
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
		const ACharacter* OwnerCharacter = Cast<ACharacter>(GetOwner());
		const USkeletalMeshComponent* CharacterMesh = OwnerCharacter ? OwnerCharacter->GetMesh() : nullptr;
		const bool bWasRagdollVisual = CharacterMesh && CharacterMesh->IsAnySimulatingPhysics();
		ExitRagdollVisualState();
		if (bWasRagdollVisual && PhysicalState == EPPPhysicalState::Grounded)
		{
			PlayGetUpFrontAnimation();
		}
	}
	OnPhysicalStateChanged.Broadcast(PhysicalState, GetDazeNormalized_Implementation());
}

void UPPPhysicalStateComponent::OnRep_Daze()
{
	OnDazeChanged.Broadcast(GetDazeNormalized_Implementation());
}

void UPPPhysicalStateComponent::ApplyImpactAuthoritative(const FPPImpactData& ImpactData)
{
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
		EffectiveSeverity = FMath::Max(EffectiveSeverity, 60.0f);
		AddDazeAuthoritative(FMath::GetMappedRangeValueClamped(
			FVector2D(60.0f, 150.0f),
			FVector2D(12.0f, 35.0f),
			EffectiveSeverity));
		LastKnockdownTimeSeconds = CurrentTime;
		SetPhysicalStateAuthoritative(EPPPhysicalState::Ragdolled);
		ApplyRagdollPropulsion(ImpactData, EffectiveSeverity);
		BeginGroundedRecovery(1.5f);
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
		SetPhysicalStateAuthoritative(
			Daze >= MaxDaze ? EPPPhysicalState::Unconscious : EPPPhysicalState::Ragdolled,
			Daze >= MaxDaze ? NaturalUnconsciousSeconds : NormalRagdollSeconds);
	}
}

void UPPPhysicalStateComponent::SetPhysicalStateAuthoritative(EPPPhysicalState NewState, float StateDurationSeconds)
{
	if (!GetOwner() || !GetOwner()->HasAuthority())
	{
		return;
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

	if (UCharacterMovementComponent* MovementComponent = OwnerCharacter->GetCharacterMovement())
	{
		MovementComponent->StopMovementImmediately();
		MovementComponent->DisableMovement();
	}
	if (UCapsuleComponent* Capsule = OwnerCharacter->GetCapsuleComponent())
	{
		Capsule->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	}
	if (USkeletalMeshComponent* CharacterMesh = OwnerCharacter->GetMesh())
	{
		CharacterMesh->SetCollisionProfileName(TEXT("Ragdoll"));
		CharacterMesh->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
		CharacterMesh->SetCollisionObjectType(ECC_PhysicsBody);
		CharacterMesh->SetCollisionResponseToAllChannels(ECR_Block);
		CharacterMesh->SetCollisionResponseToChannel(ECC_Pawn, ECR_Ignore);
		CharacterMesh->SetCollisionResponseToChannel(ECC_Camera, ECR_Ignore);
		CharacterMesh->SetAllUseCCD(true);
		CharacterMesh->SetEnableGravity(true);
		CharacterMesh->bPauseAnims = true;
		const FName RagdollRootBone = ResolvePPPhysicalRagdollRootBone(CharacterMesh);
		if (!RagdollRootBone.IsNone())
		{
			CharacterMesh->SetAllBodiesBelowSimulatePhysics(RagdollRootBone, true, true);
			CharacterMesh->SetAllBodiesBelowPhysicsBlendWeight(RagdollRootBone, 1.0f, false, true);
		}
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
			CharacterMesh->SetAllBodiesBelowPhysicsBlendWeight(RagdollRootBone, 0.0f, false, true);
			CharacterMesh->SetAllBodiesBelowSimulatePhysics(RagdollRootBone, false, true);
		}
		CharacterMesh->SetAllBodiesSimulatePhysics(false);
		CharacterMesh->SetAllUseCCD(false);
		CharacterMesh->bPauseAnims = false;
		CharacterMesh->SetCollisionProfileName(TEXT("CharacterMesh"));
		CharacterMesh->SetRelativeTransform(InitialMeshRelativeTransform);
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
	PropelDirection.Z = FMath::Max(PropelDirection.Z, 0.22f);
	PropelDirection.Normalize();
	const float PropelSpeed = FMath::GetMappedRangeValueClamped(
		FVector2D(60.0f, 150.0f),
		FVector2D(500.0f, 1450.0f),
		EffectiveSeverity);
	CharacterMesh->SetAllPhysicsLinearVelocity(PropelDirection * PropelSpeed, false);
	CharacterMesh->WakeAllRigidBodies();
}

void UPPPhysicalStateComponent::PlayGetUpFrontAnimation()
{
	ACharacter* OwnerCharacter = Cast<ACharacter>(GetOwner());
	USkeletalMeshComponent* CharacterMesh = OwnerCharacter ? OwnerCharacter->GetMesh() : nullptr;
	if (!OwnerCharacter || !CharacterMesh || CharacterMesh->IsAnySimulatingPhysics() || !GetWorld())
	{
		return;
	}

	UAnimSequence* GetUpFrontAnimation = nullptr;
	if (UDataTable* MascotAnimationTable = LoadObject<UDataTable>(
		nullptr,
		TEXT("/Game/PairPressure/Data/DT_MascotAnimations.DT_MascotAnimations")))
	{
		if (const FPPMascotAnimationRow* PenguinRow = MascotAnimationTable->FindRow<FPPMascotAnimationRow>(
			FName(TEXT("Penguin")),
			TEXT("Physical-state get-up"),
			false))
		{
			GetUpFrontAnimation = PenguinRow->GetUpFront.LoadSynchronous();
		}
	}
	if (!GetUpFrontAnimation)
	{
		GetUpFrontAnimation = LoadObject<UAnimSequence>(
			nullptr,
			TEXT("/Game/CuteChubbyPenguin/Penguin/Animations/AS_Penguin_UE_Anim_wakesup1.AS_Penguin_UE_Anim_wakesup1"));
	}
	if (!GetUpFrontAnimation)
	{
		return;
	}

	UClass* LocomotionAnimClass = CharacterMesh->GetAnimClass();
	CharacterMesh->SetAnimationMode(EAnimationMode::AnimationSingleNode);
	CharacterMesh->SetAnimation(GetUpFrontAnimation);
	CharacterMesh->Play(false);
	if (UCharacterMovementComponent* MovementComponent = OwnerCharacter->GetCharacterMovement())
	{
		MovementComponent->DisableMovement();
	}

	const float AnimationDuration = FMath::Max(0.1f, GetUpFrontAnimation->GetPlayLength());
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
