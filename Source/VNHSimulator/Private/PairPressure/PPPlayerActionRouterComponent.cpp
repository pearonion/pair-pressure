#include "PairPressure/PPPlayerActionRouterComponent.h"

#include "PairPressure/PPCarryComponent.h"
#include "PairPressure/PPGrabberComponent.h"
#include "PairPressure/PPPhysicalStateComponent.h"
#include "GameFramework/Character.h"
#include "GameFramework/CharacterMovementComponent.h"

UPPPlayerActionRouterComponent::UPPPlayerActionRouterComponent()
{
	PrimaryComponentTick.bCanEverTick = false;
	SetIsReplicatedByDefault(true);
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
}

bool UPPPlayerActionRouterComponent::CanAirDive() const
{
	const ACharacter* OwnerCharacter = Cast<ACharacter>(GetOwner());
	const UCharacterMovementComponent* OwnerMovement = OwnerCharacter ? OwnerCharacter->GetCharacterMovement() : nullptr;
	const UPPGrabberComponent* GrabberComponent = GetOwner()
		? GetOwner()->FindComponentByClass<UPPGrabberComponent>()
		: nullptr;
	return bAirDiveArmed && OwnerMovement && OwnerMovement->IsFalling()
		&& (!GrabberComponent || GrabberComponent->CanJumpOrDive());
}

void UPPPlayerActionRouterComponent::ServerNotifyJumpStarted_Implementation()
{
	ArmAirDive();
}

void UPPPlayerActionRouterComponent::ServerRequestDive_Implementation()
{
	if (CanAirDive())
	{
		PerformDiveAuthoritative();
	}
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
	if (UPPPhysicalStateComponent* PhysicalState = UPPPhysicalStateComponent::FindPhysicalStateComponent(GetOwner()))
	{
		PhysicalState->RequestRagdoll_Implementation(10.0f);
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
