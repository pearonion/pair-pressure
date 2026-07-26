#include "TwoToTangle/Gameplay/Race/TTTRaceComponents.h"

#include "Engine/World.h"
#include "GameFramework/GameStateBase.h"
#include "GameFramework/PlayerState.h"
#include "Net/UnrealNetwork.h"
#include "TimerManager.h"

namespace
{
double TTTSynchronizedServerTime(const UWorld* World)
{
	if (!World) return 0.0;
	if (const AGameStateBase* GameState = World->GetGameState()) return GameState->GetServerWorldTimeSeconds();
	return World->GetTimeSeconds();
}
}

UTTTRaceClockComponent::UTTTRaceClockComponent()
{
	SetIsReplicatedByDefault(true);
	PrimaryComponentTick.bCanEverTick = false;
}

void UTTTRaceClockComponent::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(UTTTRaceClockComponent, RaceStartServerTime);
	DOREPLIFETIME(UTTTRaceClockComponent, TeamFinishServerTime);
	DOREPLIFETIME(UTTTRaceClockComponent, bRaceClockRunning);
}

void UTTTRaceClockComponent::StartRaceClock()
{
	if (!GetOwner() || !GetOwner()->HasAuthority()) return;
	RaceStartServerTime = GetSynchronizedServerTime();
	TeamFinishServerTime = 0.0;
	bRaceClockRunning = true;
}

float UTTTRaceClockComponent::StopRaceClock()
{
	if (!GetOwner() || !GetOwner()->HasAuthority()) return GetRaceElapsedSeconds();
	if (bRaceClockRunning) TeamFinishServerTime = GetSynchronizedServerTime();
	bRaceClockRunning = false;
	return GetRaceElapsedSeconds();
}

void UTTTRaceClockComponent::ResetRaceClock()
{
	if (!GetOwner() || !GetOwner()->HasAuthority()) return;
	RaceStartServerTime = 0.0;
	TeamFinishServerTime = 0.0;
	bRaceClockRunning = false;
}

float UTTTRaceClockComponent::GetRaceElapsedSeconds() const
{
	if (RaceStartServerTime <= 0.0) return 0.0f;
	const double EndTime = bRaceClockRunning ? GetSynchronizedServerTime() : TeamFinishServerTime;
	return FMath::Max(0.0f, static_cast<float>(EndTime - RaceStartServerTime));
}

double UTTTRaceClockComponent::GetSynchronizedServerTime() const { return TTTSynchronizedServerTime(GetWorld()); }

UTTTRaceStateComponent::UTTTRaceStateComponent()
{
	SetIsReplicatedByDefault(true);
	PrimaryComponentTick.bCanEverTick = false;
}

void UTTTRaceStateComponent::BeginPlay()
{
	Super::BeginPlay();
	if (GetOwner() && GetOwner()->HasAuthority() && RacePhase == ETTTRacePhase::Countdown && GetWorld())
	{
		GetWorld()->GetTimerManager().SetTimer(CountdownTimerHandle, this, &UTTTRaceStateComponent::EvaluateCountdown, 0.05f, true);
	}
}

void UTTTRaceStateComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (GetWorld()) GetWorld()->GetTimerManager().ClearTimer(CountdownTimerHandle);
	Super::EndPlay(EndPlayReason);
}

void UTTTRaceStateComponent::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(UTTTRaceStateComponent, RacePhase);
	DOREPLIFETIME(UTTTRaceStateComponent, CountdownStartServerTime);
	DOREPLIFETIME(UTTTRaceStateComponent, CountdownDuration);
}

void UTTTRaceStateComponent::StartCountdown(float RequestedDuration)
{
	if (!GetOwner() || !GetOwner()->HasAuthority() || !GetWorld()) return;
	CountdownDuration = FMath::Max(1.0f, RequestedDuration);
	CountdownStartServerTime = GetSynchronizedServerTime();
	if (UTTTRaceClockComponent* RaceClock = GetOwner()->FindComponentByClass<UTTTRaceClockComponent>()) RaceClock->ResetRaceClock();
	if (UTTTRaceRankingComponent* Ranking = GetOwner()->FindComponentByClass<UTTTRaceRankingComponent>()) Ranking->ResetRankings();
	if (UTTTFinishTrackerComponent* FinishTracker = GetOwner()->FindComponentByClass<UTTTFinishTrackerComponent>()) FinishTracker->ResetFinishTracking();
	SetRacePhaseAuthoritative(ETTTRacePhase::Countdown);
	GetWorld()->GetTimerManager().SetTimer(CountdownTimerHandle, this, &UTTTRaceStateComponent::EvaluateCountdown, 0.05f, true);
}

void UTTTRaceStateComponent::SetTeamFinished() { SetRacePhaseAuthoritative(ETTTRacePhase::TeamFinished); }
void UTTTRaceStateComponent::SetTeamEliminated() { SetRacePhaseAuthoritative(ETTTRacePhase::Eliminated); }
void UTTTRaceStateComponent::SetResultsTransition() { SetRacePhaseAuthoritative(ETTTRacePhase::ResultsTransition); }

void UTTTRaceStateComponent::ResetRaceState()
{
	if (!GetOwner() || !GetOwner()->HasAuthority()) return;
	if (GetWorld()) GetWorld()->GetTimerManager().ClearTimer(CountdownTimerHandle);
	CountdownStartServerTime = 0.0;
	SetRacePhaseAuthoritative(ETTTRacePhase::Waiting);
}

int32 UTTTRaceStateComponent::GetCountdownValue() const
{
	if (RacePhase != ETTTRacePhase::Countdown) return RacePhase == ETTTRacePhase::Racing ? 0 : -1;
	const float Elapsed = static_cast<float>(GetSynchronizedServerTime() - CountdownStartServerTime);
	return FMath::Clamp(FMath::CeilToInt(CountdownDuration - Elapsed), 1, FMath::CeilToInt(CountdownDuration));
}

void UTTTRaceStateComponent::OnRep_RacePhase()
{
	OnRacePhaseChanged.Broadcast(RacePhase);
	if (RacePhase == ETTTRacePhase::Racing) OnRaceStarted.Broadcast();
}

void UTTTRaceStateComponent::EvaluateCountdown()
{
	if (!GetOwner() || !GetOwner()->HasAuthority() || RacePhase != ETTTRacePhase::Countdown) return;
	if (GetSynchronizedServerTime() - CountdownStartServerTime < CountdownDuration) return;
	if (GetWorld()) GetWorld()->GetTimerManager().ClearTimer(CountdownTimerHandle);
	if (UTTTRaceClockComponent* RaceClock = GetOwner()->FindComponentByClass<UTTTRaceClockComponent>()) RaceClock->StartRaceClock();
	SetRacePhaseAuthoritative(ETTTRacePhase::Racing);
}

void UTTTRaceStateComponent::SetRacePhaseAuthoritative(ETTTRacePhase NewPhase)
{
	if (!GetOwner() || !GetOwner()->HasAuthority() || RacePhase == NewPhase) return;
	RacePhase = NewPhase;
	OnRep_RacePhase();
}

double UTTTRaceStateComponent::GetSynchronizedServerTime() const { return TTTSynchronizedServerTime(GetWorld()); }

UTTTRaceRankingComponent::UTTTRaceRankingComponent()
{
	SetIsReplicatedByDefault(true);
	PrimaryComponentTick.bCanEverTick = false;
}

void UTTTRaceRankingComponent::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(UTTTRaceRankingComponent, TeamEntries);
}

void UTTTRaceRankingComponent::UpdateTeamProgress(int32 TeamId, float ProgressNormalized, int32 CourseSectionIndex, float DistanceAlongSection)
{
	if (!GetOwner() || !GetOwner()->HasAuthority() || TeamId < 0) return;
	FTTTTeamRaceEntry& Entry = FindOrAddTeam(TeamId);
	if (Entry.bFinished) return;
	Entry.ProgressNormalized = FMath::Clamp(ProgressNormalized, 0.0f, 1.0f);
	Entry.CourseSectionIndex = FMath::Max(0, CourseSectionIndex);
	Entry.DistanceAlongSection = FMath::Max(0.0f, DistanceAlongSection);
	RecalculatePlacements();
}

void UTTTRaceRankingComponent::MarkTeamFinished(int32 TeamId, float OfficialFinishTime)
{
	if (!GetOwner() || !GetOwner()->HasAuthority() || TeamId < 0) return;
	FTTTTeamRaceEntry& Entry = FindOrAddTeam(TeamId);
	Entry.bFinished = true;
	Entry.ProgressNormalized = 1.0f;
	Entry.OfficialFinishTime = FMath::Max(0.0f, OfficialFinishTime);
	RecalculatePlacements();
}

void UTTTRaceRankingComponent::ResetRankings()
{
	if (!GetOwner() || !GetOwner()->HasAuthority()) return;
	TeamEntries.Reset();
}

int32 UTTTRaceRankingComponent::GetPlacementForTeam(int32 TeamId) const
{
	if (const FTTTTeamRaceEntry* Entry = TeamEntries.FindByPredicate([TeamId](const FTTTTeamRaceEntry& Candidate) { return Candidate.TeamId == TeamId; })) return Entry->Placement;
	return TeamEntries.Num() + 1;
}

bool UTTTRaceRankingComponent::GetTeamRaceEntry(int32 TeamId, FTTTTeamRaceEntry& OutEntry) const
{
	if (const FTTTTeamRaceEntry* Entry = TeamEntries.FindByPredicate([TeamId](const FTTTTeamRaceEntry& Candidate) { return Candidate.TeamId == TeamId; }))
	{
		OutEntry = *Entry;
		return true;
	}
	return false;
}

void UTTTRaceRankingComponent::OnRep_TeamEntries(const TArray<FTTTTeamRaceEntry>& PreviousEntries)
{
	for (const FTTTTeamRaceEntry& Entry : TeamEntries)
	{
		const FTTTTeamRaceEntry* Previous = PreviousEntries.FindByPredicate([&Entry](const FTTTTeamRaceEntry& Candidate) { return Candidate.TeamId == Entry.TeamId; });
		if (!Previous || Previous->Placement != Entry.Placement) OnRankingChanged.Broadcast(Entry.TeamId, Entry.Placement);
	}
}

FTTTTeamRaceEntry& UTTTRaceRankingComponent::FindOrAddTeam(int32 TeamId)
{
	if (FTTTTeamRaceEntry* Existing = TeamEntries.FindByPredicate([TeamId](const FTTTTeamRaceEntry& Candidate) { return Candidate.TeamId == TeamId; })) return *Existing;
	FTTTTeamRaceEntry& Added = TeamEntries.AddDefaulted_GetRef();
	Added.TeamId = TeamId;
	return Added;
}

void UTTTRaceRankingComponent::RecalculatePlacements()
{
	TArray<FTTTTeamRaceEntry> PreviousEntries = TeamEntries;
	TeamEntries.Sort([](const FTTTTeamRaceEntry& Left, const FTTTTeamRaceEntry& Right)
	{
		if (Left.bFinished != Right.bFinished) return Left.bFinished;
		if (Left.bFinished && Right.bFinished) return Left.OfficialFinishTime < Right.OfficialFinishTime;
		if (!FMath::IsNearlyEqual(Left.ProgressNormalized, Right.ProgressNormalized)) return Left.ProgressNormalized > Right.ProgressNormalized;
		if (Left.CourseSectionIndex != Right.CourseSectionIndex) return Left.CourseSectionIndex > Right.CourseSectionIndex;
		if (!FMath::IsNearlyEqual(Left.DistanceAlongSection, Right.DistanceAlongSection)) return Left.DistanceAlongSection > Right.DistanceAlongSection;
		return Left.TeamId < Right.TeamId;
	});
	for (int32 Index = 0; Index < TeamEntries.Num(); ++Index) TeamEntries[Index].Placement = Index + 1;
	OnRep_TeamEntries(PreviousEntries);
}

UTTTFinishTrackerComponent::UTTTFinishTrackerComponent()
{
	SetIsReplicatedByDefault(true);
	PrimaryComponentTick.bCanEverTick = false;
}

void UTTTFinishTrackerComponent::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(UTTTFinishTrackerComponent, TeamResolutions);
}

void UTTTFinishTrackerComponent::RegisterParticipantFinished(int32 TeamId, APlayerState* Participant)
{
	if (!GetOwner() || !GetOwner()->HasAuthority() || TeamId < 0 || !Participant || ResolvedTeamIds.Contains(TeamId)) return;
	TSet<TWeakObjectPtr<APlayerState>>& TeamParticipants = FinishedParticipantsByTeam.FindOrAdd(TeamId);
	TeamParticipants.Add(Participant);
	if (TeamParticipants.Num() < RequiredPlayersPerTeam) return;
	ResolveTeam(TeamId, false);
}

void UTTTFinishTrackerComponent::RegisterTeamFinished(int32 TeamId)
{
	ResolveTeam(TeamId, false);
}

void UTTTFinishTrackerComponent::EliminateTeam(int32 TeamId)
{
	ResolveTeam(TeamId, true);
}

void UTTTFinishTrackerComponent::ResetFinishTracking()
{
	if (!GetOwner() || !GetOwner()->HasAuthority()) return;
	FinishedParticipantsByTeam.Reset();
	ResolvedTeamIds.Reset();
	TeamResolutions.Reset();
}

bool UTTTFinishTrackerComponent::HasTeamFinished(int32 TeamId) const { return ResolvedTeamIds.Contains(TeamId); }

bool UTTTFinishTrackerComponent::GetTeamResolution(int32 TeamId, FTTTTeamRaceResolution& OutResolution) const
{
	if (const FTTTTeamRaceResolution* Resolution = TeamResolutions.FindByPredicate([TeamId](const FTTTTeamRaceResolution& Candidate)
	{
		return Candidate.TeamId == TeamId;
	}))
	{
		OutResolution = *Resolution;
		return true;
	}
	return false;
}

void UTTTFinishTrackerComponent::OnRep_TeamResolutions(const TArray<FTTTTeamRaceResolution>& PreviousResolutions)
{
	for (const FTTTTeamRaceResolution& Resolution : TeamResolutions)
	{
		ResolvedTeamIds.Add(Resolution.TeamId);
		const bool bWasAlreadyResolved = PreviousResolutions.ContainsByPredicate([&Resolution](const FTTTTeamRaceResolution& Previous)
		{
			return Previous.TeamId == Resolution.TeamId;
		});
		if (bWasAlreadyResolved) continue;
		if (Resolution.bEliminated) OnTeamEliminated.Broadcast(Resolution.TeamId, Resolution.OfficialTime);
		else OnTeamFinished.Broadcast(Resolution.TeamId, Resolution.OfficialTime);
	}
}

void UTTTFinishTrackerComponent::ResolveTeam(int32 TeamId, bool bEliminated)
{
	if (!GetOwner() || !GetOwner()->HasAuthority() || TeamId < 0 || ResolvedTeamIds.Contains(TeamId)) return;

	ResolvedTeamIds.Add(TeamId);
	float OfficialTime = 0.0f;
	if (const UTTTRaceClockComponent* RaceClock = GetOwner()->FindComponentByClass<UTTTRaceClockComponent>())
	{
		OfficialTime = RaceClock->GetRaceElapsedSeconds();
	}
	if (!bEliminated)
	{
		if (UTTTRaceRankingComponent* Ranking = GetOwner()->FindComponentByClass<UTTTRaceRankingComponent>())
		{
			Ranking->MarkTeamFinished(TeamId, OfficialTime);
		}
	}

	TArray<FTTTTeamRaceResolution> PreviousResolutions = TeamResolutions;
	FTTTTeamRaceResolution& Resolution = TeamResolutions.AddDefaulted_GetRef();
	Resolution.TeamId = TeamId;
	Resolution.OfficialTime = OfficialTime;
	Resolution.bEliminated = bEliminated;
	OnRep_TeamResolutions(PreviousResolutions);
}
