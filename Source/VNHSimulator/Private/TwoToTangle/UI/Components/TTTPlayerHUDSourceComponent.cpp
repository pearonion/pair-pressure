#include "TwoToTangle/UI/Components/TTTPlayerHUDSourceComponent.h"

#include "Engine/World.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/GameStateBase.h"
#include "PairPressure/PPCarryComponent.h"
#include "PairPressure/PPGrabberComponent.h"
#include "PairPressure/PPPhysicalStateComponent.h"
#include "PairPressure/PPTeamMemberComponent.h"
#include "TimerManager.h"
#include "TwoToTangle/Gameplay/Items/TTTThrowableItemInterface.h"
#include "TwoToTangle/Gameplay/Race/TTTRaceComponents.h"
#include "TwoToTangle/UI/Components/TTTMatchHUDPresenterComponent.h"
#include "TwoToTangle/UI/Data/TTTMatchHUDConfig.h"

UTTTPlayerHUDSourceComponent::UTTTPlayerHUDSourceComponent()
{
	PrimaryComponentTick.bCanEverTick = false;
	SetIsReplicatedByDefault(false);
}

void UTTTPlayerHUDSourceComponent::BeginPlay()
{
	Super::BeginPlay();
	const APawn* OwnerPawn = Cast<APawn>(GetOwner());
	const APlayerController* LocalController = OwnerPawn
		? Cast<APlayerController>(OwnerPawn->GetController())
		: nullptr;
	const UTTTMatchHUDPresenterComponent* Presenter = LocalController
		? LocalController->FindComponentByClass<UTTTMatchHUDPresenterComponent>()
		: nullptr;
	const bool bShouldRunPresentation = GetWorld()
		&& GetWorld()->GetMapName().Contains(TEXT("PP_"))
		&& OwnerPawn && OwnerPawn->IsLocallyControlled()
		&& Presenter && Presenter->IsMatchHUDEnabled();
	if (!bShouldRunPresentation)
	{
		return;
	}

	if (!HUDConfig)
	{
		HUDConfig = LoadObject<UTTTMatchHUDConfig>(nullptr, TEXT("/Game/PairPressure/UI/Styles/DA_MatchHUDConfig.DA_MatchHUDConfig"));
	}
	ResolveSources();
	BindSources();
	RefreshAllPresentation();
	if (GetWorld())
	{
		const float RefreshRate = HUDConfig ? HUDConfig->StopwatchRefreshRate : 20.0f;
		GetWorld()->GetTimerManager().SetTimer(PresentationClockTimerHandle, this, &UTTTPlayerHUDSourceComponent::UpdatePresentationClock, 1.0f / FMath::Max(1.0f, RefreshRate), true);
	}
}

void UTTTPlayerHUDSourceComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	UnbindSources();
	if (GetWorld()) GetWorld()->GetTimerManager().ClearTimer(PresentationClockTimerHandle);
	Super::EndPlay(EndPlayReason);
}

FTTTMatchHUDSnapshot UTTTPlayerHUDSourceComponent::GetMatchHUDSnapshot_Implementation() const
{
	FTTTMatchHUDSnapshot Snapshot;
	Snapshot.RacePhase = RaceStateSource ? RaceStateSource->GetRacePhase() : ETTTRacePhase::Waiting;
	Snapshot.RaceElapsedSeconds = GetRaceElapsedSeconds_Implementation();
	Snapshot.Placement = GetCurrentPlacement_Implementation();
	Snapshot.TeamCount = GetTeamCount_Implementation();
	const FTTTHeldItemPresentation ItemData = GetHeldItemPresentation_Implementation();
	Snapshot.bHasHeldItem = ItemData.bHasItem;
	Snapshot.HeldItemIcon = ItemData.Icon;
	Snapshot.HeldItemName = ItemData.DisplayName;
	Snapshot.bIsChargingThrow = bThrowChargeActive;
	Snapshot.ThrowChargeNormalized = ThrowChargeNormalized;
	Snapshot.DazeNormalized = GetDazeNormalized_Implementation();
	Snapshot.DazeRecoveryThresholdNormalized = GetDazeRecoveryThresholdNormalized_Implementation();
	Snapshot.PhysicalState = GetPhysicalState_Implementation();
	Snapshot.bCanMove = CanLocalPlayerMove_Implementation();
	Snapshot.bCanDismount = CanLocalPlayerDismount_Implementation();
	Snapshot.TeamId = ResolveTeamId();
	FTTTTeamRaceResolution TeamResolution;
	if (FinishSource && FinishSource->GetTeamResolution(Snapshot.TeamId, TeamResolution))
	{
		Snapshot.RacePhase = TeamResolution.bEliminated ? ETTTRacePhase::Eliminated : ETTTRacePhase::TeamFinished;
		Snapshot.FinalTeamTime = TeamResolution.OfficialTime;
		Snapshot.FinishResult = TeamResolution.bEliminated
			? ETTTFinishResult::DidNotFinish
			: Snapshot.Placement == 1 ? ETTTFinishResult::First
			: Snapshot.Placement == 2 ? ETTTFinishResult::Second
			: Snapshot.Placement == 3 ? ETTTFinishResult::Third
			: ETTTFinishResult::Finished;
		Snapshot.bWaitingForResults = TeamResolution.bEliminated;
	}
	if (Snapshot.PhysicalState == ETTTPhysicalState::BeingCarried) Snapshot.ActiveMessage = Snapshot.bCanDismount ? ETTTHUDMessageType::Recovered : ETTTHUDMessageType::BeingCarried;
	else if (Snapshot.PhysicalState == ETTTPhysicalState::FullyDazed || Snapshot.PhysicalState == ETTTPhysicalState::Unconscious) Snapshot.ActiveMessage = ETTTHUDMessageType::DazedNeedsTeammate;
	return Snapshot;
}

float UTTTPlayerHUDSourceComponent::GetRaceElapsedSeconds_Implementation() const
{
	if (DebugClockOverride >= 0.0f) return DebugClockOverride;
	FTTTTeamRaceResolution TeamResolution;
	if (FinishSource && FinishSource->GetTeamResolution(ResolveTeamId(), TeamResolution))
	{
		return TeamResolution.OfficialTime;
	}
	FTTTTeamRaceEntry TeamEntry;
	if (RankingSource && RankingSource->GetTeamRaceEntry(ResolveTeamId(), TeamEntry) && TeamEntry.bFinished)
	{
		return TeamEntry.OfficialFinishTime;
	}
	return RaceClockSource ? RaceClockSource->GetRaceElapsedSeconds() : 0.0f;
}

int32 UTTTPlayerHUDSourceComponent::GetCurrentPlacement_Implementation() const
{
	if (DebugPlacementOverride > 0) return DebugPlacementOverride;
	return RankingSource ? RankingSource->GetPlacementForTeam(ResolveTeamId()) : 1;
}

int32 UTTTPlayerHUDSourceComponent::GetTeamCount_Implementation() const
{
	if (DebugTeamCountOverride > 0) return DebugTeamCountOverride;
	return RankingSource ? FMath::Max(1, RankingSource->GetTeamCount()) : 1;
}

float UTTTPlayerHUDSourceComponent::GetDazeNormalized_Implementation() const
{
	return PhysicalStateSource ? PhysicalStateSource->GetDazeNormalized_Implementation() : 0.0f;
}

float UTTTPlayerHUDSourceComponent::GetDazeRecoveryThresholdNormalized_Implementation() const
{
	return HUDConfig ? HUDConfig->DazeRecoveryThreshold : 0.3f;
}

FTTTHeldItemPresentation UTTTPlayerHUDSourceComponent::GetHeldItemPresentation_Implementation() const
{
	if (DebugHeldItemOverride.bHasItem) return DebugHeldItemOverride;
	FTTTHeldItemPresentation ItemData;
	AActor* HeldActor = GrabberSource && GrabberSource->GetGrabState_Implementation() == EPPGrabState::HoldingItem ? GrabberSource->GetGrabTarget() : nullptr;
	if (!HeldActor) return ItemData;
	if (HeldActor->GetClass()->ImplementsInterface(UTTTThrowableItemInterface::StaticClass()))
	{
		return ITTTThrowableItemInterface::Execute_GetHeldItemPresentation(HeldActor);
	}
	ItemData.bHasItem = true;
	ItemData.DisplayName = FText::FromString(HeldActor->GetName());
	ItemData.bCanThrow = true;
	ItemData.bSupportsCharge = true;
	return ItemData;
}

ETTTPhysicalState UTTTPlayerHUDSourceComponent::GetPhysicalState_Implementation() const
{
	if (PhysicalStateSource && PhysicalStateSource->IsBeingCarried()) return ETTTPhysicalState::BeingCarried;
	return PhysicalStateSource ? MapPhysicalState(PhysicalStateSource->GetCurrentPhysicalState()) : ETTTPhysicalState::Upright;
}

bool UTTTPlayerHUDSourceComponent::CanLocalPlayerMove_Implementation() const
{
	FTTTTeamRaceResolution TeamResolution;
	if (FinishSource && FinishSource->GetTeamResolution(ResolveTeamId(), TeamResolution)) return false;
	if (RaceStateSource)
	{
		const ETTTRacePhase RacePhase = RaceStateSource->GetRacePhase();
		if (RacePhase == ETTTRacePhase::Countdown || RacePhase == ETTTRacePhase::TeamFinished
			|| RacePhase == ETTTRacePhase::Eliminated || RacePhase == ETTTRacePhase::ResultsTransition)
		{
			return false;
		}
	}
	const ETTTPhysicalState State = GetPhysicalState_Implementation();
	return State == ETTTPhysicalState::Upright || State == ETTTPhysicalState::Stumbling || State == ETTTPhysicalState::Recovering;
}

bool UTTTPlayerHUDSourceComponent::CanLocalPlayerDismount_Implementation() const
{
	return PhysicalStateSource && PhysicalStateSource->CanDismountCarry_Implementation();
}

void UTTTPlayerHUDSourceComponent::SetThrowChargeState(bool bCharging, float NormalizedCharge)
{
	const float SafeCharge = FMath::Clamp(NormalizedCharge, 0.0f, 1.0f);
	if (bCharging && !bThrowChargeActive) ThrowChargeStartedEvent.Broadcast();
	if (!bCharging && bThrowChargeActive) ThrowChargeEndedEvent.Broadcast();
	bThrowChargeActive = bCharging;
	ThrowChargeNormalized = bCharging ? SafeCharge : 0.0f;
	if (bCharging) ThrowChargeChangedEvent.Broadcast(ThrowChargeNormalized);
}

void UTTTPlayerHUDSourceComponent::RefreshAllPresentation()
{
	ResolveSources();
	const FTTTMatchHUDSnapshot Snapshot = GetMatchHUDSnapshot_Implementation();
	RacePhaseChangedEvent.Broadcast(Snapshot.RacePhase);
	RaceClockUpdatedEvent.Broadcast(Snapshot.RaceElapsedSeconds);
	PlacementChangedEvent.Broadcast(Snapshot.Placement, Snapshot.TeamCount);
	const FTTTHeldItemPresentation ItemData = GetHeldItemPresentation_Implementation();
	HeldItemChangedEvent.Broadcast(ItemData);
	DazeChangedEvent.Broadcast(Snapshot.DazeNormalized, Snapshot.DazeRecoveryThresholdNormalized);
	PhysicalStateChangedEvent.Broadcast(Snapshot.PhysicalState);
}

void UTTTPlayerHUDSourceComponent::DebugSetPlacement(int32 Placement, int32 TeamCount)
{
	DebugPlacementOverride = FMath::Max(1, Placement);
	DebugTeamCountOverride = FMath::Max(DebugPlacementOverride, TeamCount);
	PlacementChangedEvent.Broadcast(DebugPlacementOverride, DebugTeamCountOverride);
}

void UTTTPlayerHUDSourceComponent::DebugSetRaceClock(float ElapsedSeconds)
{
	DebugClockOverride = FMath::Max(0.0f, ElapsedSeconds);
	RaceClockUpdatedEvent.Broadcast(DebugClockOverride);
}

void UTTTPlayerHUDSourceComponent::DebugPresentFinish(bool bFirstPlace, bool bEliminated)
{
	FTTTFinishPresentation ResultData;
	ResultData.Placement = bFirstPlace ? 1 : GetCurrentPlacement_Implementation();
	ResultData.TeamCount = GetTeamCount_Implementation();
	ResultData.FinalTeamTime = GetRaceElapsedSeconds_Implementation();
	ResultData.Result = bEliminated ? ETTTFinishResult::DidNotFinish : (bFirstPlace ? ETTTFinishResult::First : ETTTFinishResult::Finished);
	bEliminated ? TeamEliminatedEvent.Broadcast(ResultData) : TeamFinishedEvent.Broadcast(ResultData);
}

void UTTTPlayerHUDSourceComponent::DebugPresentHeldItem(const FText& DisplayName)
{
	DebugHeldItemOverride.bHasItem = !DisplayName.IsEmpty();
	DebugHeldItemOverride.DisplayName = DisplayName;
	DebugHeldItemOverride.bCanThrow = DebugHeldItemOverride.bHasItem;
	DebugHeldItemOverride.bSupportsCharge = DebugHeldItemOverride.bHasItem;
	HeldItemChangedEvent.Broadcast(DebugHeldItemOverride);
}

void UTTTPlayerHUDSourceComponent::HandlePPPhysicalStateChanged(EPPPhysicalState NewState, float DazeNormalized)
{
	const ETTTPhysicalState MappedState = MapPhysicalState(NewState);
	PhysicalStateChangedEvent.Broadcast(MappedState);
	DazeChangedEvent.Broadcast(DazeNormalized, GetDazeRecoveryThresholdNormalized_Implementation());
	const bool bCarried = MappedState == ETTTPhysicalState::BeingCarried;
	CarryStateChangedEvent.Broadcast(bCarried, CanLocalPlayerDismount_Implementation());
	if (MappedState == ETTTPhysicalState::Upright) MovementRestoredEvent.Broadcast();
}

void UTTTPlayerHUDSourceComponent::HandlePPDazeChanged(float DazeNormalized)
{
	DazeChangedEvent.Broadcast(DazeNormalized, GetDazeRecoveryThresholdNormalized_Implementation());
	if (GetPhysicalState_Implementation() == ETTTPhysicalState::BeingCarried) CarryStateChangedEvent.Broadcast(true, CanLocalPlayerDismount_Implementation());
}

void UTTTPlayerHUDSourceComponent::HandlePPCarriedRecoveryChanged(bool bIsBeingCarried, bool bCanDismount)
{
	CarryStateChangedEvent.Broadcast(bIsBeingCarried, bCanDismount);
	PhysicalStateChangedEvent.Broadcast(GetPhysicalState_Implementation());
	if (!bIsBeingCarried && CanLocalPlayerMove_Implementation()) MovementRestoredEvent.Broadcast();
}

void UTTTPlayerHUDSourceComponent::HandlePPGrabStateChanged(EPPGrabState NewGrabState, AActor* NewTarget)
{
	PresentedHeldActor = NewGrabState == EPPGrabState::HoldingItem ? NewTarget : nullptr;
	HeldItemChangedEvent.Broadcast(GetHeldItemPresentation_Implementation());
	if (NewGrabState != EPPGrabState::HoldingItem) SetThrowChargeState(false, 0.0f);
}

void UTTTPlayerHUDSourceComponent::HandlePPAssistTargetChanged(AActor* NewAssistTarget)
{
	if (!NewAssistTarget) return;
	CarryStateChangedEvent.Broadcast(false, false);
}

void UTTTPlayerHUDSourceComponent::HandlePPTeamChanged(int32 ChangedTeamId, AActor* NewPartner)
{
	EnsureTeamRegistered();
	PlacementChangedEvent.Broadcast(GetCurrentPlacement_Implementation(), GetTeamCount_Implementation());
}

void UTTTPlayerHUDSourceComponent::HandleRacePhaseChanged(ETTTRacePhase NewPhase)
{
	LastCountdownValue = INDEX_NONE;
	RacePhaseChangedEvent.Broadcast(NewPhase);
}

void UTTTPlayerHUDSourceComponent::HandleRankingChanged(int32 ChangedTeamId, int32 NewPlacement)
{
	if (ChangedTeamId == ResolveTeamId()) PlacementChangedEvent.Broadcast(NewPlacement, GetTeamCount_Implementation());
}

void UTTTPlayerHUDSourceComponent::HandleTeamFinished(int32 FinishedTeamId, float OfficialTime)
{
	if (FinishedTeamId != ResolveTeamId()) return;
	FTTTFinishPresentation ResultData;
	ResultData.Placement = GetCurrentPlacement_Implementation();
	ResultData.TeamCount = GetTeamCount_Implementation();
	ResultData.FinalTeamTime = OfficialTime;
	ResultData.Result = ResultData.Placement == 1 ? ETTTFinishResult::First : ResultData.Placement == 2 ? ETTTFinishResult::Second : ResultData.Placement == 3 ? ETTTFinishResult::Third : ETTTFinishResult::Finished;
	TeamFinishedEvent.Broadcast(ResultData);
}

void UTTTPlayerHUDSourceComponent::HandleTeamEliminated(int32 EliminatedTeamId, float ResolutionTime)
{
	if (EliminatedTeamId != ResolveTeamId()) return;
	FTTTFinishPresentation ResultData;
	ResultData.Placement = GetCurrentPlacement_Implementation();
	ResultData.TeamCount = GetTeamCount_Implementation();
	ResultData.FinalTeamTime = ResolutionTime;
	ResultData.Result = ETTTFinishResult::DidNotFinish;
	ResultData.bWaitingForResults = true;
	TeamEliminatedEvent.Broadcast(ResultData);
}

void UTTTPlayerHUDSourceComponent::ResolveSources()
{
	AActor* OwnerActor = GetOwner();
	if (!OwnerActor) return;
	PhysicalStateSource = OwnerActor->FindComponentByClass<UPPPhysicalStateComponent>();
	TeamSource = OwnerActor->FindComponentByClass<UPPTeamMemberComponent>();
	GrabberSource = OwnerActor->FindComponentByClass<UPPGrabberComponent>();
	CarrySource = OwnerActor->FindComponentByClass<UPPCarryComponent>();
	if (AGameStateBase* GameState = GetWorld() ? GetWorld()->GetGameState() : nullptr)
	{
		RaceStateSource = GameState->FindComponentByClass<UTTTRaceStateComponent>();
		RaceClockSource = GameState->FindComponentByClass<UTTTRaceClockComponent>();
		RankingSource = GameState->FindComponentByClass<UTTTRaceRankingComponent>();
		FinishSource = GameState->FindComponentByClass<UTTTFinishTrackerComponent>();
	}
}

void UTTTPlayerHUDSourceComponent::BindSources()
{
	if (PhysicalStateSource)
	{
		PhysicalStateSource->OnPhysicalStateChanged.AddUniqueDynamic(this, &UTTTPlayerHUDSourceComponent::HandlePPPhysicalStateChanged);
		PhysicalStateSource->OnDazeChanged.AddUniqueDynamic(this, &UTTTPlayerHUDSourceComponent::HandlePPDazeChanged);
		PhysicalStateSource->OnCarriedRecoveryChanged.AddUniqueDynamic(this, &UTTTPlayerHUDSourceComponent::HandlePPCarriedRecoveryChanged);
	}
	if (GrabberSource) GrabberSource->OnGrabStateChanged.AddUniqueDynamic(this, &UTTTPlayerHUDSourceComponent::HandlePPGrabStateChanged);
	if (CarrySource) CarrySource->OnAssistTargetChanged.AddUniqueDynamic(this, &UTTTPlayerHUDSourceComponent::HandlePPAssistTargetChanged);
	if (TeamSource) TeamSource->OnPartnerChanged.AddUniqueDynamic(this, &UTTTPlayerHUDSourceComponent::HandlePPTeamChanged);
	if (RaceStateSource) RaceStateSource->OnRacePhaseChanged.AddUniqueDynamic(this, &UTTTPlayerHUDSourceComponent::HandleRacePhaseChanged);
	if (RankingSource) RankingSource->OnRankingChanged.AddUniqueDynamic(this, &UTTTPlayerHUDSourceComponent::HandleRankingChanged);
	if (FinishSource)
	{
		FinishSource->OnTeamFinished.AddUniqueDynamic(this, &UTTTPlayerHUDSourceComponent::HandleTeamFinished);
		FinishSource->OnTeamEliminated.AddUniqueDynamic(this, &UTTTPlayerHUDSourceComponent::HandleTeamEliminated);
	}
}

void UTTTPlayerHUDSourceComponent::UnbindSources()
{
	if (PhysicalStateSource)
	{
		PhysicalStateSource->OnPhysicalStateChanged.RemoveDynamic(this, &UTTTPlayerHUDSourceComponent::HandlePPPhysicalStateChanged);
		PhysicalStateSource->OnDazeChanged.RemoveDynamic(this, &UTTTPlayerHUDSourceComponent::HandlePPDazeChanged);
		PhysicalStateSource->OnCarriedRecoveryChanged.RemoveDynamic(this, &UTTTPlayerHUDSourceComponent::HandlePPCarriedRecoveryChanged);
	}
	if (GrabberSource) GrabberSource->OnGrabStateChanged.RemoveDynamic(this, &UTTTPlayerHUDSourceComponent::HandlePPGrabStateChanged);
	if (CarrySource) CarrySource->OnAssistTargetChanged.RemoveDynamic(this, &UTTTPlayerHUDSourceComponent::HandlePPAssistTargetChanged);
	if (TeamSource) TeamSource->OnPartnerChanged.RemoveDynamic(this, &UTTTPlayerHUDSourceComponent::HandlePPTeamChanged);
	if (RaceStateSource) RaceStateSource->OnRacePhaseChanged.RemoveDynamic(this, &UTTTPlayerHUDSourceComponent::HandleRacePhaseChanged);
	if (RankingSource) RankingSource->OnRankingChanged.RemoveDynamic(this, &UTTTPlayerHUDSourceComponent::HandleRankingChanged);
	if (FinishSource)
	{
		FinishSource->OnTeamFinished.RemoveDynamic(this, &UTTTPlayerHUDSourceComponent::HandleTeamFinished);
		FinishSource->OnTeamEliminated.RemoveDynamic(this, &UTTTPlayerHUDSourceComponent::HandleTeamEliminated);
	}
}

void UTTTPlayerHUDSourceComponent::UpdatePresentationClock()
{
	if (!RaceStateSource || !RaceClockSource || !RankingSource || !FinishSource)
	{
		UnbindSources();
		ResolveSources();
		BindSources();
	}
	EnsureTeamRegistered();
	RaceClockUpdatedEvent.Broadcast(GetRaceElapsedSeconds_Implementation());
	if (RaceStateSource && RaceStateSource->GetRacePhase() == ETTTRacePhase::Countdown)
	{
		const int32 CountdownValue = RaceStateSource->GetCountdownValue();
		if (CountdownValue != LastCountdownValue)
		{
			LastCountdownValue = CountdownValue;
			CountdownValueChangedEvent.Broadcast(CountdownValue);
		}
	}
}

void UTTTPlayerHUDSourceComponent::EnsureTeamRegistered()
{
	if (!GetOwner() || !GetOwner()->HasAuthority() || !RankingSource) return;
	const int32 TeamId = ResolveTeamId();
	if (TeamId < 0) return;
	FTTTTeamRaceEntry ExistingEntry;
	if (!RankingSource->GetTeamRaceEntry(TeamId, ExistingEntry))
	{
		RankingSource->UpdateTeamProgress(TeamId, 0.0f, 0, 0.0f);
	}
}

ETTTPhysicalState UTTTPlayerHUDSourceComponent::MapPhysicalState(EPPPhysicalState SourceState) const
{
	switch (SourceState)
	{
	case EPPPhysicalState::Reactive:
	case EPPPhysicalState::Stumbling: return ETTTPhysicalState::Stumbling;
	case EPPPhysicalState::Falling:
	case EPPPhysicalState::Ragdolled: return ETTTPhysicalState::Ragdolled;
	case EPPPhysicalState::Unconscious: return ETTTPhysicalState::FullyDazed;
	case EPPPhysicalState::Piggybacked: return ETTTPhysicalState::BeingCarried;
	default: return ETTTPhysicalState::Upright;
	}
}

int32 UTTTPlayerHUDSourceComponent::ResolveTeamId() const
{
	return TeamSource ? TeamSource->GetTeamId_Implementation() : INDEX_NONE;
}
