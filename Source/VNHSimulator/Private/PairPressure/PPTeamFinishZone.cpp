#include "PairPressure/PPTeamFinishZone.h"

#include "Components/BoxComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Components/TextRenderComponent.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerState.h"
#include "PairPressure/PPTeamMemberComponent.h"
#include "UObject/ConstructorHelpers.h"

APPTeamFinishZone::APPTeamFinishZone()
{
	PrimaryActorTick.bCanEverTick = false;
	bReplicates = true;

	SceneRoot = CreateDefaultSubobject<USceneComponent>(TEXT("SceneRoot"));
	SetRootComponent(SceneRoot);

	FinishPlatform = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("FinishPlatform"));
	FinishPlatform->SetupAttachment(SceneRoot);
	FinishPlatform->SetRelativeScale3D(FVector(4.0f, 4.0f, 0.15f));
	FinishPlatform->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);

	FinishVolume = CreateDefaultSubobject<UBoxComponent>(TEXT("FinishVolume"));
	FinishVolume->SetupAttachment(SceneRoot);
	FinishVolume->SetBoxExtent(FVector(400.0f, 400.0f, 180.0f));
	FinishVolume->SetRelativeLocation(FVector(0.0f, 0.0f, 180.0f));
	FinishVolume->SetCollisionProfileName(TEXT("Trigger"));
	FinishVolume->OnComponentBeginOverlap.AddDynamic(this, &APPTeamFinishZone::HandleZoneBeginOverlap);
	FinishVolume->OnComponentEndOverlap.AddDynamic(this, &APPTeamFinishZone::HandleZoneEndOverlap);

	FinishLabel = CreateDefaultSubobject<UTextRenderComponent>(TEXT("FinishLabel"));
	FinishLabel->SetupAttachment(SceneRoot);
	FinishLabel->SetRelativeLocation(FVector(0.0f, 0.0f, 205.0f));
	FinishLabel->SetRelativeRotation(FRotator(0.0f, 90.0f, 0.0f));
	FinishLabel->SetHorizontalAlignment(EHorizTextAligment::EHTA_Center);
	FinishLabel->SetWorldSize(54.0f);
	FinishLabel->SetText(FText::FromString(TEXT("BRING BOTH HOME")));
	FinishLabel->SetTextRenderColor(FColor(65, 235, 190));

	static ConstructorHelpers::FObjectFinder<UStaticMesh> CubeMeshFinder(TEXT("/Engine/BasicShapes/Cube.Cube"));
	if (CubeMeshFinder.Succeeded())
	{
		FinishPlatform->SetStaticMesh(CubeMeshFinder.Object);
	}
}

void APPTeamFinishZone::HandleZoneBeginOverlap(UPrimitiveComponent* OverlappedComponent, AActor* OtherActor,
	UPrimitiveComponent* OtherComponent, int32 OtherBodyIndex, bool bFromSweep, const FHitResult& SweepResult)
{
	if (!HasAuthority() || !OtherActor)
	{
		return;
	}

	if (UPPTeamMemberComponent* TeamComponent = UPPTeamMemberComponent::FindTeamComponent(OtherActor))
	{
		TeamOccupants.FindOrAdd(TeamComponent->GetAssignedTeamId()).Add(OtherActor);
		TeamComponent->SetFinishState(true, false);
		EvaluateTeamFinish(OtherActor);
	}
}

void APPTeamFinishZone::HandleZoneEndOverlap(UPrimitiveComponent* OverlappedComponent, AActor* OtherActor,
	UPrimitiveComponent* OtherComponent, int32 OtherBodyIndex)
{
	if (!HasAuthority() || !OtherActor)
	{
		return;
	}

	if (UPPTeamMemberComponent* TeamComponent = UPPTeamMemberComponent::FindTeamComponent(OtherActor))
	{
		if (TSet<TWeakObjectPtr<AActor>>* Occupants = TeamOccupants.Find(TeamComponent->GetAssignedTeamId()))
		{
			Occupants->Remove(OtherActor);
		}
		if (!FinishedTeams.Contains(TeamComponent->GetAssignedTeamId()))
		{
			TeamComponent->SetFinishState(false, false);
		}
	}
}

void APPTeamFinishZone::EvaluateTeamFinish(AActor* EnteringActor)
{
	UPPTeamMemberComponent* TeamComponent = UPPTeamMemberComponent::FindTeamComponent(EnteringActor);
	if (!TeamComponent || TeamComponent->GetAssignedTeamId() == INDEX_NONE || FinishedTeams.Contains(TeamComponent->GetAssignedTeamId()))
	{
		return;
	}

	AActor* Partner = TeamComponent->GetAssignedPartner();
	const TSet<TWeakObjectPtr<AActor>>* Occupants = TeamOccupants.Find(TeamComponent->GetAssignedTeamId());
	if (!Partner || !Occupants || !Occupants->Contains(Partner))
	{
		return;
	}

	FinishedTeams.Add(TeamComponent->GetAssignedTeamId());
	TeamComponent->SetFinishState(true, true);
	if (UPPTeamMemberComponent* PartnerTeamComponent = UPPTeamMemberComponent::FindTeamComponent(Partner))
	{
		PartnerTeamComponent->SetFinishState(true, true);
	}
	AwardTeamFinishScore(EnteringActor, Partner);
	OnTeamFinished.Broadcast(TeamComponent->GetAssignedTeamId(), EnteringActor, Partner);
}

void APPTeamFinishZone::AwardTeamFinishScore(AActor* FirstPartner, AActor* SecondPartner) const
{
	for (AActor* TeamActor : {FirstPartner, SecondPartner})
	{
		APawn* TeamPawn = Cast<APawn>(TeamActor);
		APlayerState* TeamPlayerState = TeamPawn ? TeamPawn->GetPlayerState() : nullptr;
		if (TeamPlayerState)
		{
			TeamPlayerState->SetScore(TeamPlayerState->GetScore() + 250.0f);
		}
	}
}
