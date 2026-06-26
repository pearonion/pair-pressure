#include "VNHRoutineComponent.h"

#include "Net/UnrealNetwork.h"

UVNHRoutineComponent::UVNHRoutineComponent()
{
	PrimaryComponentTick.bCanEverTick = false;
	SetIsReplicatedByDefault(true);
}

void UVNHRoutineComponent::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);

	DOREPLIFETIME(UVNHRoutineComponent, Snapshot);
}

void UVNHRoutineComponent::SetContext(EVNHShopperContext NewContext, FName NewSuggestedNextActivity)
{
	if (!GetOwner() || !GetOwner()->HasAuthority())
	{
		return;
	}

	Snapshot.Context = NewContext;
	Snapshot.SuggestedNextActivity = NewSuggestedNextActivity;
	BroadcastChanged();
}

void UVNHRoutineComponent::SetHeldProp(FName NewHeldProp)
{
	if (!GetOwner() || !GetOwner()->HasAuthority())
	{
		return;
	}

	Snapshot.HeldProp = NewHeldProp;
	BroadcastChanged();
}

void UVNHRoutineComponent::PauseRoutineForPossession()
{
	if (!GetOwner() || !GetOwner()->HasAuthority())
	{
		return;
	}

	Snapshot.bPausedForPossession = true;
	BroadcastChanged();
}

void UVNHRoutineComponent::ResumeRoutineAfterPossession()
{
	if (!GetOwner() || !GetOwner()->HasAuthority())
	{
		return;
	}

	Snapshot.bPausedForPossession = false;
	BroadcastChanged();
}

EVNHActNaturalRecovery UVNHRoutineComponent::ChooseActNaturalRecovery() const
{
	switch (Snapshot.Context)
	{
	case EVNHShopperContext::Browsing:
		return EVNHActNaturalRecovery::InspectTag;
	case EVNHShopperContext::Mirror:
		return EVNHActNaturalRecovery::CheckAppearance;
	case EVNHShopperContext::Checkout:
		return EVNHActNaturalRecovery::AdjustClothing;
	case EVNHShopperContext::Reacting:
		return EVNHActNaturalRecovery::AdjustClothing;
	default:
		break;
	}

	if (Snapshot.HeldProp == TEXT("Phone"))
	{
		return EVNHActNaturalRecovery::CheckPhone;
	}

	if (Snapshot.HeldProp == TEXT("Drink"))
	{
		return EVNHActNaturalRecovery::TakeSip;
	}

	return EVNHActNaturalRecovery::NeutralIdle;
}

void UVNHRoutineComponent::OnRep_Snapshot()
{
	BroadcastChanged();
}

void UVNHRoutineComponent::BroadcastChanged()
{
	OnRoutineChanged.Broadcast(Snapshot);
}
