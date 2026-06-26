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
}

void AVNHPlayerState::SetRole(EVNHPlayerRole NewRole)
{
	if (HasAuthority() && AssignedRole != NewRole)
	{
		AssignedRole = NewRole;
		OnRep_AssignedRole();
	}
}

void AVNHPlayerState::OnRep_AssignedRole()
{
}
