#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "PairPressure/PPGameplayTypes.h"
#include "TwoToTangle/UI/Interfaces/TTTUIInterfaces.h"
#include "TTTPlayerHUDSourceComponent.generated.h"

class UPPCarryComponent;
class UPPGrabberComponent;
class UPPPhysicalStateComponent;
class UPPTeamMemberComponent;
class UTTTFinishTrackerComponent;
class UTTTMatchHUDConfig;
class UTTTRaceClockComponent;
class UTTTRaceRankingComponent;
class UTTTRaceStateComponent;

UCLASS(ClassGroup = (TwoToTangle), BlueprintType, Blueprintable, meta = (BlueprintSpawnableComponent))
class VNHSIMULATOR_API UTTTPlayerHUDSourceComponent : public UActorComponent,
	public ITTTUIDataProviderInterface,
	public ITTTUINotificationSourceInterface
{
	GENERATED_BODY()

public:
	UTTTPlayerHUDSourceComponent();
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

	virtual FTTTMatchHUDSnapshot GetMatchHUDSnapshot_Implementation() const override;
	virtual float GetRaceElapsedSeconds_Implementation() const override;
	virtual int32 GetCurrentPlacement_Implementation() const override;
	virtual int32 GetTeamCount_Implementation() const override;
	virtual float GetDazeNormalized_Implementation() const override;
	virtual float GetDazeRecoveryThresholdNormalized_Implementation() const override;
	virtual FTTTHeldItemPresentation GetHeldItemPresentation_Implementation() const override;
	virtual ETTTPhysicalState GetPhysicalState_Implementation() const override;
	virtual bool CanLocalPlayerMove_Implementation() const override;
	virtual bool CanLocalPlayerDismount_Implementation() const override;

	virtual FTTTRacePhaseChangedNative& OnRacePhaseChanged() override { return RacePhaseChangedEvent; }
	virtual FTTTCountdownValueChangedNative& OnCountdownValueChanged() override { return CountdownValueChangedEvent; }
	virtual FTTTRaceClockUpdatedNative& OnRaceClockUpdated() override { return RaceClockUpdatedEvent; }
	virtual FTTTPlacementChangedNative& OnPlacementChanged() override { return PlacementChangedEvent; }
	virtual FTTTHeldItemChangedNative& OnHeldItemChanged() override { return HeldItemChangedEvent; }
	virtual FTTTThrowChargeStartedNative& OnThrowChargeStarted() override { return ThrowChargeStartedEvent; }
	virtual FTTTThrowChargeChangedNative& OnThrowChargeChanged() override { return ThrowChargeChangedEvent; }
	virtual FTTTThrowChargeEndedNative& OnThrowChargeEnded() override { return ThrowChargeEndedEvent; }
	virtual FTTTDazeChangedNative& OnDazeChanged() override { return DazeChangedEvent; }
	virtual FTTTPhysicalStateChangedNative& OnPhysicalStateChanged() override { return PhysicalStateChangedEvent; }
	virtual FTTTCarryStateChangedNative& OnCarryStateChanged() override { return CarryStateChangedEvent; }
	virtual FTTTMovementRestoredNative& OnMovementRestored() override { return MovementRestoredEvent; }
	virtual FTTTTeamFinishedNative& OnTeamFinished() override { return TeamFinishedEvent; }
	virtual FTTTTeamEliminatedNative& OnTeamEliminated() override { return TeamEliminatedEvent; }
	virtual FTTTResultsTransitionRequestedNative& OnResultsTransitionRequested() override { return ResultsTransitionRequestedEvent; }

	UFUNCTION(BlueprintCallable, Category = "Two to Tangle|HUD") void SetThrowChargeState(bool bCharging, float NormalizedCharge);
	UFUNCTION(BlueprintCallable, Category = "Two to Tangle|HUD") void RefreshAllPresentation();
	UFUNCTION(BlueprintCallable, Category = "Two to Tangle|HUD|Debug") void DebugSetPlacement(int32 Placement, int32 TeamCount);
	UFUNCTION(BlueprintCallable, Category = "Two to Tangle|HUD|Debug") void DebugSetRaceClock(float ElapsedSeconds);
	UFUNCTION(BlueprintCallable, Category = "Two to Tangle|HUD|Debug") void DebugPresentFinish(bool bFirstPlace, bool bEliminated);
	UFUNCTION(BlueprintCallable, Category = "Two to Tangle|HUD|Debug") void DebugPresentHeldItem(const FText& DisplayName);

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Two to Tangle|HUD") TObjectPtr<UTTTMatchHUDConfig> HUDConfig;

private:
	UFUNCTION() void HandlePPPhysicalStateChanged(EPPPhysicalState NewState, float DazeNormalized);
	UFUNCTION() void HandlePPDazeChanged(float DazeNormalized);
	UFUNCTION() void HandlePPCarriedRecoveryChanged(bool bIsBeingCarried, bool bCanDismount);
	UFUNCTION() void HandlePPGrabStateChanged(EPPGrabState NewGrabState, AActor* NewTarget);
	UFUNCTION() void HandlePPAssistTargetChanged(AActor* NewAssistTarget);
	UFUNCTION() void HandlePPTeamChanged(int32 ChangedTeamId, AActor* NewPartner);
	UFUNCTION() void HandleRacePhaseChanged(ETTTRacePhase NewPhase);
	UFUNCTION() void HandleRankingChanged(int32 ChangedTeamId, int32 NewPlacement);
	UFUNCTION() void HandleTeamFinished(int32 FinishedTeamId, float OfficialTime);
	UFUNCTION() void HandleTeamEliminated(int32 EliminatedTeamId, float ResolutionTime);
	void ResolveSources();
	void BindSources();
	void UnbindSources();
	void UpdatePresentationClock();
	void EnsureTeamRegistered();
	ETTTPhysicalState MapPhysicalState(EPPPhysicalState SourceState) const;
	int32 ResolveTeamId() const;

	UPROPERTY(Transient) TObjectPtr<UPPPhysicalStateComponent> PhysicalStateSource;
	UPROPERTY(Transient) TObjectPtr<UPPTeamMemberComponent> TeamSource;
	UPROPERTY(Transient) TObjectPtr<UPPGrabberComponent> GrabberSource;
	UPROPERTY(Transient) TObjectPtr<UPPCarryComponent> CarrySource;
	UPROPERTY(Transient) TObjectPtr<UTTTRaceStateComponent> RaceStateSource;
	UPROPERTY(Transient) TObjectPtr<UTTTRaceClockComponent> RaceClockSource;
	UPROPERTY(Transient) TObjectPtr<UTTTRaceRankingComponent> RankingSource;
	UPROPERTY(Transient) TObjectPtr<UTTTFinishTrackerComponent> FinishSource;

	FTTTRacePhaseChangedNative RacePhaseChangedEvent;
	FTTTCountdownValueChangedNative CountdownValueChangedEvent;
	FTTTRaceClockUpdatedNative RaceClockUpdatedEvent;
	FTTTPlacementChangedNative PlacementChangedEvent;
	FTTTHeldItemChangedNative HeldItemChangedEvent;
	FTTTThrowChargeStartedNative ThrowChargeStartedEvent;
	FTTTThrowChargeChangedNative ThrowChargeChangedEvent;
	FTTTThrowChargeEndedNative ThrowChargeEndedEvent;
	FTTTDazeChangedNative DazeChangedEvent;
	FTTTPhysicalStateChangedNative PhysicalStateChangedEvent;
	FTTTCarryStateChangedNative CarryStateChangedEvent;
	FTTTMovementRestoredNative MovementRestoredEvent;
	FTTTTeamFinishedNative TeamFinishedEvent;
	FTTTTeamEliminatedNative TeamEliminatedEvent;
	FTTTResultsTransitionRequestedNative ResultsTransitionRequestedEvent;

	FTimerHandle PresentationClockTimerHandle;
	TWeakObjectPtr<AActor> PresentedHeldActor;
	bool bThrowChargeActive = false;
	float ThrowChargeNormalized = 0.0f;
	int32 LastCountdownValue = INDEX_NONE;
	float DebugClockOverride = -1.0f;
	int32 DebugPlacementOverride = INDEX_NONE;
	int32 DebugTeamCountOverride = INDEX_NONE;
	FTTTHeldItemPresentation DebugHeldItemOverride;
};
