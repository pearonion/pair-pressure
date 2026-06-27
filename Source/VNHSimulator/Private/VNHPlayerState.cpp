#include "VNHPlayerState.h"

#include "Net/UnrealNetwork.h"

AVNHPlayerState::AVNHPlayerState()
{
	bReplicates = true;
}

void AVNHPlayerState::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);

	DOREPLIFETIME(AVNHPlayerState, AssignedRole);
	DOREPLIFETIME(AVNHPlayerState, LightErrandText);
}

void AVNHPlayerState::SetRole(EVNHPlayerRole NewRole)
{
	if (HasAuthority() && AssignedRole != NewRole)
	{
		AssignedRole = NewRole;
		OnRep_AssignedRole();
	}
}

void AVNHPlayerState::SetLightErrandText(const FText& NewLightErrandText)
{
	if (HasAuthority())
	{
		LightErrandText = NewLightErrandText;
	}
}

FText AVNHPlayerState::GetRoleDisplayText() const
{
	switch (AssignedRole)
	{
	case EVNHPlayerRole::Human:
		return NSLOCTEXT("VNH", "RoleHuman", "Human");
	case EVNHPlayerRole::Alien:
		return NSLOCTEXT("VNH", "RoleAlien", "Alien");
	case EVNHPlayerRole::Hunter:
		return NSLOCTEXT("VNH", "RoleHunter", "Hunter");
	default:
		return NSLOCTEXT("VNH", "RoleUnassigned", "Unassigned");
	}
}

FText AVNHPlayerState::GetPrivateRoleRevealText() const
{
	switch (AssignedRole)
	{
	case EVNHPlayerRole::Human:
		return NSLOCTEXT("VNH", "RoleRevealHuman", "You are Human");
	case EVNHPlayerRole::Alien:
		return NSLOCTEXT("VNH", "RoleRevealAlien", "You are the Alien");
	case EVNHPlayerRole::Hunter:
		return NSLOCTEXT("VNH", "RoleRevealHunter", "You are the Hunter");
	default:
		return NSLOCTEXT("VNH", "RoleRevealUnassigned", "Waiting for role");
	}
}

FText AVNHPlayerState::GetRoleGoalText() const
{
	switch (AssignedRole)
	{
	case EVNHPlayerRole::Human:
		return NSLOCTEXT("VNH", "RoleGoalHuman", "Blend in. Finish your errand. Avoid false accusation.");
	case EVNHPlayerRole::Alien:
		return NSLOCTEXT("VNH", "RoleGoalAlien", "Blend in as Human. Survive the Hunter's accusation.");
	case EVNHPlayerRole::Hunter:
		return NSLOCTEXT("VNH", "RoleGoalHunter", "Use commands, one question, and one accusation to find the Alien.");
	default:
		return NSLOCTEXT("VNH", "RoleGoalUnassigned", "Wait for the host to start the round.");
	}
}

void AVNHPlayerState::OnRep_AssignedRole()
{
}
