#include "PairPressure/PPHUDRootWidget.h"

#include "Components/TextBlock.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerState.h"
#include "PairPressure/PPPhysicalStateComponent.h"
#include "PairPressure/PPTeamMemberComponent.h"
#include "TimerManager.h"

namespace
{
FText PairPressureStateText(EPPPhysicalState PhysicalState)
{
	switch (PhysicalState)
	{
	case EPPPhysicalState::Reactive: return NSLOCTEXT("PairPressure", "StateReactive", "SHAKEN");
	case EPPPhysicalState::Stumbling: return NSLOCTEXT("PairPressure", "StateStumbling", "STUMBLING");
	case EPPPhysicalState::Falling: return NSLOCTEXT("PairPressure", "StateFalling", "FALLING");
	case EPPPhysicalState::Ragdolled: return NSLOCTEXT("PairPressure", "StateRagdolled", "DOWN — GO BACK");
	case EPPPhysicalState::Unconscious: return NSLOCTEXT("PairPressure", "StateUnconscious", "UNCONSCIOUS — HOLD ASSIST");
	case EPPPhysicalState::Piggybacked: return NSLOCTEXT("PairPressure", "StatePiggybacked", "RIDING");
	default: return NSLOCTEXT("PairPressure", "StateGrounded", "MOVING");
	}
}

FText PairPressureDirectionText(float DirectionDegrees, float DistanceMeters)
{
	const TCHAR* Compass = TEXT("AHEAD");
	if (DirectionDegrees > 22.5f && DirectionDegrees <= 67.5f) Compass = TEXT("FRONT RIGHT");
	else if (DirectionDegrees > 67.5f && DirectionDegrees <= 112.5f) Compass = TEXT("RIGHT");
	else if (DirectionDegrees > 112.5f && DirectionDegrees <= 157.5f) Compass = TEXT("BACK RIGHT");
	else if (DirectionDegrees > 157.5f || DirectionDegrees <= -157.5f) Compass = TEXT("BEHIND");
	else if (DirectionDegrees > -157.5f && DirectionDegrees <= -112.5f) Compass = TEXT("BACK LEFT");
	else if (DirectionDegrees > -112.5f && DirectionDegrees <= -67.5f) Compass = TEXT("LEFT");
	else if (DirectionDegrees > -67.5f && DirectionDegrees <= -22.5f) Compass = TEXT("FRONT LEFT");
	return FText::FromString(FString::Printf(TEXT("%s  //  %.0f m"), Compass, DistanceMeters));
}
}

void UPPHUDRootWidget::NativeConstruct()
{
	Super::NativeConstruct();
	ResolveFeatureComponents();
	RefreshPresentation();

	if (GetWorld())
	{
		GetWorld()->GetTimerManager().SetTimer(
			PresentationTimerHandle,
			this,
			&UPPHUDRootWidget::RefreshPresentation,
			0.2f,
			true);
	}
}

void UPPHUDRootWidget::NativeDestruct()
{
	if (GetWorld())
	{
		GetWorld()->GetTimerManager().ClearTimer(PresentationTimerHandle);
	}
	Super::NativeDestruct();
}

void UPPHUDRootWidget::RefreshPresentation()
{
	ResolveFeatureComponents();
	const FPPHUDSnapshot Snapshot = BuildHUDSnapshot();

	if (ModeLabelText) ModeLabelText->SetText(NSLOCTEXT("PairPressure", "BringHomeMode", "BRING YOUR IDIOT HOME"));
	if (PartnerStateText) PartnerStateText->SetText(FText::Format(NSLOCTEXT("PairPressure", "PartnerStateFormat", "{0}  //  {1}"), Snapshot.PartnerName, Snapshot.PartnerState));
	if (DazeSignalText)
	{
		const FText DazeSignal = Snapshot.DazeNormalized >= 0.75f
			? NSLOCTEXT("PairPressure", "DazeCritical", "SEEING STARS")
			: Snapshot.DazeNormalized >= 0.4f
				? NSLOCTEXT("PairPressure", "DazeUnsteady", "UNSTEADY")
				: FText::GetEmpty();
		DazeSignalText->SetText(DazeSignal);
	}
	if (HeldItemText) HeldItemText->SetText(Snapshot.HeldItemLabel.IsEmpty() ? NSLOCTEXT("PairPressure", "HandsFree", "HANDS FREE") : Snapshot.HeldItemLabel);
	if (HomeStatusText)
	{
		HomeStatusText->SetText(Snapshot.bTeamFinished
			? NSLOCTEXT("PairPressure", "TeamHome", "TEAM HOME  //  +500")
			: Snapshot.bLocalPlayerHome
				? NSLOCTEXT("PairPressure", "WaitForPartner", "YOU'RE HOME — YOUR FRIEND ISN'T")
				: NSLOCTEXT("PairPressure", "BothRequired", "BOTH PLAYERS REQUIRED"));
	}
	if (InteractionPromptText) InteractionPromptText->SetText(Snapshot.InteractionPrompt);

	APawn* OwnerPawn = GetOwningPlayerPawn();
	AActor* Partner = TeamMemberComponent ? TeamMemberComponent->GetAssignedPartner() : nullptr;
	if (PartnerDirectionText && OwnerPawn && Partner)
	{
		const FVector ToPartner = Partner->GetActorLocation() - OwnerPawn->GetActorLocation();
		const float DirectionDegrees = FMath::FindDeltaAngleDegrees(OwnerPawn->GetActorRotation().Yaw, ToPartner.Rotation().Yaw);
		PartnerDirectionText->SetText(PairPressureDirectionText(DirectionDegrees, ToPartner.Size() / 100.0f));
	}
	else if (PartnerDirectionText)
	{
		PartnerDirectionText->SetText(NSLOCTEXT("PairPressure", "FindingPartner", "FINDING TEAMMATE…"));
	}

	OnPresentationDataChanged(Snapshot);
}

FPPHUDSnapshot UPPHUDRootWidget::BuildHUDSnapshot() const
{
	FPPHUDSnapshot Snapshot;
	APawn* OwnerPawn = GetOwningPlayerPawn();
	AActor* Partner = TeamMemberComponent ? TeamMemberComponent->GetAssignedPartner() : nullptr;
	APawn* PartnerPawn = Cast<APawn>(Partner);
	APlayerState* PartnerPlayerState = PartnerPawn ? PartnerPawn->GetPlayerState() : nullptr;
	Snapshot.PartnerName = PartnerPlayerState
		? FText::FromString(PartnerPlayerState->GetPlayerName())
		: NSLOCTEXT("PairPressure", "PartnerUnknown", "TEAMMATE");

	if (const UPPPhysicalStateComponent* PartnerPhysicalState = UPPPhysicalStateComponent::FindPhysicalStateComponent(Partner))
	{
		Snapshot.PartnerState = PairPressureStateText(PartnerPhysicalState->GetCurrentPhysicalState());
	}
	else
	{
		Snapshot.PartnerState = NSLOCTEXT("PairPressure", "PartnerConnecting", "CONNECTING");
	}

	Snapshot.DazeNormalized = PhysicalStateComponent ? PhysicalStateComponent->GetDazeNormalized_Implementation() : 0.0f;
	Snapshot.bLocalPlayerHome = TeamMemberComponent && TeamMemberComponent->IsHome();
	Snapshot.bTeamFinished = TeamMemberComponent && TeamMemberComponent->IsTeamFinished();
	if (const UPPTeamMemberComponent* PartnerTeam = UPPTeamMemberComponent::FindTeamComponent(Partner))
	{
		Snapshot.bPartnerHome = PartnerTeam->IsHome();
	}
	Snapshot.InteractionPrompt = PhysicalStateComponent && PhysicalStateComponent->IsRagdolled()
		? NSLOCTEXT("PairPressure", "RecoverPrompt", "CTRL  //  RECOVER")
		: NSLOCTEXT("PairPressure", "AssistPrompt", "E  //  HOLD TO ASSIST  •  CTRL  //  DIVE");
	return Snapshot;
}

void UPPHUDRootWidget::HandlePhysicalStateChanged(EPPPhysicalState NewState, float DazeNormalized)
{
	RefreshPresentation();
}

void UPPHUDRootWidget::HandleDazeChanged(float DazeNormalized)
{
	RefreshPresentation();
}

void UPPHUDRootWidget::HandlePartnerChanged(int32 NewTeamId, AActor* NewPartner)
{
	RefreshPresentation();
}

void UPPHUDRootWidget::ResolveFeatureComponents()
{
	APawn* OwnerPawn = GetOwningPlayerPawn();
	UPPPhysicalStateComponent* ResolvedPhysicalState = UPPPhysicalStateComponent::FindPhysicalStateComponent(OwnerPawn);
	UPPTeamMemberComponent* ResolvedTeamMember = UPPTeamMemberComponent::FindTeamComponent(OwnerPawn);

	if (PhysicalStateComponent != ResolvedPhysicalState)
	{
		if (PhysicalStateComponent)
		{
			PhysicalStateComponent->OnPhysicalStateChanged.RemoveDynamic(this, &UPPHUDRootWidget::HandlePhysicalStateChanged);
			PhysicalStateComponent->OnDazeChanged.RemoveDynamic(this, &UPPHUDRootWidget::HandleDazeChanged);
		}
		PhysicalStateComponent = ResolvedPhysicalState;
		if (PhysicalStateComponent)
		{
			PhysicalStateComponent->OnPhysicalStateChanged.AddUniqueDynamic(this, &UPPHUDRootWidget::HandlePhysicalStateChanged);
			PhysicalStateComponent->OnDazeChanged.AddUniqueDynamic(this, &UPPHUDRootWidget::HandleDazeChanged);
		}
	}

	if (TeamMemberComponent != ResolvedTeamMember)
	{
		if (TeamMemberComponent)
		{
			TeamMemberComponent->OnPartnerChanged.RemoveDynamic(this, &UPPHUDRootWidget::HandlePartnerChanged);
		}
		TeamMemberComponent = ResolvedTeamMember;
		if (TeamMemberComponent)
		{
			TeamMemberComponent->OnPartnerChanged.AddUniqueDynamic(this, &UPPHUDRootWidget::HandlePartnerChanged);
		}
	}
}
