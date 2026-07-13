#include "PairPressure/PPPhysicalStateComponent.h"

#include "Components/CapsuleComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/World.h"
#include "GameFramework/Character.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Net/UnrealNetwork.h"
#include "TimerManager.h"

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

	if (!GetWorld() || !GetWorld()->GetMapName().Contains(TEXT("PP_")))
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
		CharacterMesh->SetAllBodiesSimulatePhysics(true);
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
	if (CharacterMesh && CharacterMesh->IsSimulatingPhysics())
	{
		FVector RecoveryLocation = CharacterMesh->GetComponentLocation();
		if (CharacterMesh->DoesSocketExist(TEXT("pelvis")))
		{
			RecoveryLocation = CharacterMesh->GetSocketLocation(TEXT("pelvis"));
		}
		if (Capsule)
		{
			RecoveryLocation.Z += Capsule->GetScaledCapsuleHalfHeight();
		}
		OwnerCharacter->SetActorLocation(RecoveryLocation, false, nullptr, ETeleportType::TeleportPhysics);
		CharacterMesh->SetAllBodiesSimulatePhysics(false);
		CharacterMesh->SetCollisionProfileName(TEXT("CharacterMesh"));
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
