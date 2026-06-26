#include "VNHPlayerState.h"

#include "Net/UnrealNetwork.h"

AVNHPlayerState::AVNHPlayerState()
{
	bReplicates = true;
}

void AVNHPlayerState::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);

	DOREPLIFETIME(AVNHPlayerState, Role);
}

void AVNHPlayerState::SetRole(EVNHPlayerRole NewRole)
{
	if (HasAuthority() && Role != NewRole)
	{
		Role = NewRole;
		OnRep_Role();
	}
}

void AVNHPlayerState::OnRep_Role()
{
}
