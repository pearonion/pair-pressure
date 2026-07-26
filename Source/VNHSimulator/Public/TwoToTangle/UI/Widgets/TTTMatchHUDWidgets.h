#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "TwoToTangle/UI/Interfaces/TTTUIInterfaces.h"
#include "TTTMatchHUDWidgets.generated.h"

class UImage;
class UPanelWidget;
class UProgressBar;
class UTextBlock;
class UTTTMatchHUDConfig;

UCLASS(Abstract, BlueprintType, Blueprintable)
class VNHSIMULATOR_API UTTTRaceCountdownWidget : public UUserWidget
{
	GENERATED_BODY()
public:
	UFUNCTION(BlueprintCallable, Category = "Two to Tangle|HUD") void PresentCountdown(int32 CountdownValue);
	UFUNCTION(BlueprintCallable, Category = "Two to Tangle|HUD") void PresentGo();
protected:
	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional)) TObjectPtr<UTextBlock> TXT_Countdown;
	UFUNCTION(BlueprintImplementableEvent, Category = "Two to Tangle|HUD|Animation") void PlayCountdownIn();
	UFUNCTION(BlueprintImplementableEvent, Category = "Two to Tangle|HUD|Animation") void PlayCountdownOut();
};

UCLASS(Abstract, BlueprintType, Blueprintable)
class VNHSIMULATOR_API UTTTStopwatchWidget : public UUserWidget
{
	GENERATED_BODY()
public:
	UFUNCTION(BlueprintCallable, Category = "Two to Tangle|HUD") void SetElapsedSeconds(float ElapsedSeconds);
	UFUNCTION(BlueprintPure, Category = "Two to Tangle|HUD") static FText FormatRaceTime(float ElapsedSeconds);
protected:
	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional)) TObjectPtr<UTextBlock> TXT_Stopwatch;
};

UCLASS(Abstract, BlueprintType, Blueprintable)
class VNHSIMULATOR_API UTTTPlacementWidget : public UUserWidget
{
	GENERATED_BODY()
public:
	UFUNCTION(BlueprintCallable, Category = "Two to Tangle|HUD") void PresentPlacement(int32 Placement, int32 TeamCount);
	UFUNCTION(BlueprintPure, Category = "Two to Tangle|HUD") static FText FormatPlacement(int32 Placement, int32 TeamCount);
protected:
	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional)) TObjectPtr<UTextBlock> TXT_Placement;
	UFUNCTION(BlueprintImplementableEvent, Category = "Two to Tangle|HUD|Animation") void PlayPlacementChanged(bool bImproved);
private:
	int32 PreviousPlacement = 0;
};

UCLASS(Abstract, BlueprintType, Blueprintable)
class VNHSIMULATOR_API UTTTItemSlotWidget : public UUserWidget
{
	GENERATED_BODY()
public:
	UFUNCTION(BlueprintCallable, Category = "Two to Tangle|HUD") void PresentItem(const FTTTHeldItemPresentation& ItemData);
	UFUNCTION(BlueprintCallable, Category = "Two to Tangle|HUD") void ClearItem();
protected:
	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional)) TObjectPtr<UImage> IMG_ItemIcon;
	UFUNCTION(BlueprintImplementableEvent, Category = "Two to Tangle|HUD|Animation") void PlayItemIn();
	UFUNCTION(BlueprintImplementableEvent, Category = "Two to Tangle|HUD|Animation") void PlayItemOut();
};

UCLASS(Abstract, BlueprintType, Blueprintable)
class VNHSIMULATOR_API UTTTThrowStrengthWidget : public UUserWidget
{
	GENERATED_BODY()
public:
	UFUNCTION(BlueprintCallable, Category = "Two to Tangle|HUD") void PresentCharge(float NormalizedCharge);
	UFUNCTION(BlueprintCallable, Category = "Two to Tangle|HUD") void HideCharge();
protected:
	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional)) TObjectPtr<UProgressBar> PB_ThrowStrength;
};

UCLASS(Abstract, BlueprintType, Blueprintable)
class VNHSIMULATOR_API UTTTDazeMeterWidget : public UUserWidget
{
	GENERATED_BODY()
public:
	UFUNCTION(BlueprintCallable, Category = "Two to Tangle|HUD") void PresentDaze(float NormalizedDaze, float RecoveryThreshold, bool bRecoveryActive);
protected:
	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional)) TObjectPtr<UProgressBar> PB_Daze;
	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional)) TObjectPtr<UImage> IMG_DazePointer;
	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional)) TObjectPtr<UImage> IMG_RecoveryThreshold;
	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional)) TObjectPtr<UPanelWidget> Overlay_Meter;
	UFUNCTION(BlueprintImplementableEvent, Category = "Two to Tangle|HUD|Animation") void SetRecoveryPulseActive(bool bRecoveryActive);
};

UCLASS(Abstract, BlueprintType, Blueprintable)
class VNHSIMULATOR_API UTTTStatusMessageWidget : public UUserWidget
{
	GENERATED_BODY()
public:
	UFUNCTION(BlueprintCallable, Category = "Two to Tangle|HUD") void PresentMessage(const FText& PrimaryText, const FText& SecondaryText);
	UFUNCTION(BlueprintCallable, Category = "Two to Tangle|HUD") void HideMessage();
protected:
	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional)) TObjectPtr<UTextBlock> TXT_StatusPrimary;
	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional)) TObjectPtr<UTextBlock> TXT_StatusSecondary;
	UFUNCTION(BlueprintImplementableEvent, Category = "Two to Tangle|HUD|Animation") void PlayStatusIn();
	UFUNCTION(BlueprintImplementableEvent, Category = "Two to Tangle|HUD|Animation") void PlayStatusOut();
};

UCLASS(Abstract, BlueprintType, Blueprintable)
class VNHSIMULATOR_API UTTTFinishResultWidget : public UUserWidget
{
	GENERATED_BODY()
public:
	UFUNCTION(BlueprintCallable, Category = "Two to Tangle|HUD") void PresentFinished(const FTTTFinishPresentation& ResultData);
	UFUNCTION(BlueprintCallable, Category = "Two to Tangle|HUD") void PresentEliminated(const FTTTFinishPresentation& ResultData);
protected:
	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional)) TObjectPtr<UTextBlock> TXT_Result;
	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional)) TObjectPtr<UTextBlock> TXT_FinalTime;
	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional)) TObjectPtr<UTextBlock> TXT_ResultSecondary;
	UFUNCTION(BlueprintImplementableEvent, Category = "Two to Tangle|HUD|Animation") void PlayFinishIn();
	UFUNCTION(BlueprintImplementableEvent, Category = "Two to Tangle|HUD|Animation") void PlayEliminatedIn();
};

UCLASS(Abstract, BlueprintType, Blueprintable)
class VNHSIMULATOR_API UTTTMatchTransitionWidget : public UUserWidget
{
	GENERATED_BODY()
public:
	UFUNCTION(BlueprintCallable, Category = "Two to Tangle|HUD") void PresentMatchOver(const FTTTFinishPresentation& ResultData);
protected:
	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional)) TObjectPtr<UTextBlock> TXT_Result;
	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional)) TObjectPtr<UTextBlock> TXT_FinalTime;
};

UCLASS(Abstract, BlueprintType, Blueprintable)
class VNHSIMULATOR_API UTTTMatchHUDWidget : public UUserWidget, public ITTTMatchHUDInterface
{
	GENERATED_BODY()
public:
	UFUNCTION(BlueprintCallable, Category = "Two to Tangle|HUD") void ApplySnapshot(const FTTTMatchHUDSnapshot& Snapshot);
	void ConfigurePresentation(const UTTTMatchHUDConfig* HUDConfig);
	virtual void NativeDestruct() override;

	virtual void ShowCountdown_Implementation(int32 CountdownValue) override;
	virtual void ShowGo_Implementation() override;
	virtual void SetStopwatchTime_Implementation(float ElapsedSeconds) override;
	virtual void SetPlacement_Implementation(int32 Placement, int32 TeamCount) override;
	virtual void SetHeldItem_Implementation(const FTTTHeldItemPresentation& ItemData) override;
	virtual void ClearHeldItem_Implementation() override;
	virtual void ShowThrowStrength_Implementation(float NormalizedCharge) override;
	virtual void HideThrowStrength_Implementation() override;
	virtual void SetDaze_Implementation(float NormalizedDaze, float RecoveryThreshold) override;
	virtual void ShowFullyDazedMessage_Implementation() override;
	virtual void ShowBeingCarriedMessage_Implementation() override;
	virtual void ShowRecoveryAvailable_Implementation() override;
	virtual void ShowCourseCleared_Implementation(const FTTTFinishPresentation& Result) override;
	virtual void ShowEliminated_Implementation(const FTTTFinishPresentation& Result) override;
	virtual void HideTransientPresentation_Implementation() override;
	virtual void BeginResultsTransition_Implementation() override;

protected:
	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional)) TObjectPtr<UTTTRaceCountdownWidget> CountdownWidget;
	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional)) TObjectPtr<UTTTStopwatchWidget> StopwatchWidget;
	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional)) TObjectPtr<UTTTPlacementWidget> PlacementWidget;
	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional)) TObjectPtr<UTTTItemSlotWidget> ItemSlotWidget;
	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional)) TObjectPtr<UTTTThrowStrengthWidget> ThrowStrengthWidget;
	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional)) TObjectPtr<UTTTDazeMeterWidget> DazeMeterWidget;
	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional)) TObjectPtr<UTTTStatusMessageWidget> StatusMessageWidget;
	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional)) TObjectPtr<UTTTFinishResultWidget> FinishResultWidget;
	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional)) TObjectPtr<UPanelWidget> Overlay_TransientCountdown;
	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional)) TObjectPtr<UPanelWidget> Overlay_StatusMessages;
	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional)) TObjectPtr<UPanelWidget> Overlay_FinishPresentation;
	UFUNCTION(BlueprintImplementableEvent, Category = "Two to Tangle|HUD|Animation") void PlayHUDOut();

private:
	void CollapseDazeMeter();
	float DazeMeterShowThreshold = 0.05f;
	float DazeMeterHideDelay = 2.5f;
	FTimerHandle DazeMeterHideTimerHandle;
};
