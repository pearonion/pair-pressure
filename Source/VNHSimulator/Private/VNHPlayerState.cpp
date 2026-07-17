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
	DOREPLIFETIME(AVNHPlayerState, bPreRoundReady);
	DOREPLIFETIME(AVNHPlayerState, LobbyTeamId);
}

void AVNHPlayerState::SetRole(EVNHPlayerRole NewRole)
{
	if (AssignedRole != NewRole)
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

void AVNHPlayerState::SetPreRoundReady(bool bNewPreRoundReady)
{
	if (HasAuthority())
	{
		bPreRoundReady = bNewPreRoundReady;
	}
}

void AVNHPlayerState::SetLobbyTeamId(int32 NewLobbyTeamId)
{
	if (HasAuthority())
	{
		LobbyTeamId = NewLobbyTeamId;
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
		return NSLOCTEXT("VNH", "RoleGoalHuman", "Be a real civilian. Finish your errand, act natural, and survive suspicion.");
	case EVNHPlayerRole::Alien:
		return NSLOCTEXT("VNH", "RoleGoalAlien", "You are pretending. Do not get accused. Use the same public controls as Human.");
	case EVNHPlayerRole::Hunter:
		return NSLOCTEXT("VNH", "RoleGoalHunter", "Use commands, three questions, and one accusation to find the Alien.");
	default:
		return NSLOCTEXT("VNH", "RoleGoalUnassigned", "Wait for the host to start the round.");
	}
}

void AVNHPlayerState::OnRep_AssignedRole()
{
}
