#include "TwoToTangle/UI/Widgets/TTTMatchHUDWidgets.h"

#include "Components/Image.h"
#include "Components/PanelWidget.h"
#include "Components/ProgressBar.h"
#include "Components/TextBlock.h"
#include "TimerManager.h"
#include "TwoToTangle/UI/Data/TTTMatchHUDConfig.h"

namespace
{
FText TTTPlacementOrdinal(int32 Placement)
{
	const int32 SafePlacement = FMath::Max(1, Placement);
	const int32 LastTwoDigits = SafePlacement % 100;
	const TCHAR* Suffix = TEXT("TH");
	if (LastTwoDigits < 11 || LastTwoDigits > 13)
	{
		switch (SafePlacement % 10)
		{
		case 1: Suffix = TEXT("ST"); break;
		case 2: Suffix = TEXT("ND"); break;
		case 3: Suffix = TEXT("RD"); break;
		default: break;
		}
	}
	return FText::FromString(FString::Printf(TEXT("%d%s"), SafePlacement, Suffix));
}

FText TTTFinishedTitle(const FTTTFinishPresentation& ResultData)
{
	if (ResultData.Placement > 0 && ResultData.Placement <= 3)
	{
		return FText::Format(NSLOCTEXT("TwoToTangle", "PlaceResult", "{0} PLACE!"), TTTPlacementOrdinal(ResultData.Placement));
	}
	return NSLOCTEXT("TwoToTangle", "CourseCleared", "COURSE CLEARED");
}
}

void UTTTRaceCountdownWidget::PresentCountdown(int32 CountdownValue)
{
	SetVisibility(ESlateVisibility::HitTestInvisible);
	if (TXT_Countdown) TXT_Countdown->SetText(FText::AsNumber(FMath::Clamp(CountdownValue, 1, 3)));
	PlayCountdownIn();
}

void UTTTRaceCountdownWidget::PresentGo()
{
	SetVisibility(ESlateVisibility::HitTestInvisible);
	if (TXT_Countdown) TXT_Countdown->SetText(NSLOCTEXT("TwoToTangle", "Go", "GO"));
	PlayCountdownIn();
}

void UTTTStopwatchWidget::SetElapsedSeconds(float ElapsedSeconds)
{
	if (TXT_Stopwatch) TXT_Stopwatch->SetText(FormatRaceTime(ElapsedSeconds));
}

FText UTTTStopwatchWidget::FormatRaceTime(float ElapsedSeconds)
{
	const int32 TotalHundredths = FMath::Max(0, FMath::FloorToInt(ElapsedSeconds * 100.0f + KINDA_SMALL_NUMBER));
	const int32 Minutes = TotalHundredths / 6000;
	const int32 Seconds = (TotalHundredths / 100) % 60;
	const int32 Hundredths = TotalHundredths % 100;
	return FText::FromString(FString::Printf(TEXT("%02d:%02d.%02d"), Minutes, Seconds, Hundredths));
}

void UTTTPlacementWidget::PresentPlacement(int32 Placement, int32 TeamCount)
{
	const int32 SafePlacement = FMath::Max(1, Placement);
	if (TXT_Placement) TXT_Placement->SetText(FormatPlacement(SafePlacement, TeamCount));
	if (PreviousPlacement > 0 && PreviousPlacement != SafePlacement) PlayPlacementChanged(SafePlacement < PreviousPlacement);
	PreviousPlacement = SafePlacement;
}

FText UTTTPlacementWidget::FormatPlacement(int32 Placement, int32 TeamCount)
{
	return FText::Format(
		NSLOCTEXT("TwoToTangle", "PlacementFormat", "{0} / {1} TEAMS"),
		TTTPlacementOrdinal(Placement),
		FText::AsNumber(FMath::Max(1, TeamCount)));
}

void UTTTItemSlotWidget::PresentItem(const FTTTHeldItemPresentation& ItemData)
{
	if (!ItemData.bHasItem) { ClearItem(); return; }
	SetVisibility(ESlateVisibility::HitTestInvisible);
	if (IMG_ItemIcon)
	{
		if (UTexture2D* ItemTexture = ItemData.Icon.LoadSynchronous()) IMG_ItemIcon->SetBrushFromTexture(ItemTexture, true);
		IMG_ItemIcon->SetColorAndOpacity(ItemData.AccentColor);
	}
	PlayItemIn();
}

void UTTTItemSlotWidget::ClearItem()
{
	if (IMG_ItemIcon) IMG_ItemIcon->SetBrushFromTexture(nullptr);
	PlayItemOut();
	SetVisibility(ESlateVisibility::Collapsed);
}

void UTTTThrowStrengthWidget::PresentCharge(float NormalizedCharge)
{
	SetVisibility(ESlateVisibility::HitTestInvisible);
	if (PB_ThrowStrength) PB_ThrowStrength->SetPercent(FMath::Clamp(NormalizedCharge, 0.0f, 1.0f));
}

void UTTTThrowStrengthWidget::HideCharge()
{
	SetVisibility(ESlateVisibility::Collapsed);
}

void UTTTDazeMeterWidget::PresentDaze(float NormalizedDaze, float RecoveryThreshold, bool bRecoveryActive)
{
	const float SafeDaze = FMath::Clamp(NormalizedDaze, 0.0f, 1.0f);
	const float SafeThreshold = FMath::Clamp(RecoveryThreshold, 0.0f, 1.0f);
	if (PB_Daze) PB_Daze->SetPercent(SafeDaze);
	const float MeterWidth = Overlay_Meter ? FMath::Max(1.0f, Overlay_Meter->GetCachedGeometry().GetLocalSize().X) : 260.0f;
	if (IMG_DazePointer) IMG_DazePointer->SetRenderTranslation(FVector2D((SafeDaze - 0.5f) * MeterWidth, 0.0f));
	if (IMG_RecoveryThreshold) IMG_RecoveryThreshold->SetRenderTranslation(FVector2D((SafeThreshold - 0.5f) * MeterWidth, 0.0f));
	SetRecoveryPulseActive(bRecoveryActive);
}

void UTTTStatusMessageWidget::PresentMessage(const FText& PrimaryText, const FText& SecondaryText)
{
	SetVisibility(ESlateVisibility::HitTestInvisible);
	if (TXT_StatusPrimary) TXT_StatusPrimary->SetText(PrimaryText);
	if (TXT_StatusSecondary)
	{
		TXT_StatusSecondary->SetText(SecondaryText);
		TXT_StatusSecondary->SetVisibility(SecondaryText.IsEmpty() ? ESlateVisibility::Collapsed : ESlateVisibility::HitTestInvisible);
	}
	PlayStatusIn();
}

void UTTTStatusMessageWidget::HideMessage()
{
	PlayStatusOut();
	SetVisibility(ESlateVisibility::Collapsed);
}

void UTTTFinishResultWidget::PresentFinished(const FTTTFinishPresentation& ResultData)
{
	SetVisibility(ESlateVisibility::HitTestInvisible);
	if (TXT_Result) TXT_Result->SetText(TTTFinishedTitle(ResultData));
	if (TXT_FinalTime) TXT_FinalTime->SetText(UTTTStopwatchWidget::FormatRaceTime(ResultData.FinalTeamTime));
	if (TXT_ResultSecondary) TXT_ResultSecondary->SetText(FText::GetEmpty());
	PlayFinishIn();
}

void UTTTFinishResultWidget::PresentEliminated(const FTTTFinishPresentation& ResultData)
{
	SetVisibility(ESlateVisibility::HitTestInvisible);
	if (TXT_Result) TXT_Result->SetText(NSLOCTEXT("TwoToTangle", "Eliminated", "ELIMINATED"));
	if (TXT_FinalTime) TXT_FinalTime->SetText(UTTTStopwatchWidget::FormatRaceTime(ResultData.FinalTeamTime));
	if (TXT_ResultSecondary) TXT_ResultSecondary->SetText(NSLOCTEXT("TwoToTangle", "DidNotFinish", "Did not finish the course"));
	PlayEliminatedIn();
}

void UTTTMatchTransitionWidget::PresentMatchOver(const FTTTFinishPresentation& ResultData)
{
	if (TXT_Result) TXT_Result->SetText(NSLOCTEXT("TwoToTangle", "MatchOver", "MATCH OVER"));
	if (TXT_FinalTime) TXT_FinalTime->SetText(UTTTStopwatchWidget::FormatRaceTime(ResultData.FinalTeamTime));
}

void UTTTMatchHUDWidget::ApplySnapshot(const FTTTMatchHUDSnapshot& Snapshot)
{
	SetStopwatchTime_Implementation(Snapshot.RaceElapsedSeconds);
	SetPlacement_Implementation(Snapshot.Placement, Snapshot.TeamCount);
	FTTTHeldItemPresentation ItemData;
	ItemData.bHasItem = Snapshot.bHasHeldItem;
	ItemData.DisplayName = Snapshot.HeldItemName;
	ItemData.Icon = Snapshot.HeldItemIcon;
	ItemData.bCanThrow = Snapshot.bHasHeldItem;
	ItemData.bSupportsCharge = Snapshot.bHasHeldItem;
	Snapshot.bHasHeldItem ? SetHeldItem_Implementation(ItemData) : ClearHeldItem_Implementation();
	Snapshot.bIsChargingThrow ? ShowThrowStrength_Implementation(Snapshot.ThrowChargeNormalized) : HideThrowStrength_Implementation();
	SetDaze_Implementation(Snapshot.DazeNormalized, Snapshot.DazeRecoveryThresholdNormalized);
	if (Snapshot.PhysicalState == ETTTPhysicalState::BeingCarried)
	{
		Snapshot.bCanDismount ? ShowRecoveryAvailable_Implementation() : ShowBeingCarriedMessage_Implementation();
	}
	else if (Snapshot.PhysicalState == ETTTPhysicalState::FullyDazed || Snapshot.PhysicalState == ETTTPhysicalState::Unconscious)
	{
		ShowFullyDazedMessage_Implementation();
	}
	if (Snapshot.FinishResult != ETTTFinishResult::None)
	{
		FTTTFinishPresentation ResultData;
		ResultData.Result = Snapshot.FinishResult;
		ResultData.Placement = Snapshot.Placement;
		ResultData.TeamCount = Snapshot.TeamCount;
		ResultData.FinalTeamTime = Snapshot.FinalTeamTime;
		ResultData.bWaitingForResults = Snapshot.bWaitingForResults;
		Snapshot.FinishResult == ETTTFinishResult::DidNotFinish
			? ShowEliminated_Implementation(ResultData)
			: ShowCourseCleared_Implementation(ResultData);
	}
}

void UTTTMatchHUDWidget::ConfigurePresentation(const UTTTMatchHUDConfig* HUDConfig)
{
	if (!HUDConfig) return;
	DazeMeterShowThreshold = FMath::Clamp(HUDConfig->DazeMeterShowThreshold, 0.0f, 1.0f);
	DazeMeterHideDelay = FMath::Max(0.0f, HUDConfig->DazeMeterHideDelay);
}

void UTTTMatchHUDWidget::NativeDestruct()
{
	if (GetWorld()) GetWorld()->GetTimerManager().ClearTimer(DazeMeterHideTimerHandle);
	Super::NativeDestruct();
}

void UTTTMatchHUDWidget::ShowCountdown_Implementation(int32 CountdownValue)
{
	if (Overlay_TransientCountdown) Overlay_TransientCountdown->SetVisibility(ESlateVisibility::HitTestInvisible);
	if (CountdownWidget) CountdownWidget->PresentCountdown(CountdownValue);
}

void UTTTMatchHUDWidget::ShowGo_Implementation()
{
	if (Overlay_TransientCountdown) Overlay_TransientCountdown->SetVisibility(ESlateVisibility::HitTestInvisible);
	if (CountdownWidget) CountdownWidget->PresentGo();
}

void UTTTMatchHUDWidget::SetStopwatchTime_Implementation(float ElapsedSeconds) { if (StopwatchWidget) StopwatchWidget->SetElapsedSeconds(ElapsedSeconds); }
void UTTTMatchHUDWidget::SetPlacement_Implementation(int32 Placement, int32 TeamCount) { if (PlacementWidget) PlacementWidget->PresentPlacement(Placement, TeamCount); }
void UTTTMatchHUDWidget::SetHeldItem_Implementation(const FTTTHeldItemPresentation& ItemData) { if (ItemSlotWidget) ItemSlotWidget->PresentItem(ItemData); }
void UTTTMatchHUDWidget::ClearHeldItem_Implementation() { if (ItemSlotWidget) ItemSlotWidget->ClearItem(); HideThrowStrength_Implementation(); }
void UTTTMatchHUDWidget::ShowThrowStrength_Implementation(float NormalizedCharge) { if (ThrowStrengthWidget) ThrowStrengthWidget->PresentCharge(NormalizedCharge); }
void UTTTMatchHUDWidget::HideThrowStrength_Implementation() { if (ThrowStrengthWidget) ThrowStrengthWidget->HideCharge(); }

void UTTTMatchHUDWidget::SetDaze_Implementation(float NormalizedDaze, float RecoveryThreshold)
{
	if (DazeMeterWidget)
	{
		if (NormalizedDaze >= DazeMeterShowThreshold)
		{
			if (GetWorld()) GetWorld()->GetTimerManager().ClearTimer(DazeMeterHideTimerHandle);
			DazeMeterWidget->SetVisibility(ESlateVisibility::HitTestInvisible);
		}
		else if (GetWorld() && !GetWorld()->GetTimerManager().IsTimerActive(DazeMeterHideTimerHandle))
		{
			GetWorld()->GetTimerManager().SetTimer(
				DazeMeterHideTimerHandle,
				this,
				&UTTTMatchHUDWidget::CollapseDazeMeter,
				DazeMeterHideDelay,
				false);
		}
		DazeMeterWidget->PresentDaze(NormalizedDaze, RecoveryThreshold, false);
	}
}

void UTTTMatchHUDWidget::CollapseDazeMeter()
{
	if (DazeMeterWidget) DazeMeterWidget->SetVisibility(ESlateVisibility::Collapsed);
}

void UTTTMatchHUDWidget::ShowFullyDazedMessage_Implementation()
{
	HideThrowStrength_Implementation();
	if (Overlay_StatusMessages) Overlay_StatusMessages->SetVisibility(ESlateVisibility::HitTestInvisible);
	if (StatusMessageWidget) StatusMessageWidget->PresentMessage(NSLOCTEXT("TwoToTangle", "DazedTitle", "DAZED"), NSLOCTEXT("TwoToTangle", "DazedHelp", "WAIT FOR YOUR TEAMMATE"));
}

void UTTTMatchHUDWidget::ShowBeingCarriedMessage_Implementation()
{
	if (Overlay_StatusMessages) Overlay_StatusMessages->SetVisibility(ESlateVisibility::HitTestInvisible);
	if (StatusMessageWidget) StatusMessageWidget->PresentMessage(NSLOCTEXT("TwoToTangle", "BeingCarried", "BEING CARRIED"), NSLOCTEXT("TwoToTangle", "Recovering", "Recovering..."));
}

void UTTTMatchHUDWidget::ShowRecoveryAvailable_Implementation()
{
	if (Overlay_StatusMessages) Overlay_StatusMessages->SetVisibility(ESlateVisibility::HitTestInvisible);
	if (StatusMessageWidget) StatusMessageWidget->PresentMessage(NSLOCTEXT("TwoToTangle", "Recovered", "RECOVERED"), NSLOCTEXT("TwoToTangle", "HopOff", "PRESS JUMP TO HOP OFF"));
}

void UTTTMatchHUDWidget::ShowCourseCleared_Implementation(const FTTTFinishPresentation& Result)
{
	ClearHeldItem_Implementation();
	if (DazeMeterWidget) DazeMeterWidget->SetVisibility(ESlateVisibility::Collapsed);
	if (Overlay_FinishPresentation) Overlay_FinishPresentation->SetVisibility(ESlateVisibility::HitTestInvisible);
	if (FinishResultWidget) FinishResultWidget->PresentFinished(Result);
}

void UTTTMatchHUDWidget::ShowEliminated_Implementation(const FTTTFinishPresentation& Result)
{
	ClearHeldItem_Implementation();
	if (DazeMeterWidget) DazeMeterWidget->SetVisibility(ESlateVisibility::Collapsed);
	if (Overlay_FinishPresentation) Overlay_FinishPresentation->SetVisibility(ESlateVisibility::HitTestInvisible);
	if (FinishResultWidget) FinishResultWidget->PresentEliminated(Result);
}

void UTTTMatchHUDWidget::HideTransientPresentation_Implementation()
{
	if (CountdownWidget) CountdownWidget->SetVisibility(ESlateVisibility::Collapsed);
	if (Overlay_TransientCountdown) Overlay_TransientCountdown->SetVisibility(ESlateVisibility::Collapsed);
	if (StatusMessageWidget) StatusMessageWidget->HideMessage();
	if (Overlay_StatusMessages) Overlay_StatusMessages->SetVisibility(ESlateVisibility::Collapsed);
}

void UTTTMatchHUDWidget::BeginResultsTransition_Implementation() { PlayHUDOut(); }
