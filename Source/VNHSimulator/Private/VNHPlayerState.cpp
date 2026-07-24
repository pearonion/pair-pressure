#include "VNHPlayerState.h"

#include "Engine/DataTable.h"
#include "GameFramework/Controller.h"
#include "GameFramework/Pawn.h"
#include "Net/UnrealNetwork.h"
#include "PairPressure/PPGameplayTypes.h"
#include "PairPressure/Interfaces/PPMascotSelectionInterface.h"

AVNHPlayerState::AVNHPlayerState()
{
	bReplicates = true;
}

void AVNHPlayerState::BeginPlay()
{
	Super::BeginPlay();

	if (!HasAuthority() || !SelectedMascotRowName.IsNone())
	{
		return;
	}

	UDataTable* MascotTable = LoadObject<UDataTable>(
		nullptr,
		TEXT("/Game/PairPressure/Data/DT_MascotAnimations.DT_MascotAnimations"));
	TArray<FName> ValidMascotRows;
	if (MascotTable)
	{
		for (const FName RowName : MascotTable->GetRowNames())
		{
			const FPPMascotAnimationRow* MascotRow = MascotTable->FindRow<FPPMascotAnimationRow>(
				RowName,
				TEXT("Initialize random lobby mascot"),
				false);
			if (MascotRow && !MascotRow->Mesh.IsNull())
			{
				ValidMascotRows.Add(RowName);
			}
		}
	}

	const FName InitialMascotRow = ValidMascotRows.IsEmpty()
		? FName(TEXT("Penguin"))
		: ValidMascotRows[FMath::RandRange(0, ValidMascotRows.Num() - 1)];
	ApplySelectedMascotRowName_Implementation(InitialMascotRow);
}

void AVNHPlayerState::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);

	DOREPLIFETIME(AVNHPlayerState, AssignedRole);
	DOREPLIFETIME(AVNHPlayerState, LightErrandText);
	DOREPLIFETIME(AVNHPlayerState, bPreRoundReady);
	DOREPLIFETIME(AVNHPlayerState, LobbyTeamId);
	DOREPLIFETIME(AVNHPlayerState, SelectedMascotRowName);
}

void AVNHPlayerState::CopyProperties(APlayerState* TargetPlayerStateBase)
{
	Super::CopyProperties(TargetPlayerStateBase);
	if (AVNHPlayerState* TargetPlayerState = Cast<AVNHPlayerState>(TargetPlayerStateBase))
	{
		TargetPlayerState->SelectedMascotRowName = SelectedMascotRowName;
	}
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

FName AVNHPlayerState::GetSelectedMascotRowName_Implementation() const
{
	return SelectedMascotRowName.IsNone() ? FName(TEXT("Penguin")) : SelectedMascotRowName;
}

void AVNHPlayerState::ApplySelectedMascotRowName_Implementation(FName InMascotRowName)
{
	if (!HasAuthority() || InMascotRowName.IsNone() || SelectedMascotRowName == InMascotRowName)
	{
		return;
	}

	SelectedMascotRowName = InMascotRowName;
	ForceNetUpdate();
	OnRep_SelectedMascotRowName();
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

void AVNHPlayerState::OnRep_SelectedMascotRowName()
{
	AController* OwningController = Cast<AController>(GetOwner());
	APawn* ControlledPawn = OwningController ? OwningController->GetPawn() : nullptr;
	if (ControlledPawn && ControlledPawn->GetClass()->ImplementsInterface(UPPMascotSelectionInterface::StaticClass()))
	{
		IPPMascotSelectionInterface::Execute_ApplySelectedMascotRowName(
			ControlledPawn,
			GetSelectedMascotRowName_Implementation());
	}
}
