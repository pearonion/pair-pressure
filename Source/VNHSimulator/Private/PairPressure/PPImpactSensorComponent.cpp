#include "PairPressure/PPImpactSensorComponent.h"

#include "Components/CapsuleComponent.h"
#include "Components/PrimitiveComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Character.h"
#include "PairPressure/PPGameplayTypes.h"
#include "PairPressure/PPPhysicalStateComponent.h"
#include "UObject/UnrealType.h"

UPPImpactSensorComponent::UPPImpactSensorComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.bStartWithTickEnabled = false;
	PrimaryComponentTick.TickInterval = 0.05f;
}

namespace
{
constexpr double PPImpactMaximumMotionSampleAgeSeconds = 0.12;

bool IsPPImpactCourseObstacle(const AActor* ObstacleActor)
{
	if (!ObstacleActor)
	{
		return false;
	}

	const FString ObstacleName = ObstacleActor->GetName();
	return ObstacleActor->ActorHasTag(TEXT("PP_SpinnerObstacle"))
		|| ObstacleName.Contains(TEXT("Spinner_V2"), ESearchCase::IgnoreCase)
		|| ObstacleName.Contains(TEXT("SwingBall"), ESearchCase::IgnoreCase)
		|| ObstacleName.Contains(TEXT("SwingHammer"), ESearchCase::IgnoreCase)
		|| ObstacleName.Contains(TEXT("Turntable"), ESearchCase::IgnoreCase)
		|| ObstacleName.Contains(TEXT("Drop_V1"), ESearchCase::IgnoreCase)
		|| ObstacleName.Contains(TEXT("Pusher_V1"), ESearchCase::IgnoreCase)
		|| ObstacleName.Contains(TEXT("Pusher_V2"), ESearchCase::IgnoreCase);
}

bool IsPPImpactSensorSpinnerStyleCourseObstacle(const AActor* ObstacleActor)
{
	if (!ObstacleActor)
	{
		return false;
	}

	const FString ObstacleName = ObstacleActor->GetName();
	return ObstacleActor->ActorHasTag(TEXT("PP_SpinnerObstacle"))
		|| ObstacleName.Contains(TEXT("Spinner_V2"), ESearchCase::IgnoreCase)
		|| ObstacleName.Contains(TEXT("SwingBall"), ESearchCase::IgnoreCase)
		|| ObstacleName.Contains(TEXT("SwingHammer"), ESearchCase::IgnoreCase)
		|| ObstacleName.Contains(TEXT("Turntable"), ESearchCase::IgnoreCase);
}

bool IsPPImpactSensorPusherObstacle(const AActor* ObstacleActor)
{
	const FString ObstacleName = ObstacleActor ? ObstacleActor->GetName() : FString();
	return ObstacleName.Contains(TEXT("Pusher_V1"), ESearchCase::IgnoreCase)
		|| ObstacleName.Contains(TEXT("Pusher_V2"), ESearchCase::IgnoreCase);
}

bool IsPPImpactSensorPusherContactComponent(const AActor* ObstacleActor, const UPrimitiveComponent* ObstacleComponent)
{
	if (!IsPPImpactSensorPusherObstacle(ObstacleActor) || !ObstacleComponent)
	{
		return false;
	}

	const FString ObstacleName = ObstacleActor->GetName();
	const FString ComponentName = ObstacleComponent->GetName();
	if (ObstacleName.Contains(TEXT("Pusher_V1"), ESearchCase::IgnoreCase))
	{
		return ComponentName.Equals(TEXT("SM_Spinner_V1"), ESearchCase::IgnoreCase);
	}
	return ComponentName.Equals(TEXT("SM_Pusher_V1"), ESearchCase::IgnoreCase)
		|| ComponentName.Equals(TEXT("StaticMesh"), ESearchCase::IgnoreCase)
		|| ComponentName.Equals(TEXT("StaticMesh1"), ESearchCase::IgnoreCase);
}

bool IsPPDropPropellerImpact(const AActor* ObstacleActor, const UPrimitiveComponent* HitObstacleComponent)
{
	if (!ObstacleActor || !HitObstacleComponent || !ObstacleActor->GetName().Contains(TEXT("Drop_V1"), ESearchCase::IgnoreCase))
	{
		return false;
	}

	const FString ComponentName = HitObstacleComponent->GetName();
	return ComponentName.Contains(TEXT("Propeller_V1"), ESearchCase::IgnoreCase)
		|| ComponentName.Equals(TEXT("StaticMesh7"), ESearchCase::IgnoreCase);
}

bool ReadPPAuthoredObstacleSpeed(const AActor* ObstacleActor, FName PropertyName, float& OutObstacleSpeed)
{
	if (!ObstacleActor || PropertyName.IsNone())
	{
		return false;
	}

	const FProperty* SpeedProperty = ObstacleActor->GetClass()->FindPropertyByName(PropertyName);
	const FNumericProperty* NumericSpeedProperty = CastField<FNumericProperty>(SpeedProperty);
	if (!NumericSpeedProperty)
	{
		return false;
	}

	const void* SpeedValueAddress = NumericSpeedProperty->ContainerPtrToValuePtr<void>(ObstacleActor);
	const double NumericSpeedValue = NumericSpeedProperty->IsFloatingPoint()
		? NumericSpeedProperty->GetFloatingPointPropertyValue(SpeedValueAddress)
		: static_cast<double>(NumericSpeedProperty->GetSignedIntPropertyValue(SpeedValueAddress));
	OutObstacleSpeed = FMath::Max(0.0f, static_cast<float>(NumericSpeedValue));
	return FMath::IsFinite(OutObstacleSpeed);
}

float ResolvePPAuthoredCourseObstacleSpeed(
	const AActor* ObstacleActor,
	const UPrimitiveComponent* HitObstacleComponent)
{
	if (!ObstacleActor)
	{
		return -1.0f;
	}

	FName SpeedPropertyName = NAME_None;
	if (IsPPImpactSensorSpinnerStyleCourseObstacle(ObstacleActor)
		|| IsPPDropPropellerImpact(ObstacleActor, HitObstacleComponent))
	{
		SpeedPropertyName = FName(TEXT("SpinnerSpeed"));
	}
	else if (IsPPImpactSensorPusherContactComponent(ObstacleActor, HitObstacleComponent))
	{
		const FString ObstacleName = ObstacleActor->GetName();
		if (ObstacleName.Contains(TEXT("Pusher_V1"), ESearchCase::IgnoreCase))
		{
			SpeedPropertyName = FName(TEXT("Speed"));
		}
		else
		{
			const FString ComponentName = HitObstacleComponent->GetName();
			if (ComponentName.Equals(TEXT("StaticMesh"), ESearchCase::IgnoreCase))
			{
				SpeedPropertyName = FName(TEXT("Pusher1Speed"));
			}
			else if (ComponentName.Equals(TEXT("SM_Pusher_V1"), ESearchCase::IgnoreCase))
			{
				SpeedPropertyName = FName(TEXT("Pusher2Speed"));
			}
			else if (ComponentName.Equals(TEXT("StaticMesh1"), ESearchCase::IgnoreCase))
			{
				SpeedPropertyName = FName(TEXT("Pusher3Speed"));
			}
		}
	}

	float AuthoredObstacleSpeed = 0.0f;
	if (SpeedPropertyName.IsNone()
		|| !ReadPPAuthoredObstacleSpeed(ObstacleActor, SpeedPropertyName, AuthoredObstacleSpeed))
	{
		// A recognized course obstacle must never fall back to measured collision
		// force. Missing authored data therefore selects the bounded quick profile.
		return SpeedPropertyName.IsNone() ? -1.0f : 0.0f;
	}
	return AuthoredObstacleSpeed;
}
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
	if (!OwnerCharacter || !OwnerCharacter->HasAuthority())
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
	for (TActorIterator<AActor> ActorIterator(GetWorld()); ActorIterator; ++ActorIterator)
	{
		RegisterCourseObstacleActor(*ActorIterator);
	}
	CourseObstacleSpawnedDelegateHandle = GetWorld()->AddOnActorSpawnedHandler(
		FOnActorSpawned::FDelegate::CreateUObject(this, &UPPImpactSensorComponent::HandleCourseObstacleSpawned));
	SetComponentTickEnabled(!CourseObstacleMotionSamples.IsEmpty());
}

void UPPImpactSensorComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (CourseObstacleSpawnedDelegateHandle.IsValid() && GetWorld())
	{
		GetWorld()->RemoveOnActorSpawnedHandler(CourseObstacleSpawnedDelegateHandle);
		CourseObstacleSpawnedDelegateHandle.Reset();
	}
	Super::EndPlay(EndPlayReason);
}

void UPPImpactSensorComponent::RegisterCourseObstacleActor(AActor* CourseObstacleActor)
{
	if (!CourseObstacleActor || !IsPPImpactCourseObstacle(CourseObstacleActor) || !GetWorld())
	{
		return;
	}

	const double CurrentTimeSeconds = GetWorld()->GetTimeSeconds();
	TInlineComponentArray<UPrimitiveComponent*> PrimitiveComponents;
	CourseObstacleActor->GetComponents(PrimitiveComponents);
	for (UPrimitiveComponent* PrimitiveComponent : PrimitiveComponents)
	{
		if (!PrimitiveComponent)
		{
			continue;
		}

		PrimitiveComponent->SetNotifyRigidBodyCollision(true);
		if (ResolvePPAuthoredCourseObstacleSpeed(CourseObstacleActor, PrimitiveComponent) >= 0.0f)
		{
			const TWeakObjectPtr<UPrimitiveComponent> PrimitiveComponentKey(PrimitiveComponent);
			FPPImpactSensorObstacleMotionSample& MotionSample = CourseObstacleMotionSamples.FindOrAdd(PrimitiveComponentKey);
			if (!MotionSample.bInitialized)
			{
				MotionSample.LastTransform = PrimitiveComponent->GetComponentTransform();
				MotionSample.LastSampleTimeSeconds = CurrentTimeSeconds;
				MotionSample.bInitialized = true;
			}
		}
		if (IsPPImpactSensorPusherContactComponent(CourseObstacleActor, PrimitiveComponent))
		{
			PusherContactComponents.AddUnique(PrimitiveComponent);
		}
	}

	if (!CourseObstacleMotionSamples.IsEmpty())
	{
		SetComponentTickEnabled(true);
	}
}

void UPPImpactSensorComponent::HandleCourseObstacleSpawned(AActor* SpawnedActor)
{
	RegisterCourseObstacleActor(SpawnedActor);
}

void UPPImpactSensorComponent::UpdateCourseObstacleMotionSamples()
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	const double CurrentTimeSeconds = World->GetTimeSeconds();
	for (auto MotionIterator = CourseObstacleMotionSamples.CreateIterator(); MotionIterator; ++MotionIterator)
	{
		UPrimitiveComponent* ObstacleComponent = MotionIterator.Key().Get();
		if (!ObstacleComponent)
		{
			MotionIterator.RemoveCurrent();
			continue;
		}

		FPPImpactSensorObstacleMotionSample& MotionSample = MotionIterator.Value();
		const FTransform CurrentTransform = ObstacleComponent->GetComponentTransform();
		const double SampleIntervalSeconds = CurrentTimeSeconds - MotionSample.LastSampleTimeSeconds;
		if (MotionSample.bInitialized && SampleIntervalSeconds > UE_KINDA_SMALL_NUMBER)
		{
			const float InverseSampleInterval = 1.0f / static_cast<float>(SampleIntervalSeconds);
			MotionSample.LinearVelocity =
				(CurrentTransform.GetLocation() - MotionSample.LastTransform.GetLocation()) * InverseSampleInterval;

			FQuat DeltaRotation = CurrentTransform.GetRotation() * MotionSample.LastTransform.GetRotation().Inverse();
			DeltaRotation.Normalize();
			FVector RotationAxis = FVector::ZeroVector;
			float RotationAngle = 0.0f;
			DeltaRotation.ToAxisAndAngle(RotationAxis, RotationAngle);
			RotationAngle = FMath::UnwindRadians(RotationAngle);
			MotionSample.AngularVelocityRadians =
				FMath::Abs(RotationAngle) > UE_KINDA_SMALL_NUMBER
				? RotationAxis.GetSafeNormal() * (RotationAngle * InverseSampleInterval)
				: FVector::ZeroVector;
		}
		else
		{
			MotionSample.LinearVelocity = FVector::ZeroVector;
			MotionSample.AngularVelocityRadians = FVector::ZeroVector;
			MotionSample.bInitialized = true;
		}

		MotionSample.LastTransform = CurrentTransform;
		MotionSample.LastSampleTimeSeconds = CurrentTimeSeconds;
	}
}

bool UPPImpactSensorComponent::IsCourseObstacleMovingIntoOwner(
	UPrimitiveComponent* ObstacleComponent,
	const FVector& ImpactPoint,
	const FVector& FallbackImpactDirection) const
{
	const AActor* OwnerActor = GetOwner();
	const FPPImpactSensorObstacleMotionSample* MotionSample = nullptr;
	if (ObstacleComponent)
	{
		const TWeakObjectPtr<UPrimitiveComponent> ObstacleComponentKey(ObstacleComponent);
		MotionSample = CourseObstacleMotionSamples.Find(ObstacleComponentKey);
	}
	if (!OwnerActor || !ObstacleComponent || !MotionSample || !MotionSample->bInitialized)
	{
		return false;
	}

	FVector AwayFromContact = (OwnerActor->GetActorLocation() - ImpactPoint).GetSafeNormal();
	if (AwayFromContact.IsNearlyZero())
	{
		AwayFromContact = FallbackImpactDirection.GetSafeNormal();
	}
	if (AwayFromContact.IsNearlyZero())
	{
		return false;
	}

	const double CurrentTimeSeconds = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.0;
	const double SampleAgeSeconds = CurrentTimeSeconds - MotionSample->LastSampleTimeSeconds;
	if (SampleAgeSeconds < 0.0 || SampleAgeSeconds > PPImpactMaximumMotionSampleAgeSeconds)
	{
		return false;
	}

	FVector SampledLinearVelocity = MotionSample->LinearVelocity;
	FVector SampledAngularVelocityRadians = MotionSample->AngularVelocityRadians;
	if (SampleAgeSeconds > UE_KINDA_SMALL_NUMBER)
	{
		const FTransform CurrentTransform = ObstacleComponent->GetComponentTransform();
		const float InverseSampleAge = 1.0f / static_cast<float>(SampleAgeSeconds);
		SampledLinearVelocity =
			(CurrentTransform.GetLocation() - MotionSample->LastTransform.GetLocation()) * InverseSampleAge;
		FQuat DeltaRotation = CurrentTransform.GetRotation() * MotionSample->LastTransform.GetRotation().Inverse();
		DeltaRotation.Normalize();
		FVector RotationAxis = FVector::ZeroVector;
		float RotationAngle = 0.0f;
		DeltaRotation.ToAxisAndAngle(RotationAxis, RotationAngle);
		RotationAngle = FMath::UnwindRadians(RotationAngle);
		SampledAngularVelocityRadians = FMath::Abs(RotationAngle) > UE_KINDA_SMALL_NUMBER
			? RotationAxis.GetSafeNormal() * (RotationAngle * InverseSampleAge)
			: FVector::ZeroVector;
	}

	const FVector ContactOffset = ImpactPoint - ObstacleComponent->GetComponentLocation();
	const FVector ObstaclePointVelocity = SampledLinearVelocity
		+ FVector::CrossProduct(SampledAngularVelocityRadians, ContactOffset);
	const float ObstacleClosingSpeed = FVector::DotProduct(ObstaclePointVelocity, AwayFromContact);
	return ObstacleClosingSpeed >= MinimumCourseObstacleClosingSpeed;
}

void UPPImpactSensorComponent::TickComponent(
	float DeltaTime,
	ELevelTick TickType,
	FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
	UpdateCourseObstacleMotionSamples();

	ACharacter* OwnerCharacter = Cast<ACharacter>(GetOwner());
	UPPPhysicalStateComponent* PhysicalState = UPPPhysicalStateComponent::FindPhysicalStateComponent(OwnerCharacter);
	UCapsuleComponent* OwnerCapsule = OwnerCharacter ? OwnerCharacter->GetCapsuleComponent() : nullptr;
	if (!OwnerCharacter || !OwnerCharacter->HasAuthority() || !OwnerCapsule || !PhysicalState
		|| OwnerCapsule->GetCollisionEnabled() == ECollisionEnabled::NoCollision
		|| PhysicalState->IsRagdolled())
	{
		return;
	}

	const FCollisionShape OwnerCollisionShape = OwnerCapsule->GetCollisionShape(-2.0f);
	for (const TWeakObjectPtr<UPrimitiveComponent>& WeakPusherComponent : PusherContactComponents)
	{
		UPrimitiveComponent* PusherComponent = WeakPusherComponent.Get();
		AActor* PusherActor = PusherComponent ? PusherComponent->GetOwner() : nullptr;
		if (!PusherComponent || !PusherActor || !CanReportImpact(PusherActor))
		{
			continue;
		}
		if (!PusherComponent->OverlapComponent(
			OwnerCapsule->GetComponentLocation(),
			OwnerCapsule->GetComponentQuat(),
			OwnerCollisionShape))
		{
			continue;
		}

		const FVector OwnerLocation = OwnerCapsule->GetComponentLocation();
		FVector PusherImpactPoint = PusherComponent->GetComponentLocation();
		PusherComponent->GetClosestPointOnCollision(OwnerLocation, PusherImpactPoint);
		FVector PusherImpactDirection = (OwnerLocation - PusherImpactPoint).GetSafeNormal2D();
		if (PusherImpactDirection.IsNearlyZero())
		{
			PusherImpactDirection = (OwnerLocation - PusherComponent->GetComponentLocation()).GetSafeNormal2D();
		}
		if (!IsCourseObstacleMovingIntoOwner(PusherComponent, PusherImpactPoint, PusherImpactDirection))
		{
			continue;
		}
		ReportResolvedImpact(
			30.0f,
			PusherActor,
			NAME_None,
			true,
			PusherImpactPoint,
			PusherImpactDirection,
			PusherComponent,
			ResolvePPAuthoredCourseObstacleSpeed(PusherActor, PusherComponent));
		RememberImpact(PusherActor);
		break;
	}
}

void UPPImpactSensorComponent::ReportImpact(
	float Severity,
	AActor* InstigatorActor,
	FName BodyRegion,
	bool bHeavyObstacle)
{
	AActor* OwnerActor = GetOwner();
	ReportResolvedImpact(
		Severity,
		InstigatorActor,
		BodyRegion,
		bHeavyObstacle,
		OwnerActor ? OwnerActor->GetActorLocation() : FVector::ZeroVector,
		FVector::UpVector,
		nullptr,
		-1.0f);
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
	const bool bSpinnerImpact = IsPPImpactSensorSpinnerStyleCourseObstacle(OtherActor);
	const bool bPusherActor = IsPPImpactSensorPusherObstacle(OtherActor);
	const bool bPusherImpact = IsPPImpactSensorPusherContactComponent(OtherActor, OtherComponent);
	const bool bDropPropellerImpact = IsPPDropPropellerImpact(OtherActor, OtherComponent);
	if (OtherActor && OtherActor->GetName().Contains(TEXT("Drop_V1"), ESearchCase::IgnoreCase) && !bDropPropellerImpact)
	{
		return;
	}
	if (bPusherActor && !bPusherImpact)
	{
		return;
	}
	const float CourseObstacleSpeed = ResolvePPAuthoredCourseObstacleSpeed(OtherActor, OtherComponent);
	if (CourseObstacleSpeed >= 0.0f)
	{
		RegisterCourseObstacleActor(OtherActor);
		const FVector CourseImpactDirection = NormalImpulse.IsNearlyZero()
			? -Hit.ImpactNormal
			: NormalImpulse.GetSafeNormal();
		if (!IsCourseObstacleMovingIntoOwner(OtherComponent, Hit.ImpactPoint, CourseImpactDirection))
		{
			return;
		}
		// Timeline movement often reports little or no rigid-body impulse. A valid
		// moving course contact must still enter its bounded throw profile.
		Severity = FMath::Max(Severity, 30.0f);
	}
	if (Severity < MinimumReportedSeverity)
	{
		return;
	}

	ReportResolvedImpact(
		Severity,
		OtherActor,
		Hit.BoneName,
		bSpinnerImpact || bPusherImpact || bDropPropellerImpact
			|| (OtherActor && OtherActor->ActorHasTag(HeavyObstacleTag)),
		Hit.ImpactPoint,
		NormalImpulse.IsNearlyZero() ? -Hit.ImpactNormal : NormalImpulse.GetSafeNormal(),
		OtherComponent,
		CourseObstacleSpeed);
	RememberImpact(OtherActor);
}

void UPPImpactSensorComponent::ReportResolvedImpact(
	float Severity,
	AActor* InstigatorActor,
	FName BodyRegion,
	bool bHeavyObstacle,
	const FVector& ImpactPoint,
	const FVector& ImpactDirection,
	UPrimitiveComponent* ImpactSourceComponent,
	float CourseObstacleSpeed)
{
	AActor* OwnerActor = GetOwner();
	UPPPhysicalStateComponent* PhysicalState = UPPPhysicalStateComponent::FindPhysicalStateComponent(OwnerActor);
	if (!OwnerActor || !OwnerActor->HasAuthority() || !PhysicalState)
	{
		return;
	}
	if (CourseObstacleSpeed >= 0.0f)
	{
		const FVector AwayFromContact = (OwnerActor->GetActorLocation() - ImpactPoint).GetSafeNormal2D();
		// A raw solver impulse can run tangentially along a rotating surface and
		// make an otherwise bounded throw orbit the obstacle. Prefer the geometric
		// separation direction so every course hit cleanly exits its contact.
		const FVector CourseImpactDirection = !AwayFromContact.IsNearlyZero()
			? AwayFromContact
			: ImpactDirection.GetSafeNormal2D();
		PhysicalState->RequestCourseObstacleRagdoll(
			CourseImpactDirection,
			CourseObstacleSpeed,
			InstigatorActor,
			ImpactSourceComponent,
			ImpactPoint);
		return;
	}

	FPPImpactData ImpactData;
	ImpactData.Severity = FMath::Clamp(Severity, 0.0f, 150.0f);
	ImpactData.ImpactPoint = ImpactPoint;
	ImpactData.ImpactDirection = ImpactDirection.GetSafeNormal();
	ImpactData.BodyRegion = BodyRegion;
	ImpactData.InstigatorActor = InstigatorActor;
	ImpactData.bHeavyObstacle = bHeavyObstacle;
	PhysicalState->ReceiveImpactData_Implementation(ImpactData);
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
