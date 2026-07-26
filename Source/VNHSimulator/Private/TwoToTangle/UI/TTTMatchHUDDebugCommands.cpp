#include "CoreMinimal.h"

#if !UE_BUILD_SHIPPING

#include "GameFramework/GameStateBase.h"
#include "GameFramework/PlayerController.h"
#include "HAL/IConsoleManager.h"
#include "PairPressure/PPPhysicalStateComponent.h"
#include "PairPressure/PPTeamMemberComponent.h"
#include "TwoToTangle/Gameplay/Race/TTTRaceComponents.h"
#include "TwoToTangle/UI/Components/TTTMatchHUDPresenterComponent.h"
#include "TwoToTangle/UI/Components/TTTPlayerHUDSourceComponent.h"

namespace
{
UTTTPlayerHUDSourceComponent* TTTMatchHUDDebugFindSource(UWorld* World)
{
	APlayerController* PlayerController = World ? World->GetFirstPlayerController() : nullptr;
	APawn* Pawn = PlayerController ? PlayerController->GetPawn() : nullptr;
	return Pawn ? Pawn->FindComponentByClass<UTTTPlayerHUDSourceComponent>() : nullptr;
}

UPPPhysicalStateComponent* TTTMatchHUDDebugFindPhysicalState(UWorld* World)
{
	APlayerController* PlayerController = World ? World->GetFirstPlayerController() : nullptr;
	return PlayerController && PlayerController->GetPawn()
		? PlayerController->GetPawn()->FindComponentByClass<UPPPhysicalStateComponent>()
		: nullptr;
}

FAutoConsoleCommandWithWorldAndArgs TTTMatchHUDToggleCommand(
	TEXT("ttt.ui.hud.toggle"), TEXT("Toggle the local Two to Tangle match HUD."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>&, UWorld* World)
	{
		if (APlayerController* PlayerController = World ? World->GetFirstPlayerController() : nullptr)
		{
			if (UTTTMatchHUDPresenterComponent* Presenter = PlayerController->FindComponentByClass<UTTTMatchHUDPresenterComponent>()) Presenter->ToggleHUD();
		}
	}));

FAutoConsoleCommandWithWorldAndArgs TTTMatchHUDCountdownCommand(
	TEXT("ttt.ui.countdown.test"), TEXT("Start the authoritative 3-2-1-GO countdown."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>&, UWorld* World)
	{
		if (AGameStateBase* GameState = World ? World->GetGameState() : nullptr)
		{
			if (UTTTRaceStateComponent* RaceState = GameState->FindComponentByClass<UTTTRaceStateComponent>()) RaceState->StartCountdown(3.0f);
		}
	}));

FAutoConsoleCommandWithWorldAndArgs TTTMatchHUDFinishFirstCommand(
	TEXT("ttt.ui.finish.first"), TEXT("Present a local first-place result."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>&, UWorld* World)
	{
		if (UTTTPlayerHUDSourceComponent* Source = TTTMatchHUDDebugFindSource(World)) Source->DebugPresentFinish(true, false);
	}));

FAutoConsoleCommandWithWorldAndArgs TTTMatchHUDEliminatedCommand(
	TEXT("ttt.ui.finish.eliminated"), TEXT("Present a local eliminated result."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>&, UWorld* World)
	{
		if (UTTTPlayerHUDSourceComponent* Source = TTTMatchHUDDebugFindSource(World)) Source->DebugPresentFinish(false, true);
	}));

FAutoConsoleCommandWithWorldAndArgs TTTMatchHUDGiveFootballCommand(
	TEXT("ttt.ui.item.givefootball"), TEXT("Present the football item slot for HUD testing."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>&, UWorld* World)
	{
		if (UTTTPlayerHUDSourceComponent* Source = TTTMatchHUDDebugFindSource(World)) Source->DebugPresentHeldItem(NSLOCTEXT("TwoToTangle", "DebugFootball", "FOOTBALL"));
	}));

FAutoConsoleCommandWithWorldAndArgs TTTMatchHUDThrowChargeCommand(
	TEXT("ttt.ui.throwcharge.set"), TEXT("Set contextual throw charge, e.g. ttt.ui.throwcharge.set 0.75."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World)
	{
		const float Charge = Args.IsEmpty() ? 0.0f : FCString::Atof(*Args[0]);
		if (UTTTPlayerHUDSourceComponent* Source = TTTMatchHUDDebugFindSource(World)) Source->SetThrowChargeState(Charge > 0.0f, Charge);
	}));

FAutoConsoleCommandWithWorldAndArgs TTTMatchHUDDazeSetCommand(
	TEXT("ttt.daze.set"), TEXT("Set authoritative normalized Daze, e.g. ttt.daze.set 0.5."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World)
	{
		const float Daze = Args.IsEmpty() ? 0.0f : FCString::Atof(*Args[0]);
		if (UPPPhysicalStateComponent* PhysicalState = TTTMatchHUDDebugFindPhysicalState(World)) PhysicalState->DebugSetDazeNormalized(Daze);
	}));

FAutoConsoleCommandWithWorldAndArgs TTTMatchHUDDazeFullCommand(
	TEXT("ttt.daze.full"), TEXT("Set the local authoritative player to fully Dazed."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>&, UWorld* World)
	{
		if (UPPPhysicalStateComponent* PhysicalState = TTTMatchHUDDebugFindPhysicalState(World)) PhysicalState->DebugSetDazeNormalized(1.0f);
	}));

FAutoConsoleCommandWithWorldAndArgs TTTMatchHUDDazeRecoverCommand(
	TEXT("ttt.daze.recover"), TEXT("Request recovery for the local authoritative player."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>&, UWorld* World)
	{
		if (UPPPhysicalStateComponent* PhysicalState = TTTMatchHUDDebugFindPhysicalState(World)) PhysicalState->RequestDebugRecovery();
	}));

FAutoConsoleCommandWithWorldAndArgs TTTMatchHUDCarrySimulateCommand(
	TEXT("ttt.carry.simulate"), TEXT("Simulate carried recovery for the local authoritative player."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>&, UWorld* World)
	{
		APlayerController* PlayerController = World ? World->GetFirstPlayerController() : nullptr;
		APawn* Pawn = PlayerController ? PlayerController->GetPawn() : nullptr;
		UPPPhysicalStateComponent* PhysicalState = Pawn ? Pawn->FindComponentByClass<UPPPhysicalStateComponent>() : nullptr;
		UPPTeamMemberComponent* TeamMember = Pawn ? Pawn->FindComponentByClass<UPPTeamMemberComponent>() : nullptr;
		if (PhysicalState && TeamMember)
		{
			PhysicalState->DebugSetDazeNormalized(1.0f);
			PhysicalState->OnCarryStarted_Implementation(TeamMember->GetAssignedPartner());
		}
	}));

FAutoConsoleCommandWithWorldAndArgs TTTMatchHUDPlacementCommand(
	TEXT("ttt.race.place"), TEXT("Set local HUD placement, e.g. ttt.race.place 2 3."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World)
	{
		const int32 Place = Args.Num() > 0 ? FCString::Atoi(*Args[0]) : 1;
		const int32 Teams = Args.Num() > 1 ? FCString::Atoi(*Args[1]) : 3;
		if (UTTTPlayerHUDSourceComponent* Source = TTTMatchHUDDebugFindSource(World)) Source->DebugSetPlacement(Place, Teams);
	}));

FAutoConsoleCommandWithWorldAndArgs TTTMatchHUDClockCommand(
	TEXT("ttt.race.clock"), TEXT("Set local HUD clock, e.g. ttt.race.clock 84.56."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World)
	{
		const float TimeSeconds = Args.IsEmpty() ? 0.0f : FCString::Atof(*Args[0]);
		if (UTTTPlayerHUDSourceComponent* Source = TTTMatchHUDDebugFindSource(World)) Source->DebugSetRaceClock(TimeSeconds);
	}));
}

#endif
