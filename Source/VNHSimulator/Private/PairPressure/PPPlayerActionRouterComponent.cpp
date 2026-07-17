#include "PairPressure/PPPlayerActionRouterComponent.h"

#include "PairPressure/PPCarryComponent.h"
#include "PairPressure/PPGrabberComponent.h"
#include "PairPressure/PPPhysicalStateComponent.h"
#include "GameFramework/Character.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Net/UnrealNetwork.h"
#include "TimerManager.h"

UPPPlayerActionRouterComponent::UPPPlayerActionRouterComponent()
{
	PrimaryComponentTick.bCanEverTick = false;
	SetIsReplicatedByDefault(true);
}

void UPPPlayerActionRouterComponent::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME_CONDITION(UPPPlayerActionRouterComponent, bAirDiveArmed, COND_OwnerOnly);
	DOREPLIFETIME(UPPPlayerActionRouterComponent, bAirDiveActive);
	DOREPLIFETIME(UPPPlayerActionRouterComponent, bAirDiveRecoveryActive);
}

void UPPPlayerActionRouterComponent::RequestDive()
{
	if (!CanAirDive() || !GetOwner())
	{
		return;
	}

	if (!GetOwner()->HasAuthority())
	{
		bAirDiveArmed = false;
		bAirDiveActive = true;
		OnRep_AirDiveActive();
		ServerRequestDive();
		return;
	}
	PerformDiveAuthoritative();
}

void UPPPlayerActionRouterComponent::NotifyJumpStarted()
{
	if (!GetOwner())
	{
		return;
	}
	ArmAirDive();
	if (!GetOwner()->HasAuthority())
	{
		ServerNotifyJumpStarted();
	}
}

void UPPPlayerActionRouterComponent::NotifyLanded()
{
	bAirDiveArmed = false;
	if (bAirDiveActive && !bAirDiveRecoveryActive)
	{
		BeginDiveRecovery();
	}
}

bool UPPPlayerActionRouterComponent::CanAirDive() const
{
	const ACharacter* OwnerCharacter = Cast<ACharacter>(GetOwner());
	const UCharacterMovementComponent* OwnerMovement = OwnerCharacter ? OwnerCharacter->GetCharacterMovement() : nullptr;
	const UPPGrabberComponent* GrabberComponent = GetOwner()
		? GetOwner()->FindComponentByClass<UPPGrabberComponent>()
		: nullptr;
	const UPPPhysicalStateComponent* PhysicalState = UPPPhysicalStateComponent::FindPhysicalStateComponent(GetOwner());
	return bAirDiveArmed && !bAirDiveActive && OwnerMovement && OwnerMovement->IsFalling()
		&& (!PhysicalState || (!PhysicalState->IsRagdolled() && !PhysicalState->IsUnconscious()))
		&& (!GrabberComponent || GrabberComponent->CanJumpOrDive());
}

void UPPPlayerActionRouterComponent::ServerNotifyJumpStarted_Implementation()
{
	const ACharacter* OwnerCharacter = Cast<ACharacter>(GetOwner());
	const UCharacterMovementComponent* OwnerMovement = OwnerCharacter ? OwnerCharacter->GetCharacterMovement() : nullptr;
	if (OwnerMovement && OwnerMovement->IsFalling() && OwnerMovement->Velocity.Z > 0.0f)
	{
		ArmAirDive();
	}
}

void UPPPlayerActionRouterComponent::ServerRequestDive_Implementation()
{
	if (CanAirDive())
	{
		PerformDiveAuthoritative();
		return;
	}
	ClientDiveRejected();
}

void UPPPlayerActionRouterComponent::ClientDiveRejected_Implementation()
{
	bAirDiveRecoveryActive = false;
	OnRep_AirDiveRecoveryActive();
	bAirDiveActive = false;
	OnRep_AirDiveActive();
}

void UPPPlayerActionRouterComponent::OnRep_AirDiveActive()
{
	if (bAirDiveActive && DiveStartTimeSeconds < 0.0 && GetWorld())
	{
		DiveStartTimeSeconds = GetWorld()->GetTimeSeconds();
	}
	else if (!bAirDiveActive)
	{
		DiveStartTimeSeconds = -1.0;
		if (ACharacter* OwnerCharacter = Cast<ACharacter>(GetOwner()))
		{
			if (UCharacterMovementComponent* OwnerMovement = OwnerCharacter->GetCharacterMovement();
				OwnerMovement && OwnerMovement->MovementMode == MOVE_None)
			{
				OwnerMovement->SetMovementMode(MOVE_Walking);
			}
		}
	}
	OnAirDiveStateChanged.Broadcast(bAirDiveActive);
}

void UPPPlayerActionRouterComponent::OnRep_AirDiveRecoveryActive()
{
	OnAirDiveRecoveryStateChanged.Broadcast(bAirDiveRecoveryActive);
}

void UPPPlayerActionRouterComponent::ArmAirDive()
{
	bAirDiveArmed = true;
}

void UPPPlayerActionRouterComponent::PerformDiveAuthoritative()
{
	if (!GetOwner() || !GetOwner()->HasAuthority() || !CanAirDive())
	{
		return;
	}

	bAirDiveArmed = false;
	bAirDiveActive = true;
	bAirDiveRecoveryActive = false;
	DiveStartTimeSeconds = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.0;
	OnRep_AirDiveActive();
	if (ACharacter* OwnerCharacter = Cast<ACharacter>(GetOwner()))
	{
		FVector DiveDirection = OwnerCharacter->GetActorForwardVector().GetSafeNormal2D();
		if (DiveDirection.IsNearlyZero())
		{
			DiveDirection = FVector::ForwardVector;
		}
		const float ExistingHorizontalSpeed = OwnerCharacter->GetVelocity().Size2D();
		const float AppliedHorizontalSpeed = FMath::Max(DiveHorizontalSpeed, ExistingHorizontalSpeed);
		OwnerCharacter->LaunchCharacter(
			DiveDirection * AppliedHorizontalSpeed + FVector(0.0f, 0.0f, DiveVerticalSpeed),
			true,
			true);
	}
}

void UPPPlayerActionRouterComponent::BeginDiveRecovery()
{
	if (!GetOwner() || bAirDiveRecoveryActive)
	{
		return;
	}

	bAirDiveRecoveryActive = true;
	OnRep_AirDiveRecoveryActive();
	if (ACharacter* OwnerCharacter = Cast<ACharacter>(GetOwner()))
	{
		if (UCharacterMovementComponent* OwnerMovement = OwnerCharacter->GetCharacterMovement())
		{
			OwnerMovement->StopMovementImmediately();
			OwnerMovement->DisableMovement();
		}
	}

	if (!GetOwner()->HasAuthority() || !GetWorld())
	{
		return;
	}

	GetWorld()->GetTimerManager().SetTimer(
		DiveRecoveryTimerHandle,
		this,
		&UPPPlayerActionRouterComponent::FinishDiveRecovery,
		DiveLandingGetUpDelaySeconds + DiveLandingRecoverySeconds,
		false);
}

void UPPPlayerActionRouterComponent::FinishDiveRecovery()
{
	if (!GetOwner() || !GetOwner()->HasAuthority())
	{
		return;
	}

	bAirDiveRecoveryActive = false;
	OnRep_AirDiveRecoveryActive();
	bAirDiveActive = false;
	OnRep_AirDiveActive();
	if (ACharacter* OwnerCharacter = Cast<ACharacter>(GetOwner()))
	{
		if (UCharacterMovementComponent* OwnerMovement = OwnerCharacter->GetCharacterMovement())
		{
			OwnerMovement->SetMovementMode(MOVE_Walking);
		}
	}
}

void UPPPlayerActionRouterComponent::BeginAssist()
{
	if (UPPCarryComponent* CarryComponent = GetOwner() ? GetOwner()->FindComponentByClass<UPPCarryComponent>() : nullptr)
	{
		CarryComponent->BeginAssist();
	}
}

void UPPPlayerActionRouterComponent::EndAssist()
{
	if (UPPCarryComponent* CarryComponent = GetOwner() ? GetOwner()->FindComponentByClass<UPPCarryComponent>() : nullptr)
	{
		CarryComponent->EndAssist();
	}
}
