#include "PairPressure/PPPlayerActionRouterComponent.h"

#include "PairPressure/PPCarryComponent.h"
#include "PairPressure/PPPhysicalStateComponent.h"

UPPPlayerActionRouterComponent::UPPPlayerActionRouterComponent()
{
	PrimaryComponentTick.bCanEverTick = false;
}

void UPPPlayerActionRouterComponent::RequestDive()
{
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
