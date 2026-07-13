#include "PairPressure/PPCarryComponent.h"

#include "Components/SkeletalMeshComponent.h"
#include "Engine/World.h"
#include "GameFramework/Character.h"
#include "Net/UnrealNetwork.h"
#include "PairPressure/PPPhysicalStateComponent.h"
#include "PairPressure/PPTeamMemberComponent.h"

UPPCarryComponent::UPPCarryComponent()
{
	SetIsReplicatedByDefault(true);
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.bStartWithTickEnabled = false;
}

void UPPCarryComponent::BeginPlay()
{
	Super::BeginPlay();

	if (GetWorld() && GetWorld()->GetMapName().Contains(TEXT("PP_")))
	{
		SetComponentTickEnabled(true);
	}
}

void UPPCarryComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

	if (GetOwner() && GetOwner()->HasAuthority() && bAssistActive)
	{
		UpdateAssist(DeltaTime);
	}
}

void UPPCarryComponent::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(UPPCarryComponent, AssistTarget);
	DOREPLIFETIME(UPPCarryComponent, bAssistActive);
}

void UPPCarryComponent::BeginAssist()
{
	if (!GetOwner())
	{
		return;
	}
	if (!GetOwner()->HasAuthority())
	{
		ServerSetAssistActive(true);
		return;
	}

	bAssistActive = true;
	SetAssistTargetAuthoritative(FindBestAssistTarget());
}

void UPPCarryComponent::EndAssist()
{
	if (!GetOwner())
	{
		return;
	}
	if (!GetOwner()->HasAuthority())
	{
		ServerSetAssistActive(false);
		return;
	}

	bAssistActive = false;
	SetAssistTargetAuthoritative(nullptr);
}

void UPPCarryComponent::ServerSetAssistActive_Implementation(bool bNewAssistActive)
{
	if (bNewAssistActive)
	{
		BeginAssist();
	}
	else
	{
		EndAssist();
	}
}

void UPPCarryComponent::OnRep_AssistTarget()
{
	OnAssistTargetChanged.Broadcast(AssistTarget);
}

AActor* UPPCarryComponent::FindBestAssistTarget() const
{
	const AActor* OwnerActor = GetOwner();
	const UWorld* World = GetWorld();
	const UPPTeamMemberComponent* OwnerTeam = UPPTeamMemberComponent::FindTeamComponent(OwnerActor);
	if (!OwnerActor || !World || !OwnerTeam)
	{
		return nullptr;
	}

	const FVector TraceStart = OwnerActor->GetActorLocation() + FVector(0.0f, 0.0f, 55.0f);
	const FVector TraceEnd = TraceStart + OwnerActor->GetActorForwardVector() * AssistReach;
	FCollisionQueryParams QueryParams(SCENE_QUERY_STAT(PairPressureAssist), false, OwnerActor);
	TArray<FHitResult> Hits;
	World->SweepMultiByChannel(
		Hits,
		TraceStart,
		TraceEnd,
		FQuat::Identity,
		ECC_Pawn,
		FCollisionShape::MakeSphere(AssistRadius),
		QueryParams);

	AActor* BestTarget = nullptr;
	float BestDistanceSquared = TNumericLimits<float>::Max();
	for (const FHitResult& Hit : Hits)
	{
		AActor* Candidate = Hit.GetActor();
		UPPPhysicalStateComponent* CandidatePhysicalState = UPPPhysicalStateComponent::FindPhysicalStateComponent(Candidate);
		if (!Candidate || !CandidatePhysicalState || !CandidatePhysicalState->IsRagdolled()
			|| !OwnerTeam->IsFriendlyTo_Implementation(Candidate))
		{
			continue;
		}

		const float CandidateDistanceSquared = FVector::DistSquared(OwnerActor->GetActorLocation(), Candidate->GetActorLocation());
		if (CandidateDistanceSquared < BestDistanceSquared)
		{
			BestTarget = Candidate;
			BestDistanceSquared = CandidateDistanceSquared;
		}
	}

	return BestTarget;
}

void UPPCarryComponent::SetAssistTargetAuthoritative(AActor* NewAssistTarget)
{
	if (AssistTarget == NewAssistTarget)
	{
		return;
	}

	if (UPPPhysicalStateComponent* PreviousPhysicalState = UPPPhysicalStateComponent::FindPhysicalStateComponent(AssistTarget))
	{
		PreviousPhysicalState->OnCarryEnded_Implementation(GetOwner());
	}
	AssistTarget = NewAssistTarget;
	if (UPPPhysicalStateComponent* NewPhysicalState = UPPPhysicalStateComponent::FindPhysicalStateComponent(AssistTarget))
	{
		NewPhysicalState->OnCarryStarted_Implementation(GetOwner());
	}
	OnRep_AssistTarget();
}

void UPPCarryComponent::UpdateAssist(float DeltaTime)
{
	AActor* OwnerActor = GetOwner();
	UPPPhysicalStateComponent* TargetPhysicalState = UPPPhysicalStateComponent::FindPhysicalStateComponent(AssistTarget);
	if (!OwnerActor || !AssistTarget || !TargetPhysicalState || !TargetPhysicalState->IsRagdolled())
	{
		SetAssistTargetAuthoritative(FindBestAssistTarget());
		return;
	}

	const float CurrentDistance = FVector::Dist(OwnerActor->GetActorLocation(), AssistTarget->GetActorLocation());
	if (CurrentDistance > AssistReach * 1.75f)
	{
		SetAssistTargetAuthoritative(nullptr);
		return;
	}

	ACharacter* TargetCharacter = Cast<ACharacter>(AssistTarget);
	USkeletalMeshComponent* TargetMesh = TargetCharacter ? TargetCharacter->GetMesh() : nullptr;
	if (TargetMesh && TargetMesh->IsSimulatingPhysics())
	{
		const FVector DesiredPoint = OwnerActor->GetActorLocation()
			- OwnerActor->GetActorForwardVector() * DragTargetDistance
			+ FVector(0.0f, 0.0f, 35.0f);
		const FVector PullDirection = (DesiredPoint - TargetMesh->GetComponentLocation()).GetClampedToMaxSize(250.0f);
		TargetMesh->AddForce(PullDirection * DragAcceleration, NAME_None, true);
	}

	if (CurrentDistance <= ReviveRange)
	{
		TargetPhysicalState->AddReviveProgress(DeltaTime, OwnerActor);
	}
}
