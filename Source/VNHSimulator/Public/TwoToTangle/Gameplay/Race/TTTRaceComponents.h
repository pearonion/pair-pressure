#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "TwoToTangle/UI/Data/TTTMatchHUDTypes.h"
#include "TTTRaceComponents.generated.h"

class APlayerState;

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FTTTRacePhaseChanged, ETTTRacePhase, NewPhase);
DECLARE_DYNAMIC_MULTICAST_DELEGATE(FTTTRaceStarted);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FTTTRankingChanged, int32, TeamId, int32, NewPlacement);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FTTTTeamRaceResolved, int32, TeamId, float, OfficialTime);

USTRUCT(BlueprintType)
struct VNHSIMULATOR_API FTTTTeamRaceResolution
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Two to Tangle|Race") int32 TeamId = INDEX_NONE;
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Two to Tangle|Race") float OfficialTime = 0.0f;
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Two to Tangle|Race") bool bEliminated = false;
};

USTRUCT(BlueprintType)
struct VNHSIMULATOR_API FTTTTeamRaceEntry
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Two to Tangle|Race") int32 TeamId = INDEX_NONE;
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Two to Tangle|Race") float ProgressNormalized = 0.0f;
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Two to Tangle|Race") int32 CourseSectionIndex = 0;
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Two to Tangle|Race") float DistanceAlongSection = 0.0f;
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Two to Tangle|Race") bool bFinished = false;
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Two to Tangle|Race") float OfficialFinishTime = 0.0f;
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Two to Tangle|Race") int32 Placement = 1;
};

UCLASS(ClassGroup = (TwoToTangle), BlueprintType, Blueprintable, meta = (BlueprintSpawnableComponent))
class VNHSIMULATOR_API UTTTRaceClockComponent : public UActorComponent
{
	GENERATED_BODY()
public:
	UTTTRaceClockComponent();
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	UFUNCTION(BlueprintCallable, BlueprintAuthorityOnly, Category = "Two to Tangle|Race") void StartRaceClock();
	UFUNCTION(BlueprintCallable, BlueprintAuthorityOnly, Category = "Two to Tangle|Race") float StopRaceClock();
	UFUNCTION(BlueprintCallable, BlueprintAuthorityOnly, Category = "Two to Tangle|Race") void ResetRaceClock();
	UFUNCTION(BlueprintPure, Category = "Two to Tangle|Race") float GetRaceElapsedSeconds() const;
	UFUNCTION(BlueprintPure, Category = "Two to Tangle|Race") bool IsRaceClockRunning() const { return bRaceClockRunning; }
	UFUNCTION(BlueprintPure, Category = "Two to Tangle|Race") double GetRaceStartServerTime() const { return RaceStartServerTime; }

private:
	double GetSynchronizedServerTime() const;

	UPROPERTY(Replicated) double RaceStartServerTime = 0.0;
	UPROPERTY(Replicated) double TeamFinishServerTime = 0.0;
	UPROPERTY(Replicated) bool bRaceClockRunning = false;
};

UCLASS(ClassGroup = (TwoToTangle), BlueprintType, Blueprintable, meta = (BlueprintSpawnableComponent))
class VNHSIMULATOR_API UTTTRaceStateComponent : public UActorComponent
{
	GENERATED_BODY()
public:
	UTTTRaceStateComponent();
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	UFUNCTION(BlueprintCallable, BlueprintAuthorityOnly, Category = "Two to Tangle|Race") void StartCountdown(float RequestedDuration = 3.0f);
	UFUNCTION(BlueprintCallable, BlueprintAuthorityOnly, Category = "Two to Tangle|Race") void SetTeamFinished();
	UFUNCTION(BlueprintCallable, BlueprintAuthorityOnly, Category = "Two to Tangle|Race") void SetTeamEliminated();
	UFUNCTION(BlueprintCallable, BlueprintAuthorityOnly, Category = "Two to Tangle|Race") void SetResultsTransition();
	UFUNCTION(BlueprintCallable, BlueprintAuthorityOnly, Category = "Two to Tangle|Race") void ResetRaceState();
	UFUNCTION(BlueprintPure, Category = "Two to Tangle|Race") ETTTRacePhase GetRacePhase() const { return RacePhase; }
	UFUNCTION(BlueprintPure, Category = "Two to Tangle|Race") int32 GetCountdownValue() const;
	UFUNCTION(BlueprintPure, Category = "Two to Tangle|Race") double GetCountdownStartServerTime() const { return CountdownStartServerTime; }

	UPROPERTY(BlueprintAssignable, Category = "Two to Tangle|Race") FTTTRacePhaseChanged OnRacePhaseChanged;
	UPROPERTY(BlueprintAssignable, Category = "Two to Tangle|Race") FTTTRaceStarted OnRaceStarted;

private:
	UFUNCTION() void OnRep_RacePhase();
	void EvaluateCountdown();
	void SetRacePhaseAuthoritative(ETTTRacePhase NewPhase);
	double GetSynchronizedServerTime() const;

	UPROPERTY(ReplicatedUsing = OnRep_RacePhase) ETTTRacePhase RacePhase = ETTTRacePhase::Waiting;
	UPROPERTY(Replicated) double CountdownStartServerTime = 0.0;
	UPROPERTY(Replicated) float CountdownDuration = 3.0f;
	FTimerHandle CountdownTimerHandle;
};

UCLASS(ClassGroup = (TwoToTangle), BlueprintType, Blueprintable, meta = (BlueprintSpawnableComponent))
class VNHSIMULATOR_API UTTTRaceRankingComponent : public UActorComponent
{
	GENERATED_BODY()
public:
	UTTTRaceRankingComponent();
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	UFUNCTION(BlueprintCallable, BlueprintAuthorityOnly, Category = "Two to Tangle|Race") void UpdateTeamProgress(int32 TeamId, float ProgressNormalized, int32 CourseSectionIndex, float DistanceAlongSection);
	UFUNCTION(BlueprintCallable, BlueprintAuthorityOnly, Category = "Two to Tangle|Race") void MarkTeamFinished(int32 TeamId, float OfficialFinishTime);
	UFUNCTION(BlueprintCallable, BlueprintAuthorityOnly, Category = "Two to Tangle|Race") void ResetRankings();
	UFUNCTION(BlueprintPure, Category = "Two to Tangle|Race") int32 GetPlacementForTeam(int32 TeamId) const;
	UFUNCTION(BlueprintPure, Category = "Two to Tangle|Race") int32 GetTeamCount() const { return TeamEntries.Num(); }
	UFUNCTION(BlueprintPure, Category = "Two to Tangle|Race") bool GetTeamRaceEntry(int32 TeamId, FTTTTeamRaceEntry& OutEntry) const;

	UPROPERTY(BlueprintAssignable, Category = "Two to Tangle|Race") FTTTRankingChanged OnRankingChanged;

private:
	UFUNCTION() void OnRep_TeamEntries(const TArray<FTTTTeamRaceEntry>& PreviousEntries);
	FTTTTeamRaceEntry& FindOrAddTeam(int32 TeamId);
	void RecalculatePlacements();

	UPROPERTY(ReplicatedUsing = OnRep_TeamEntries) TArray<FTTTTeamRaceEntry> TeamEntries;
};

UCLASS(ClassGroup = (TwoToTangle), BlueprintType, Blueprintable, meta = (BlueprintSpawnableComponent))
class VNHSIMULATOR_API UTTTFinishTrackerComponent : public UActorComponent
{
	GENERATED_BODY()
public:
	UTTTFinishTrackerComponent();
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	UFUNCTION(BlueprintCallable, BlueprintAuthorityOnly, Category = "Two to Tangle|Race") void RegisterParticipantFinished(int32 TeamId, APlayerState* Participant);
	UFUNCTION(BlueprintCallable, BlueprintAuthorityOnly, Category = "Two to Tangle|Race") void RegisterTeamFinished(int32 TeamId);
	UFUNCTION(BlueprintCallable, BlueprintAuthorityOnly, Category = "Two to Tangle|Race") void EliminateTeam(int32 TeamId);
	UFUNCTION(BlueprintCallable, BlueprintAuthorityOnly, Category = "Two to Tangle|Race") void ResetFinishTracking();
	UFUNCTION(BlueprintPure, Category = "Two to Tangle|Race") bool HasTeamFinished(int32 TeamId) const;
	UFUNCTION(BlueprintPure, Category = "Two to Tangle|Race") bool GetTeamResolution(int32 TeamId, FTTTTeamRaceResolution& OutResolution) const;

	UPROPERTY(BlueprintAssignable, Category = "Two to Tangle|Race") FTTTTeamRaceResolved OnTeamFinished;
	UPROPERTY(BlueprintAssignable, Category = "Two to Tangle|Race") FTTTTeamRaceResolved OnTeamEliminated;
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Two to Tangle|Race", meta = (ClampMin = "1")) int32 RequiredPlayersPerTeam = 2;

private:
	UFUNCTION() void OnRep_TeamResolutions(const TArray<FTTTTeamRaceResolution>& PreviousResolutions);
	void ResolveTeam(int32 TeamId, bool bEliminated);

	UPROPERTY(ReplicatedUsing = OnRep_TeamResolutions) TArray<FTTTTeamRaceResolution> TeamResolutions;
	TMap<int32, TSet<TWeakObjectPtr<APlayerState>>> FinishedParticipantsByTeam;
	TSet<int32> ResolvedTeamIds;
};
