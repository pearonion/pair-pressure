#include "TwoToTangle/UI/Components/TTTMatchHUDPresenterComponent.h"

#include "Blueprint/UserWidget.h"
#include "Engine/LocalPlayer.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "TimerManager.h"
#include "TwoToTangle/UI/Components/TTTPlayerHUDSourceComponent.h"
#include "TwoToTangle/UI/Data/TTTMatchHUDConfig.h"
#include "TwoToTangle/UI/Interfaces/TTTUIInterfaces.h"
#include "TwoToTangle/UI/Widgets/TTTMatchHUDWidgets.h"

UTTTMatchHUDPresenterComponent::UTTTMatchHUDPresenterComponent()
{
	PrimaryComponentTick.bCanEverTick = false;
	SetIsReplicatedByDefault(false);
	HUDConfigAsset = TSoftObjectPtr<UTTTMatchHUDConfig>(FSoftObjectPath(TEXT("/Game/PairPressure/UI/Styles/DA_MatchHUDConfig.DA_MatchHUDConfig")));
}

void UTTTMatchHUDPresenterComponent::BeginPlay()
{
	Super::BeginPlay();
	if (!bEnableMatchHUD)
	{
		return;
	}

	ResolvedConfig = HUDConfigAsset.LoadSynchronous();
	if (!ResolvedConfig || !ResolvedConfig->MatchHUDClass)
	{
		return;
	}

	TryInitializeHUD();
	if (!MatchHUDWidget && GetWorld())
	{
		GetWorld()->GetTimerManager().SetTimer(InitializationRetryTimerHandle, this, &UTTTMatchHUDPresenterComponent::TryInitializeHUD, 0.25f, true);
	}
}

void UTTTMatchHUDPresenterComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	TeardownHUD();
	Super::EndPlay(EndPlayReason);
}

void UTTTMatchHUDPresenterComponent::TryInitializeHUD()
{
	if (!bEnableMatchHUD)
	{
		return;
	}

	APlayerController* PlayerController = Cast<APlayerController>(GetOwner());
	if (!PlayerController || !PlayerController->IsLocalController() || !PlayerController->GetLocalPlayer() || !GetWorld()) return;
	if (!GetWorld()->GetMapName().Contains(TEXT("PP_"))) return;

	APawn* ControlledPawn = PlayerController->GetPawn();
	UTTTPlayerHUDSourceComponent* CurrentPawnSource = ControlledPawn ? ControlledPawn->FindComponentByClass<UTTTPlayerHUDSourceComponent>() : nullptr;
	if (!CurrentPawnSource) return;
	if (MatchHUDWidget)
	{
		if (CurrentPawnSource != PlayerHUDSource)
		{
			UnsubscribeFromSource();
			PlayerHUDSource = CurrentPawnSource;
			SubscribeToSource();
			PushInitialSnapshot();
		}
		return;
	}
	PlayerHUDSource = CurrentPawnSource;

	if (!ResolvedConfig)
	{
		ResolvedConfig = HUDConfigAsset.LoadSynchronous();
	}
	if (!ResolvedConfig || !ResolvedConfig->MatchHUDClass) return;
	MatchHUDWidget = CreateWidget<UTTTMatchHUDWidget>(PlayerController, ResolvedConfig->MatchHUDClass);
	if (!MatchHUDWidget) return;

	MatchHUDWidget->ConfigurePresentation(ResolvedConfig);
	MatchHUDWidget->AddToViewport(25);
	SubscribeToSource();
	PushInitialSnapshot();
	GetWorld()->GetTimerManager().ClearTimer(InitializationRetryTimerHandle);
}

void UTTTMatchHUDPresenterComponent::TeardownHUD()
{
	UnsubscribeFromSource();
	if (GetWorld())
	{
		GetWorld()->GetTimerManager().ClearTimer(InitializationRetryTimerHandle);
		GetWorld()->GetTimerManager().ClearTimer(ResultFadeTimerHandle);
		GetWorld()->GetTimerManager().ClearTimer(ResultTransitionTimerHandle);
	}
	if (MatchHUDWidget) MatchHUDWidget->RemoveFromParent();
	if (MatchTransitionWidget) MatchTransitionWidget->RemoveFromParent();
	MatchHUDWidget = nullptr;
	MatchTransitionWidget = nullptr;
	PlayerHUDSource = nullptr;
	ResolvedConfig = nullptr;
	bResultTransitionPending = false;
}

void UTTTMatchHUDPresenterComponent::ToggleHUD()
{
	if (!MatchHUDWidget) { TryInitializeHUD(); return; }
	MatchHUDWidget->SetVisibility(MatchHUDWidget->GetVisibility() == ESlateVisibility::Collapsed ? ESlateVisibility::HitTestInvisible : ESlateVisibility::Collapsed);
}

void UTTTMatchHUDPresenterComponent::SubscribeToSource()
{
	ITTTUINotificationSourceInterface* NotificationSource = Cast<ITTTUINotificationSourceInterface>(PlayerHUDSource);
	if (!NotificationSource) return;
	NotificationSource->OnRacePhaseChanged().AddUObject(this, &UTTTMatchHUDPresenterComponent::HandleRacePhaseChanged);
	NotificationSource->OnCountdownValueChanged().AddUObject(this, &UTTTMatchHUDPresenterComponent::HandleCountdownValueChanged);
	NotificationSource->OnRaceClockUpdated().AddUObject(this, &UTTTMatchHUDPresenterComponent::HandleRaceClockUpdated);
	NotificationSource->OnPlacementChanged().AddUObject(this, &UTTTMatchHUDPresenterComponent::HandlePlacementChanged);
	NotificationSource->OnHeldItemChanged().AddUObject(this, &UTTTMatchHUDPresenterComponent::HandleHeldItemChanged);
	NotificationSource->OnThrowChargeStarted().AddUObject(this, &UTTTMatchHUDPresenterComponent::HandleThrowChargeStarted);
	NotificationSource->OnThrowChargeChanged().AddUObject(this, &UTTTMatchHUDPresenterComponent::HandleThrowChargeChanged);
	NotificationSource->OnThrowChargeEnded().AddUObject(this, &UTTTMatchHUDPresenterComponent::HandleThrowChargeEnded);
	NotificationSource->OnDazeChanged().AddUObject(this, &UTTTMatchHUDPresenterComponent::HandleDazeChanged);
	NotificationSource->OnPhysicalStateChanged().AddUObject(this, &UTTTMatchHUDPresenterComponent::HandlePhysicalStateChanged);
	NotificationSource->OnCarryStateChanged().AddUObject(this, &UTTTMatchHUDPresenterComponent::HandleCarryStateChanged);
	NotificationSource->OnMovementRestored().AddUObject(this, &UTTTMatchHUDPresenterComponent::HandleMovementRestored);
	NotificationSource->OnTeamFinished().AddUObject(this, &UTTTMatchHUDPresenterComponent::HandleTeamFinished);
	NotificationSource->OnTeamEliminated().AddUObject(this, &UTTTMatchHUDPresenterComponent::HandleTeamEliminated);
	NotificationSource->OnResultsTransitionRequested().AddUObject(this, &UTTTMatchHUDPresenterComponent::HandleResultsTransitionRequested);
}

void UTTTMatchHUDPresenterComponent::UnsubscribeFromSource()
{
	ITTTUINotificationSourceInterface* NotificationSource = Cast<ITTTUINotificationSourceInterface>(PlayerHUDSource);
	if (!NotificationSource) return;
	NotificationSource->OnRacePhaseChanged().RemoveAll(this);
	NotificationSource->OnCountdownValueChanged().RemoveAll(this);
	NotificationSource->OnRaceClockUpdated().RemoveAll(this);
	NotificationSource->OnPlacementChanged().RemoveAll(this);
	NotificationSource->OnHeldItemChanged().RemoveAll(this);
	NotificationSource->OnThrowChargeStarted().RemoveAll(this);
	NotificationSource->OnThrowChargeChanged().RemoveAll(this);
	NotificationSource->OnThrowChargeEnded().RemoveAll(this);
	NotificationSource->OnDazeChanged().RemoveAll(this);
	NotificationSource->OnPhysicalStateChanged().RemoveAll(this);
	NotificationSource->OnCarryStateChanged().RemoveAll(this);
	NotificationSource->OnMovementRestored().RemoveAll(this);
	NotificationSource->OnTeamFinished().RemoveAll(this);
	NotificationSource->OnTeamEliminated().RemoveAll(this);
	NotificationSource->OnResultsTransitionRequested().RemoveAll(this);
}

void UTTTMatchHUDPresenterComponent::PushInitialSnapshot()
{
	if (!MatchHUDWidget || !PlayerHUDSource) return;
	const FTTTMatchHUDSnapshot Snapshot = PlayerHUDSource->GetMatchHUDSnapshot_Implementation();
	MatchHUDWidget->ApplySnapshot(Snapshot);
	if (Snapshot.FinishResult != ETTTFinishResult::None)
	{
		FTTTFinishPresentation ResultData;
		ResultData.Result = Snapshot.FinishResult;
		ResultData.Placement = Snapshot.Placement;
		ResultData.TeamCount = Snapshot.TeamCount;
		ResultData.FinalTeamTime = Snapshot.FinalTeamTime;
		ResultData.bWaitingForResults = Snapshot.bWaitingForResults;
		BeginResultTimer(ResultData);
	}
}

void UTTTMatchHUDPresenterComponent::HandleRacePhaseChanged(ETTTRacePhase NewPhase)
{
	if (!MatchHUDWidget) return;
	if (NewPhase == ETTTRacePhase::Racing)
	{
		ITTTMatchHUDInterface::Execute_ShowGo(MatchHUDWidget);
		if (GetWorld())
		{
			FTimerHandle GoHideHandle;
			const float GoSeconds = ResolvedConfig ? ResolvedConfig->GoDisplayDuration : 0.7f;
			GetWorld()->GetTimerManager().SetTimer(GoHideHandle, FTimerDelegate::CreateWeakLambda(MatchHUDWidget, [WeakHUD = TWeakObjectPtr<UTTTMatchHUDWidget>(MatchHUDWidget)]()
			{
				if (WeakHUD.IsValid()) ITTTMatchHUDInterface::Execute_HideTransientPresentation(WeakHUD.Get());
			}), GoSeconds, false);
		}
	}
}

void UTTTMatchHUDPresenterComponent::HandleCountdownValueChanged(int32 CountdownValue) { if (MatchHUDWidget) ITTTMatchHUDInterface::Execute_ShowCountdown(MatchHUDWidget, CountdownValue); }
void UTTTMatchHUDPresenterComponent::HandleRaceClockUpdated(float ElapsedSeconds) { if (MatchHUDWidget) ITTTMatchHUDInterface::Execute_SetStopwatchTime(MatchHUDWidget, ElapsedSeconds); }
void UTTTMatchHUDPresenterComponent::HandlePlacementChanged(int32 Placement, int32 TeamCount) { if (MatchHUDWidget) ITTTMatchHUDInterface::Execute_SetPlacement(MatchHUDWidget, Placement, TeamCount); }
void UTTTMatchHUDPresenterComponent::HandleHeldItemChanged(const FTTTHeldItemPresentation& ItemData) { if (MatchHUDWidget) ItemData.bHasItem ? ITTTMatchHUDInterface::Execute_SetHeldItem(MatchHUDWidget, ItemData) : ITTTMatchHUDInterface::Execute_ClearHeldItem(MatchHUDWidget); }
void UTTTMatchHUDPresenterComponent::HandleThrowChargeStarted() { if (MatchHUDWidget) ITTTMatchHUDInterface::Execute_ShowThrowStrength(MatchHUDWidget, 0.0f); }
void UTTTMatchHUDPresenterComponent::HandleThrowChargeChanged(float NormalizedCharge) { if (MatchHUDWidget) ITTTMatchHUDInterface::Execute_ShowThrowStrength(MatchHUDWidget, NormalizedCharge); }
void UTTTMatchHUDPresenterComponent::HandleThrowChargeEnded() { if (MatchHUDWidget) ITTTMatchHUDInterface::Execute_HideThrowStrength(MatchHUDWidget); }
void UTTTMatchHUDPresenterComponent::HandleDazeChanged(float NormalizedDaze, float RecoveryThreshold) { if (MatchHUDWidget) ITTTMatchHUDInterface::Execute_SetDaze(MatchHUDWidget, NormalizedDaze, RecoveryThreshold); }

void UTTTMatchHUDPresenterComponent::HandlePhysicalStateChanged(ETTTPhysicalState NewState)
{
	if (!MatchHUDWidget) return;
	if (NewState == ETTTPhysicalState::FullyDazed || NewState == ETTTPhysicalState::Unconscious) ITTTMatchHUDInterface::Execute_ShowFullyDazedMessage(MatchHUDWidget);
	else if (NewState == ETTTPhysicalState::BeingCarried) ITTTMatchHUDInterface::Execute_ShowBeingCarriedMessage(MatchHUDWidget);
	else ITTTMatchHUDInterface::Execute_HideTransientPresentation(MatchHUDWidget);
}

void UTTTMatchHUDPresenterComponent::HandleCarryStateChanged(bool bIsCarried, bool bCanDismount)
{
	if (!MatchHUDWidget) return;
	if (!bIsCarried) return;
	bCanDismount ? ITTTMatchHUDInterface::Execute_ShowRecoveryAvailable(MatchHUDWidget) : ITTTMatchHUDInterface::Execute_ShowBeingCarriedMessage(MatchHUDWidget);
}

void UTTTMatchHUDPresenterComponent::HandleMovementRestored() { if (MatchHUDWidget) ITTTMatchHUDInterface::Execute_HideTransientPresentation(MatchHUDWidget); }
void UTTTMatchHUDPresenterComponent::HandleTeamFinished(const FTTTFinishPresentation& ResultData) { if (MatchHUDWidget) ITTTMatchHUDInterface::Execute_ShowCourseCleared(MatchHUDWidget, ResultData); BeginResultTimer(ResultData); }
void UTTTMatchHUDPresenterComponent::HandleTeamEliminated(const FTTTFinishPresentation& ResultData) { if (MatchHUDWidget) ITTTMatchHUDInterface::Execute_ShowEliminated(MatchHUDWidget, ResultData); BeginResultTimer(ResultData); }
void UTTTMatchHUDPresenterComponent::HandleResultsTransitionRequested() { if (!bResultTransitionPending) BeginResultTimer(PendingResult); }

void UTTTMatchHUDPresenterComponent::BeginResultTimer(const FTTTFinishPresentation& ResultData)
{
	if (bResultTransitionPending || !GetWorld()) return;
	bResultTransitionPending = true;
	PendingResult = ResultData;
	const float DisplaySeconds = ResolvedConfig ? ResolvedConfig->ResultDisplayDuration : 5.0f;
	const float FadeSeconds = ResolvedConfig ? ResolvedConfig->ResultFadeDuration : 0.5f;
	GetWorld()->GetTimerManager().SetTimer(ResultFadeTimerHandle, this, &UTTTMatchHUDPresenterComponent::BeginHUDFadeOut, FMath::Max(0.0f, DisplaySeconds - FadeSeconds), false);
	GetWorld()->GetTimerManager().SetTimer(ResultTransitionTimerHandle, this, &UTTTMatchHUDPresenterComponent::CompleteResultsTransition, DisplaySeconds, false);
}

void UTTTMatchHUDPresenterComponent::BeginHUDFadeOut() { if (MatchHUDWidget) ITTTMatchHUDInterface::Execute_BeginResultsTransition(MatchHUDWidget); }

void UTTTMatchHUDPresenterComponent::CompleteResultsTransition()
{
	APlayerController* PlayerController = Cast<APlayerController>(GetOwner());
	if (!PlayerController || !ResolvedConfig || !ResolvedConfig->DummyMatchOverClass) return;
	if (MatchHUDWidget) MatchHUDWidget->RemoveFromParent();
	MatchTransitionWidget = CreateWidget<UTTTMatchTransitionWidget>(PlayerController, ResolvedConfig->DummyMatchOverClass);
	if (MatchTransitionWidget)
	{
		MatchTransitionWidget->PresentMatchOver(PendingResult);
		MatchTransitionWidget->AddToViewport(30);
	}
}
