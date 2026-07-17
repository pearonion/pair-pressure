#include "PairPressure/PPImpactSensorComponent.h"

#include "Components/CapsuleComponent.h"
#include "Components/PrimitiveComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/World.h"
#include "GameFramework/Character.h"
#include "PairPressure/PPGameplayTypes.h"
#include "PairPressure/PPPhysicalStateComponent.h"

UPPImpactSensorComponent::UPPImpactSensorComponent()
{
	PrimaryComponentTick.bCanEverTick = false;
}

void UPPImpactSensorComponent::BeginPlay()
{
	Super::BeginPlay();

	if (!GetWorld() || (!GetWorld()->GetMapName().Contains(TEXT("PP_"))
		&& !GetWorld()->GetMapName().Contains(TEXT("Lobby"))))
	{
		return;
	}

	ACharacter* OwnerCharacter = Cast<ACharacter>(GetOwner());
	if (!OwnerCharacter)
	{
		return;
	}

	if (UCapsuleComponent* OwnerCapsule = OwnerCharacter->GetCapsuleComponent())
	{
		OwnerCapsule->OnComponentHit.AddDynamic(this, &UPPImpactSensorComponent::HandleComponentHit);
	}
	if (USkeletalMeshComponent* OwnerMesh = OwnerCharacter->GetMesh())
	{
		OwnerMesh->OnComponentHit.AddDynamic(this, &UPPImpactSensorComponent::HandleComponentHit);
	}
}

void UPPImpactSensorComponent::ReportImpact(
	float Severity,
	AActor* InstigatorActor,
	FName BodyRegion,
	bool bHeavyObstacle)
{
	AActor* OwnerActor = GetOwner();
	UPPPhysicalStateComponent* PhysicalState = UPPPhysicalStateComponent::FindPhysicalStateComponent(OwnerActor);
	if (!OwnerActor || !OwnerActor->HasAuthority() || !PhysicalState)
	{
		return;
	}

	FPPImpactData ImpactData;
	ImpactData.Severity = FMath::Clamp(Severity, 0.0f, 150.0f);
	ImpactData.ImpactPoint = OwnerActor->GetActorLocation();
	ImpactData.ImpactDirection = FVector::UpVector;
	ImpactData.BodyRegion = BodyRegion;
	ImpactData.InstigatorActor = InstigatorActor;
	ImpactData.bHeavyObstacle = bHeavyObstacle;
	PhysicalState->ReceiveImpactData_Implementation(ImpactData);
}

void UPPImpactSensorComponent::HandleComponentHit(
	UPrimitiveComponent* HitComponent,
	AActor* OtherActor,
	UPrimitiveComponent* OtherComponent,
	FVector NormalImpulse,
	const FHitResult& Hit)
{
	AActor* OwnerActor = GetOwner();
	UPPPhysicalStateComponent* PhysicalState = UPPPhysicalStateComponent::FindPhysicalStateComponent(OwnerActor);
	if (!OwnerActor || !OwnerActor->HasAuthority() || !PhysicalState || OtherActor == OwnerActor || !CanReportImpact(OtherActor))
	{
		return;
	}

	float Severity = CalculateImpactSeverity(HitComponent, OtherComponent, NormalImpulse, Hit);
	const bool bSpinnerImpact = OtherActor
		&& (OtherActor->ActorHasTag(TEXT("PP_SpinnerObstacle"))
			|| OtherActor->GetName().Contains(TEXT("Spinner_V2"), ESearchCase::IgnoreCase));
	if (bSpinnerImpact)
	{
		// Slow spinner grazes should still topple the mascot, but must not be
		// promoted to a mid-strength launch.
		Severity = FMath::Max(Severity, 30.0f);
	}
	if (Severity < MinimumReportedSeverity)
	{
		return;
	}

	FPPImpactData ImpactData;
	ImpactData.Severity = Severity;
	ImpactData.ImpactPoint = Hit.ImpactPoint;
	ImpactData.ImpactDirection = NormalImpulse.IsNearlyZero() ? -Hit.ImpactNormal : NormalImpulse.GetSafeNormal();
	ImpactData.BodyRegion = Hit.BoneName;
	ImpactData.InstigatorActor = OtherActor;
	ImpactData.bHeavyObstacle = bSpinnerImpact || (OtherActor && OtherActor->ActorHasTag(HeavyObstacleTag));
	PhysicalState->ReceiveImpactData_Implementation(ImpactData);
	RememberImpact(OtherActor);
}

float UPPImpactSensorComponent::CalculateImpactSeverity(
	const UPrimitiveComponent* HitComponent,
	const UPrimitiveComponent* OtherComponent,
	const FVector& NormalImpulse,
	const FHitResult& Hit) const
{
	const FVector OwnerVelocity = HitComponent ? HitComponent->GetComponentVelocity() : FVector::ZeroVector;
	const FVector OtherVelocity = OtherComponent ? OtherComponent->GetComponentVelocity() : FVector::ZeroVector;
	const float RelativeSpeed = (OwnerVelocity - OtherVelocity).Size();
	const float SpeedSeverity = FMath::GetMappedRangeValueClamped(
		FVector2D(200.0f, HeavyRelativeSpeed),
		FVector2D(0.0f, 100.0f),
		RelativeSpeed);
	const float ImpulseSeverity = FMath::Clamp(NormalImpulse.Size() / HeavyNormalImpulse * 100.0f, 0.0f, 150.0f);

	float ResultSeverity = FMath::Max(SpeedSeverity, ImpulseSeverity);
	if (Hit.BoneName.ToString().Contains(TEXT("head"), ESearchCase::IgnoreCase))
	{
		ResultSeverity *= 1.25f;
	}
	return FMath::Clamp(ResultSeverity, 0.0f, 150.0f);
}

bool UPPImpactSensorComponent::CanReportImpact(AActor* OtherActor) const
{
	if (!OtherActor || SameActorCooldownSeconds <= 0.0f || !GetWorld())
	{
		return true;
	}

	const TWeakObjectPtr<AActor> OtherActorKey(OtherActor);
	const double* PreviousImpactTime = LastImpactTimes.Find(OtherActorKey);
	return !PreviousImpactTime || GetWorld()->GetTimeSeconds() - *PreviousImpactTime >= SameActorCooldownSeconds;
}

void UPPImpactSensorComponent::RememberImpact(AActor* OtherActor)
{
	if (OtherActor && GetWorld())
	{
		LastImpactTimes.FindOrAdd(TWeakObjectPtr<AActor>(OtherActor)) = GetWorld()->GetTimeSeconds();
	}
}
