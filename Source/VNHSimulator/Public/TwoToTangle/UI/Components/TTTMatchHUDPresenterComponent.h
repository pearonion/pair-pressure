#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "TwoToTangle/UI/Data/TTTMatchHUDTypes.h"
#include "TTTMatchHUDPresenterComponent.generated.h"

class ITTTUINotificationSourceInterface;
class UTTTMatchHUDConfig;
class UTTTMatchHUDWidget;
class UTTTMatchTransitionWidget;
class UTTTPlayerHUDSourceComponent;

UCLASS(ClassGroup = (TwoToTangle), BlueprintType, Blueprintable, meta = (BlueprintSpawnableComponent))
class VNHSIMULATOR_API UTTTMatchHUDPresenterComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UTTTMatchHUDPresenterComponent();
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

	UFUNCTION(BlueprintCallable, Category = "Two to Tangle|HUD") void TryInitializeHUD();
	UFUNCTION(BlueprintCallable, Category = "Two to Tangle|HUD") void TeardownHUD();
	UFUNCTION(BlueprintCallable, Category = "Two to Tangle|HUD|Debug") void ToggleHUD();
	UFUNCTION(BlueprintPure, Category = "Two to Tangle|HUD") UTTTPlayerHUDSourceComponent* GetPlayerHUDSource() const { return PlayerHUDSource; }
	UFUNCTION(BlueprintPure, Category = "Two to Tangle|HUD") bool IsMatchHUDEnabled() const { return bEnableMatchHUD; }
	UFUNCTION(BlueprintPure, Category = "Two to Tangle|HUD") bool HasActiveMatchHUD() const { return MatchHUDWidget != nullptr; }

	// Keep the staged native framework dormant until its config and Designer
	// assets are committed and the legacy HUD consumers are fully bridged.
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Two to Tangle|HUD")
	bool bEnableMatchHUD = false;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Two to Tangle|HUD") TSoftObjectPtr<UTTTMatchHUDConfig> HUDConfigAsset;

private:
	void SubscribeToSource();
	void UnsubscribeFromSource();
	void PushInitialSnapshot();
	void HandleRacePhaseChanged(ETTTRacePhase NewPhase);
	void HandleCountdownValueChanged(int32 CountdownValue);
	void HandleRaceClockUpdated(float ElapsedSeconds);
	void HandlePlacementChanged(int32 Placement, int32 TeamCount);
	void HandleHeldItemChanged(const FTTTHeldItemPresentation& ItemData);
	void HandleThrowChargeStarted();
	void HandleThrowChargeChanged(float NormalizedCharge);
	void HandleThrowChargeEnded();
	void HandleDazeChanged(float NormalizedDaze, float RecoveryThreshold);
	void HandlePhysicalStateChanged(ETTTPhysicalState NewState);
	void HandleCarryStateChanged(bool bIsCarried, bool bCanDismount);
	void HandleMovementRestored();
	void HandleTeamFinished(const FTTTFinishPresentation& ResultData);
	void HandleTeamEliminated(const FTTTFinishPresentation& ResultData);
	void HandleResultsTransitionRequested();
	void BeginResultTimer(const FTTTFinishPresentation& ResultData);
	void BeginHUDFadeOut();
	void CompleteResultsTransition();

	UPROPERTY(Transient) TObjectPtr<UTTTMatchHUDConfig> ResolvedConfig;
	UPROPERTY(Transient) TObjectPtr<UTTTPlayerHUDSourceComponent> PlayerHUDSource;
	UPROPERTY(Transient) TObjectPtr<UTTTMatchHUDWidget> MatchHUDWidget;
	UPROPERTY(Transient) TObjectPtr<UTTTMatchTransitionWidget> MatchTransitionWidget;
	FTTTFinishPresentation PendingResult;
	FTimerHandle InitializationRetryTimerHandle;
	FTimerHandle ResultFadeTimerHandle;
	FTimerHandle ResultTransitionTimerHandle;
	bool bResultTransitionPending = false;
};
