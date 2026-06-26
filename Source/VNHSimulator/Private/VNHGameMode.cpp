#include "VNHGameMode.h"

#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/PlayerController.h"
#include "TimerManager.h"
#include "VNHGameState.h"
#include "VNHPlayerController.h"
#include "VNHPlayerState.h"
#include "VNHShopperCharacter.h"

AVNHGameMode::AVNHGameMode()
{
	GameStateClass = AVNHGameState::StaticClass();
	PlayerControllerClass = AVNHPlayerController::StaticClass();
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
		VNHGameState->ClearRoundOutcome();
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
		PossessAlienShopper();
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

void AVNHGameMode::RequestPublicTest(AVNHPlayerController* RequestingPlayer, EVNHPublicTestType TestType)
{
	AVNHGameState* VNHGameState = GetVNHGameState();
	if (!VNHGameState || !IsHunterController(RequestingPlayer))
	{
		return;
	}

	if (VNHGameState->GetRoundPhase() != EVNHRoundPhase::Investigation || VNHGameState->GetTestsRemaining() <= 0)
	{
		return;
	}

	VNHGameState->SetTestsRemaining(VNHGameState->GetTestsRemaining() - 1);
	VNHGameState->SetActivePublicTest(TestType);
	ApplyPublicTestToShoppers(TestType);
}

void AVNHGameMode::RequestAccusation(AVNHPlayerController* RequestingPlayer, AVNHShopperCharacter* AccusedShopper)
{
	AVNHGameState* VNHGameState = GetVNHGameState();
	if (!VNHGameState || !IsHunterController(RequestingPlayer) || !AccusedShopper)
	{
		return;
	}

	const EVNHRoundPhase CurrentPhase = VNHGameState->GetRoundPhase();
	if (CurrentPhase != EVNHRoundPhase::Investigation && CurrentPhase != EVNHRoundPhase::Accusation)
	{
		return;
	}

	FVNHAccusationResult Result;
	Result.AccusedActor = AccusedShopper;
	Result.bCorrect = AccusedShopper->IsPossessedByAlien();
	Result.bResolved = true;
	VNHGameState->SetAccusationResult(Result);

	EnterPhase(EVNHRoundPhase::Reveal, PhaseTiming.RevealSeconds);
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

bool AVNHGameMode::IsHunterController(const APlayerController* PlayerController) const
{
	if (!PlayerController)
	{
		return false;
	}

	const AVNHPlayerState* VNHPlayerState = PlayerController->GetPlayerState<AVNHPlayerState>();
	return VNHPlayerState && VNHPlayerState->IsHunter();
}

APlayerController* AVNHGameMode::FindControllerForRole(EVNHPlayerRole TargetRole) const
{
	for (FConstPlayerControllerIterator It = GetWorld()->GetPlayerControllerIterator(); It; ++It)
	{
		APlayerController* PlayerController = It->Get();
		const AVNHPlayerState* VNHPlayerState = PlayerController ? PlayerController->GetPlayerState<AVNHPlayerState>() : nullptr;
		if (VNHPlayerState && VNHPlayerState->GetRole() == TargetRole)
		{
			return PlayerController;
		}
	}

	return nullptr;
}

AVNHShopperCharacter* AVNHGameMode::SelectAlienShopper() const
{
	TArray<AVNHShopperCharacter*> EligibleShoppers;
	for (TActorIterator<AVNHShopperCharacter> It(GetWorld()); It; ++It)
	{
		if (!It->IsPossessedByAlien())
		{
			EligibleShoppers.Add(*It);
		}
	}

	if (EligibleShoppers.IsEmpty())
	{
		return nullptr;
	}

	return EligibleShoppers[FMath::RandRange(0, EligibleShoppers.Num() - 1)];
}

void AVNHGameMode::PossessAlienShopper()
{
	AVNHGameState* VNHGameState = GetVNHGameState();
	APlayerController* AlienController = FindControllerForRole(EVNHPlayerRole::Alien);
	AVNHShopperCharacter* Shopper = SelectAlienShopper();
	if (!VNHGameState || !AlienController || !Shopper)
	{
		return;
	}

	Shopper->SetPossessedByAlien(true);
	AlienController->Possess(Shopper);
	VNHGameState->SetPossessedShopper(Shopper);
}

void AVNHGameMode::ApplyPublicTestToShoppers(EVNHPublicTestType TestType)
{
	for (TActorIterator<AVNHShopperCharacter> It(GetWorld()); It; ++It)
	{
		It->ApplyPublicTest(TestType);
	}
}
