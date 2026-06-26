#include "VNHGameMode.h"

#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "TimerManager.h"
#include "VNHGameState.h"
#include "VNHPlayerState.h"

AVNHGameMode::AVNHGameMode()
{
	GameStateClass = AVNHGameState::StaticClass();
	PlayerStateClass = AVNHPlayerState::StaticClass();
}

void AVNHGameMode::BeginPlay()
{
	Super::BeginPlay();
	TryStartRound();
}

void AVNHGameMode::PostLogin(APlayerController* NewPlayer)
{
	Super::PostLogin(NewPlayer);
	TryStartRound();
}

void AVNHGameMode::Logout(AController* Exiting)
{
	Super::Logout(Exiting);

	if (AVNHGameState* VNHGameState = GetVNHGameState())
	{
		VNHGameState->SetRoundPhase(EVNHRoundPhase::WaitingForPlayers, 0.0f);
	}
}

void AVNHGameMode::TryStartRound()
{
	if (CountConnectedPlayers() < RequiredPlayers)
	{
		if (AVNHGameState* VNHGameState = GetVNHGameState())
		{
			VNHGameState->SetRoundPhase(EVNHRoundPhase::WaitingForPlayers, 0.0f);
		}
		return;
	}

	AssignRoles();

	if (AVNHGameState* VNHGameState = GetVNHGameState())
	{
		VNHGameState->SetRoundNumber(VNHGameState->GetRoundNumber() + 1);
		VNHGameState->SetTestsRemaining(2);
	}

	EnterPhase(EVNHRoundPhase::AssigningRoles, 3.0f);
}

void AVNHGameMode::AdvanceRoundPhase()
{
	AVNHGameState* VNHGameState = GetVNHGameState();
	if (!VNHGameState)
	{
		return;
	}

	switch (VNHGameState->GetRoundPhase())
	{
	case EVNHRoundPhase::AssigningRoles:
		EnterPhase(EVNHRoundPhase::AlienSetup, PhaseTiming.AlienSetupSeconds);
		break;
	case EVNHRoundPhase::AlienSetup:
		EnterPhase(EVNHRoundPhase::AlienHeadStart, PhaseTiming.AlienHeadStartSeconds);
		break;
	case EVNHRoundPhase::AlienHeadStart:
		EnterPhase(EVNHRoundPhase::Investigation, PhaseTiming.InvestigationSeconds);
		break;
	case EVNHRoundPhase::Investigation:
		EnterPhase(EVNHRoundPhase::Accusation, PhaseTiming.AccusationSeconds);
		break;
	case EVNHRoundPhase::Accusation:
		EnterPhase(EVNHRoundPhase::Reveal, PhaseTiming.RevealSeconds);
		break;
	case EVNHRoundPhase::Reveal:
		EnterPhase(EVNHRoundPhase::Resetting, 3.0f);
		break;
	case EVNHRoundPhase::Resetting:
		TryStartRound();
		break;
	default:
		break;
	}
}

AVNHGameState* AVNHGameMode::GetVNHGameState() const
{
	return GetGameState<AVNHGameState>();
}

void AVNHGameMode::AssignRoles()
{
	TArray<AVNHPlayerState*> PlayerStates;
	for (APlayerState* PlayerState : GameState->PlayerArray)
	{
		if (AVNHPlayerState* VNHPlayerState = Cast<AVNHPlayerState>(PlayerState))
		{
			PlayerStates.Add(VNHPlayerState);
		}
	}

	if (PlayerStates.Num() < RequiredPlayers)
	{
		return;
	}

	const int32 AlienIndex = FMath::RandRange(0, PlayerStates.Num() - 1);
	for (int32 Index = 0; Index < PlayerStates.Num(); ++Index)
	{
		PlayerStates[Index]->SetRole(Index == AlienIndex ? EVNHPlayerRole::Alien : EVNHPlayerRole::Hunter);
	}
}

void AVNHGameMode::EnterPhase(EVNHRoundPhase NewPhase, float DurationSeconds)
{
	AVNHGameState* VNHGameState = GetVNHGameState();
	if (!VNHGameState)
	{
		return;
	}

	GetWorldTimerManager().ClearTimer(PhaseTimerHandle);

	const float EndTime = DurationSeconds > 0.0f ? GetWorld()->GetTimeSeconds() + DurationSeconds : 0.0f;
	VNHGameState->SetRoundPhase(NewPhase, EndTime);

	if (DurationSeconds > 0.0f)
	{
		GetWorldTimerManager().SetTimer(PhaseTimerHandle, this, &AVNHGameMode::AdvanceRoundPhase, DurationSeconds, false);
	}
}

int32 AVNHGameMode::CountConnectedPlayers() const
{
	return GameState ? GameState->PlayerArray.Num() : 0;
}
