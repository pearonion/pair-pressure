#pragma once

#include "CoreMinimal.h"
#include "UObject/Interface.h"
#include "TwoToTangle/UI/Data/TTTMatchHUDTypes.h"
#include "TTTUIInterfaces.generated.h"

DECLARE_MULTICAST_DELEGATE_OneParam(FTTTRacePhaseChangedNative, ETTTRacePhase);
DECLARE_MULTICAST_DELEGATE_OneParam(FTTTCountdownValueChangedNative, int32);
DECLARE_MULTICAST_DELEGATE_OneParam(FTTTRaceClockUpdatedNative, float);
DECLARE_MULTICAST_DELEGATE_TwoParams(FTTTPlacementChangedNative, int32, int32);
DECLARE_MULTICAST_DELEGATE_OneParam(FTTTHeldItemChangedNative, const FTTTHeldItemPresentation&);
DECLARE_MULTICAST_DELEGATE(FTTTThrowChargeStartedNative);
DECLARE_MULTICAST_DELEGATE_OneParam(FTTTThrowChargeChangedNative, float);
DECLARE_MULTICAST_DELEGATE(FTTTThrowChargeEndedNative);
DECLARE_MULTICAST_DELEGATE_TwoParams(FTTTDazeChangedNative, float, float);
DECLARE_MULTICAST_DELEGATE_OneParam(FTTTPhysicalStateChangedNative, ETTTPhysicalState);
DECLARE_MULTICAST_DELEGATE_TwoParams(FTTTCarryStateChangedNative, bool, bool);
DECLARE_MULTICAST_DELEGATE(FTTTMovementRestoredNative);
DECLARE_MULTICAST_DELEGATE_OneParam(FTTTTeamFinishedNative, const FTTTFinishPresentation&);
DECLARE_MULTICAST_DELEGATE_OneParam(FTTTTeamEliminatedNative, const FTTTFinishPresentation&);
DECLARE_MULTICAST_DELEGATE(FTTTResultsTransitionRequestedNative);

UINTERFACE(BlueprintType)
class VNHSIMULATOR_API UTTTUIDataProviderInterface : public UInterface
{
	GENERATED_BODY()
};
class VNHSIMULATOR_API ITTTUIDataProviderInterface
{
	GENERATED_BODY()

public:
	UFUNCTION(BlueprintNativeEvent, BlueprintCallable, Category = "Two to Tangle|UI") FTTTMatchHUDSnapshot GetMatchHUDSnapshot() const;
	UFUNCTION(BlueprintNativeEvent, BlueprintCallable, Category = "Two to Tangle|UI") float GetRaceElapsedSeconds() const;
	UFUNCTION(BlueprintNativeEvent, BlueprintCallable, Category = "Two to Tangle|UI") int32 GetCurrentPlacement() const;
	UFUNCTION(BlueprintNativeEvent, BlueprintCallable, Category = "Two to Tangle|UI") int32 GetTeamCount() const;
	UFUNCTION(BlueprintNativeEvent, BlueprintCallable, Category = "Two to Tangle|UI") float GetDazeNormalized() const;
	UFUNCTION(BlueprintNativeEvent, BlueprintCallable, Category = "Two to Tangle|UI") float GetDazeRecoveryThresholdNormalized() const;
	UFUNCTION(BlueprintNativeEvent, BlueprintCallable, Category = "Two to Tangle|UI") FTTTHeldItemPresentation GetHeldItemPresentation() const;
	UFUNCTION(BlueprintNativeEvent, BlueprintCallable, Category = "Two to Tangle|UI") ETTTPhysicalState GetPhysicalState() const;
	UFUNCTION(BlueprintNativeEvent, BlueprintCallable, Category = "Two to Tangle|UI") bool CanLocalPlayerMove() const;
	UFUNCTION(BlueprintNativeEvent, BlueprintCallable, Category = "Two to Tangle|UI") bool CanLocalPlayerDismount() const;
};

UINTERFACE(MinimalAPI)
class UTTTUINotificationSourceInterface : public UInterface
{
	GENERATED_BODY()
};

class VNHSIMULATOR_API ITTTUINotificationSourceInterface
{
	GENERATED_BODY()

public:
	virtual FTTTRacePhaseChangedNative& OnRacePhaseChanged() = 0;
	virtual FTTTCountdownValueChangedNative& OnCountdownValueChanged() = 0;
	virtual FTTTRaceClockUpdatedNative& OnRaceClockUpdated() = 0;
	virtual FTTTPlacementChangedNative& OnPlacementChanged() = 0;
	virtual FTTTHeldItemChangedNative& OnHeldItemChanged() = 0;
	virtual FTTTThrowChargeStartedNative& OnThrowChargeStarted() = 0;
	virtual FTTTThrowChargeChangedNative& OnThrowChargeChanged() = 0;
	virtual FTTTThrowChargeEndedNative& OnThrowChargeEnded() = 0;
	virtual FTTTDazeChangedNative& OnDazeChanged() = 0;
	virtual FTTTPhysicalStateChangedNative& OnPhysicalStateChanged() = 0;
	virtual FTTTCarryStateChangedNative& OnCarryStateChanged() = 0;
	virtual FTTTMovementRestoredNative& OnMovementRestored() = 0;
	virtual FTTTTeamFinishedNative& OnTeamFinished() = 0;
	virtual FTTTTeamEliminatedNative& OnTeamEliminated() = 0;
	virtual FTTTResultsTransitionRequestedNative& OnResultsTransitionRequested() = 0;
};

UINTERFACE(BlueprintType)
class VNHSIMULATOR_API UTTTMatchHUDInterface : public UInterface
{
	GENERATED_BODY()
};

class VNHSIMULATOR_API ITTTMatchHUDInterface
{
	GENERATED_BODY()

public:
	UFUNCTION(BlueprintNativeEvent, BlueprintCallable, Category = "Two to Tangle|HUD") void ShowCountdown(int32 CountdownValue);
	UFUNCTION(BlueprintNativeEvent, BlueprintCallable, Category = "Two to Tangle|HUD") void ShowGo();
	UFUNCTION(BlueprintNativeEvent, BlueprintCallable, Category = "Two to Tangle|HUD") void SetStopwatchTime(float ElapsedSeconds);
	UFUNCTION(BlueprintNativeEvent, BlueprintCallable, Category = "Two to Tangle|HUD") void SetPlacement(int32 Placement, int32 TeamCount);
	UFUNCTION(BlueprintNativeEvent, BlueprintCallable, Category = "Two to Tangle|HUD") void SetHeldItem(const FTTTHeldItemPresentation& ItemData);
	UFUNCTION(BlueprintNativeEvent, BlueprintCallable, Category = "Two to Tangle|HUD") void ClearHeldItem();
	UFUNCTION(BlueprintNativeEvent, BlueprintCallable, Category = "Two to Tangle|HUD") void ShowThrowStrength(float NormalizedCharge);
	UFUNCTION(BlueprintNativeEvent, BlueprintCallable, Category = "Two to Tangle|HUD") void HideThrowStrength();
	UFUNCTION(BlueprintNativeEvent, BlueprintCallable, Category = "Two to Tangle|HUD") void SetDaze(float NormalizedDaze, float RecoveryThreshold);
	UFUNCTION(BlueprintNativeEvent, BlueprintCallable, Category = "Two to Tangle|HUD") void ShowFullyDazedMessage();
	UFUNCTION(BlueprintNativeEvent, BlueprintCallable, Category = "Two to Tangle|HUD") void ShowBeingCarriedMessage();
	UFUNCTION(BlueprintNativeEvent, BlueprintCallable, Category = "Two to Tangle|HUD") void ShowRecoveryAvailable();
	UFUNCTION(BlueprintNativeEvent, BlueprintCallable, Category = "Two to Tangle|HUD") void ShowCourseCleared(const FTTTFinishPresentation& Result);
	UFUNCTION(BlueprintNativeEvent, BlueprintCallable, Category = "Two to Tangle|HUD") void ShowEliminated(const FTTTFinishPresentation& Result);
	UFUNCTION(BlueprintNativeEvent, BlueprintCallable, Category = "Two to Tangle|HUD") void HideTransientPresentation();
	UFUNCTION(BlueprintNativeEvent, BlueprintCallable, Category = "Two to Tangle|HUD") void BeginResultsTransition();
};
