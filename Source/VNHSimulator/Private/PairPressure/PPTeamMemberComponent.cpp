#include "PairPressure/PPTeamMemberComponent.h"

#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerState.h"
#include "Net/UnrealNetwork.h"
#include "TimerManager.h"

UPPTeamMemberComponent::UPPTeamMemberComponent()
{
	SetIsReplicatedByDefault(true);
	PrimaryComponentTick.bCanEverTick = false;
}

void UPPTeamMemberComponent::BeginPlay()
{
	Super::BeginPlay();

	if (GetWorld() && GetWorld()->GetMapName().Contains(TEXT("PP_")) && GetOwner() && GetOwner()->HasAuthority())
	{
		RefreshAutomaticAssignment();
		if (!Partner && GetWorld())
		{
			GetWorld()->GetTimerManager().SetTimer(
				AssignmentTimerHandle,
				this,
				&UPPTeamMemberComponent::RefreshAutomaticAssignment,
				1.0f,
				true);
		}
	}
}

void UPPTeamMemberComponent::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(UPPTeamMemberComponent, TeamId);
	DOREPLIFETIME(UPPTeamMemberComponent, Partner);
	DOREPLIFETIME(UPPTeamMemberComponent, bIsHome);
	DOREPLIFETIME(UPPTeamMemberComponent, bTeamFinished);
}

bool UPPTeamMemberComponent::IsFriendlyTo_Implementation(const AActor* OtherActor) const
{
	const UPPTeamMemberComponent* OtherTeam = FindTeamComponent(OtherActor);
	return OtherTeam && TeamId != INDEX_NONE && TeamId == OtherTeam->TeamId;
}

void UPPTeamMemberComponent::AssignTeam(int32 NewTeamId, AActor* NewPartner)
{
	AActor* OwnerActor = GetOwner();
	if (!OwnerActor || !OwnerActor->HasAuthority())
	{
		return;
	}

	if (TeamId == NewTeamId && Partner == NewPartner)
	{
		return;
	}

	TeamId = NewTeamId;
	Partner = NewPartner;
	OnPartnerChanged.Broadcast(TeamId, Partner);
}

void UPPTeamMemberComponent::SetFinishState(bool bNewIsHome, bool bNewTeamFinished)
{
	AActor* OwnerActor = GetOwner();
	if (!OwnerActor || !OwnerActor->HasAuthority())
	{
		return;
	}

	bIsHome = bNewIsHome;
	bTeamFinished = bNewTeamFinished;
	OnPartnerChanged.Broadcast(TeamId, Partner);
}

UPPTeamMemberComponent* UPPTeamMemberComponent::FindTeamComponent(const AActor* Actor)
{
	return Actor ? Actor->FindComponentByClass<UPPTeamMemberComponent>() : nullptr;
}

void UPPTeamMemberComponent::RefreshAutomaticAssignment()
{
	APawn* OwnerPawn = Cast<APawn>(GetOwner());
	APlayerState* OwnerPlayerState = OwnerPawn ? OwnerPawn->GetPlayerState() : nullptr;
	if (!OwnerPawn || !OwnerPlayerState || !GetWorld())
	{
		return;
	}

	const int32 PlayerId = FMath::Max(0, OwnerPlayerState->GetPlayerId());
	// Keep native feature components aligned with the asset-only team contract used by
	// BP_PP_PlayerCharacter and the two placed tether-link actors: players alternate teams.
	const int32 AutomaticTeamId = PlayerId % 2;
	AActor* FoundPartner = nullptr;

	for (TActorIterator<APawn> It(GetWorld()); It; ++It)
	{
		APawn* CandidatePawn = *It;
		APlayerState* CandidatePlayerState = CandidatePawn ? CandidatePawn->GetPlayerState() : nullptr;
		if (!CandidatePawn || CandidatePawn == OwnerPawn || !CandidatePlayerState)
		{
			continue;
		}

		const int32 CandidatePlayerId = FMath::Max(0, CandidatePlayerState->GetPlayerId());
		if (CandidatePlayerId % 2 == AutomaticTeamId)
		{
			FoundPartner = CandidatePawn;
			break;
		}
	}

	AssignTeam(AutomaticTeamId, FoundPartner);
	if (FoundPartner)
	{
		GetWorld()->GetTimerManager().ClearTimer(AssignmentTimerHandle);
	}
}

void UPPTeamMemberComponent::OnRep_TeamAssignment()
{
	OnPartnerChanged.Broadcast(TeamId, Partner);
}
