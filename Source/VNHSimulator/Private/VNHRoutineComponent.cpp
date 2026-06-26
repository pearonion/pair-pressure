#include "VNHRoutineComponent.h"

#include "Net/UnrealNetwork.h"
#include "VNHShopperWaypoint.h"

UVNHRoutineComponent::UVNHRoutineComponent()
{
	PrimaryComponentTick.bCanEverTick = false;
	SetIsReplicatedByDefault(true);
}

void UVNHRoutineComponent::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);

	DOREPLIFETIME(UVNHRoutineComponent, Snapshot);
	DOREPLIFETIME(UVNHRoutineComponent, CurrentWaypointIndex);
}

AVNHShopperWaypoint* UVNHRoutineComponent::GetCurrentWaypoint() const
{
	if (!RoutineWaypoints.IsValidIndex(CurrentWaypointIndex))
	{
		return nullptr;
	}

	return RoutineWaypoints[CurrentWaypointIndex];
}

void UVNHRoutineComponent::SetRoutineWaypoints(const TArray<AVNHShopperWaypoint*>& NewWaypoints)
{
	if (!GetOwner() || !GetOwner()->HasAuthority())
	{
		return;
	}

	RoutineWaypoints.Reset(NewWaypoints.Num());
	for (AVNHShopperWaypoint* Waypoint : NewWaypoints)
	{
		if (IsValid(Waypoint))
		{
			RoutineWaypoints.Add(Waypoint);
		}
	}

	CurrentWaypointIndex = 0;
	if (AVNHShopperWaypoint* Waypoint = GetCurrentWaypoint())
	{
		Snapshot.Context = Waypoint->GetContext();
		Snapshot.SuggestedNextActivity = Waypoint->GetSuggestedNextActivity();
		Snapshot.HeldProp = Waypoint->GetHeldProp();
		BroadcastChanged();
	}
}

void UVNHRoutineComponent::AdvanceToNextWaypoint()
{
	if (!GetOwner() || !GetOwner()->HasAuthority() || RoutineWaypoints.IsEmpty())
	{
		return;
	}

	CurrentWaypointIndex = RoutineWaypoints.IsValidIndex(CurrentWaypointIndex + 1) ? CurrentWaypointIndex + 1 : 0;

	if (AVNHShopperWaypoint* Waypoint = GetCurrentWaypoint())
	{
		Snapshot.Context = Waypoint->GetContext();
		Snapshot.SuggestedNextActivity = Waypoint->GetSuggestedNextActivity();
		Snapshot.HeldProp = Waypoint->GetHeldProp();
		BroadcastChanged();
	}
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
