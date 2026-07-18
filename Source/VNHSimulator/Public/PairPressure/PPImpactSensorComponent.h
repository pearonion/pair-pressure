#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "PPImpactSensorComponent.generated.h"

class UPrimitiveComponent;

struct FPPImpactSensorObstacleMotionSample
{
	FTransform LastTransform = FTransform::Identity;
	FVector LinearVelocity = FVector::ZeroVector;
	FVector AngularVelocityRadians = FVector::ZeroVector;
	double LastSampleTimeSeconds = 0.0;
	bool bInitialized = false;
};

UCLASS(ClassGroup = (PairPressure), BlueprintType, Blueprintable, meta = (BlueprintSpawnableComponent))
class VNHSIMULATOR_API UPPImpactSensorComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UPPImpactSensorComponent();

	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;

	UFUNCTION(BlueprintCallable, BlueprintAuthorityOnly, Category = "Pair Pressure|Impact")
	void ReportImpact(float Severity, AActor* InstigatorActor, FName BodyRegion = NAME_None, bool bHeavyObstacle = false);

private:
	UFUNCTION()
	void HandleComponentHit(
		UPrimitiveComponent* HitComponent,
		AActor* OtherActor,
		UPrimitiveComponent* OtherComponent,
		FVector NormalImpulse,
		const FHitResult& Hit);

	float CalculateImpactSeverity(
		const UPrimitiveComponent* HitComponent,
		const UPrimitiveComponent* OtherComponent,
		const FVector& NormalImpulse,
		const FHitResult& Hit) const;
	void ReportResolvedImpact(
		float Severity,
		AActor* InstigatorActor,
		FName BodyRegion,
		bool bHeavyObstacle,
		const FVector& ImpactPoint,
		const FVector& ImpactDirection,
		float CourseObstacleSpeed);
	void RegisterCourseObstacleActor(AActor* CourseObstacleActor);
	void HandleCourseObstacleSpawned(AActor* SpawnedActor);
	void UpdateCourseObstacleMotionSamples();
	bool IsCourseObstacleMovingIntoOwner(
		UPrimitiveComponent* ObstacleComponent,
		const FVector& ImpactPoint,
		const FVector& FallbackImpactDirection) const;

	bool CanReportImpact(AActor* OtherActor) const;
	void RememberImpact(AActor* OtherActor);

	UPROPERTY(EditDefaultsOnly, Category = "Pair Pressure|Impact", meta = (ClampMin = "0.0"))
	float MinimumReportedSeverity = 8.0f;

	UPROPERTY(EditDefaultsOnly, Category = "Pair Pressure|Impact", meta = (ClampMin = "1.0"))
	float HeavyRelativeSpeed = 1800.0f;

	UPROPERTY(EditDefaultsOnly, Category = "Pair Pressure|Impact", meta = (ClampMin = "1.0"))
	float HeavyNormalImpulse = 70000.0f;

	UPROPERTY(EditDefaultsOnly, Category = "Pair Pressure|Impact", meta = (ClampMin = "0.0"))
	float SameActorCooldownSeconds = 0.25f;

	UPROPERTY(EditDefaultsOnly, Category = "Pair Pressure|Impact", meta = (ClampMin = "0.0"))
	float MinimumCourseObstacleClosingSpeed = 35.0f;

	UPROPERTY(EditDefaultsOnly, Category = "Pair Pressure|Impact")
	FName HeavyObstacleTag = TEXT("PP_HeavyObstacle");

	TMap<TWeakObjectPtr<AActor>, double> LastImpactTimes;
	TArray<TWeakObjectPtr<UPrimitiveComponent>> PusherContactComponents;
	TMap<TWeakObjectPtr<UPrimitiveComponent>, FPPImpactSensorObstacleMotionSample> CourseObstacleMotionSamples;
	FDelegateHandle CourseObstacleSpawnedDelegateHandle;
};
