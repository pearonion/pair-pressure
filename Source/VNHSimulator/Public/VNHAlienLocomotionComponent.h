#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "VNHAlienLocomotionComponent.generated.h"

class ACharacter;
class UCharacterMovementComponent;
class UVNHMovementTuningData;

USTRUCT(BlueprintType)
struct FVNHAlienLocomotionState
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "VNH|Alien Locomotion")
	FVector DesiredWorldDirection = FVector::ZeroVector;

	UPROPERTY(BlueprintReadOnly, Category = "VNH|Alien Locomotion")
	float DesiredSpeed = 0.0f;

	UPROPERTY(BlueprintReadOnly, Category = "VNH|Alien Locomotion")
	float CurrentSpeed = 0.0f;

	UPROPERTY(BlueprintReadOnly, Category = "VNH|Alien Locomotion")
	bool bFastWalkRequested = false;

	UPROPERTY(BlueprintReadOnly, Category = "VNH|Alien Locomotion")
	bool bManualBrake = false;

	UPROPERTY(BlueprintReadOnly, Category = "VNH|Alien Locomotion")
	bool bCorrectionStepRequested = false;

	UPROPERTY(BlueprintReadOnly, Category = "VNH|Alien Locomotion")
	bool bHasMoveInput = false;

	UPROPERTY(BlueprintReadOnly, Category = "VNH|Alien Locomotion")
	float BodyYawDeltaDegrees = 0.0f;

	UPROPERTY(BlueprintReadOnly, Category = "VNH|Alien Locomotion")
	float Stability = 0.0f;

	UPROPERTY(BlueprintReadOnly, Category = "VNH|Alien Locomotion")
	float Instability = 1.0f;

	UPROPERTY(BlueprintReadOnly, Category = "VNH|Alien Locomotion")
	float WobbleDegrees = 0.0f;
};

UCLASS(ClassGroup = (VNH), meta = (BlueprintSpawnableComponent))
class VNHSIMULATOR_API UVNHAlienLocomotionComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UVNHAlienLocomotionComponent();

	virtual void BeginPlay() override;
	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;

	UFUNCTION(BlueprintCallable, Category = "VNH|Alien Locomotion")
	void SetMoveInput(FVector2D NewMoveInput);

	UFUNCTION(BlueprintCallable, Category = "VNH|Alien Locomotion")
	void SetFastWalkRequested(bool bNewFastWalkRequested);

	UFUNCTION(BlueprintCallable, Category = "VNH|Alien Locomotion")
	void ClearInput();

	UFUNCTION(BlueprintPure, Category = "VNH|Alien Locomotion")
	FVNHAlienLocomotionState GetLocomotionState() const { return LocomotionState; }

	UFUNCTION(BlueprintPure, Category = "VNH|Alien Locomotion")
	FString DescribeLocomotionState() const;

protected:
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "VNH|Alien Locomotion|Tuning")
	TObjectPtr<UVNHMovementTuningData> TuningData;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "VNH|Alien Locomotion|Tuning", meta = (ClampMin = "0.0"))
	float DefaultWalkSpeed = 285.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "VNH|Alien Locomotion|Tuning", meta = (ClampMin = "0.0"))
	float DefaultMinWalkSpeed = 95.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "VNH|Alien Locomotion|Tuning", meta = (ClampMin = "0.0"))
	float DefaultFastWalkSpeed = 415.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "VNH|Alien Locomotion|Tuning", meta = (ClampMin = "0.0"))
	float DefaultAcceleration = 520.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "VNH|Alien Locomotion|Tuning", meta = (ClampMin = "0.0"))
	float DefaultCoastBraking = 320.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "VNH|Alien Locomotion|Tuning", meta = (ClampMin = "1.0"))
	float DefaultManualBrakeMultiplier = 2.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "VNH|Alien Locomotion|Tuning", meta = (ClampMin = "0.0"))
	float DefaultBodyTurnRateDegrees = 220.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "VNH|Alien Locomotion|Tuning", meta = (ClampMin = "0.0", ClampMax = "180.0"))
	float DefaultCorrectionStepAngleDegrees = 120.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "VNH|Alien Locomotion|Instability", meta = (ClampMin = "0.0"))
	float StabilityBuildRate = 0.65f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "VNH|Alien Locomotion|Instability", meta = (ClampMin = "0.0"))
	float StabilityDecayRate = 1.15f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "VNH|Alien Locomotion|Instability", meta = (ClampMin = "0.0"))
	float AbruptTurnInstability = 0.35f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "VNH|Alien Locomotion|Instability", meta = (ClampMin = "0.0"))
	float FastWalkInstability = 0.22f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "VNH|Alien Locomotion|Instability", meta = (ClampMin = "0.0"))
	float WobbleFrequencyHz = 1.85f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "VNH|Alien Locomotion|Instability", meta = (ClampMin = "0.0"))
	float WobbleYawDegrees = 14.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "VNH|Alien Locomotion|Instability", meta = (ClampMin = "0.0"))
	float LateralDriftStrength = 0.34f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "VNH|Alien Locomotion|Instability", meta = (ClampMin = "0.0"))
	float UnevenStrideStrength = 0.18f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "VNH|Alien Locomotion|Instability", meta = (ClampMin = "0.1"))
	float MinAlienAnimRate = 0.72f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "VNH|Alien Locomotion|Instability", meta = (ClampMin = "0.1"))
	float MaxAlienAnimRate = 1.18f;

private:
	UPROPERTY(BlueprintReadOnly, Category = "VNH|Alien Locomotion", meta = (AllowPrivateAccess = "true"))
	FVNHAlienLocomotionState LocomotionState;

	FVector2D MoveInput = FVector2D::ZeroVector;
	FVector LastDesiredDirection = FVector::ZeroVector;
	float WobblePhaseRadians = 0.0f;

	TWeakObjectPtr<ACharacter> OwnerCharacter;
	TWeakObjectPtr<UCharacterMovementComponent> MovementComponent;

	bool ShouldDriveLocomotion() const;
	FVector BuildCameraRelativeMoveDirection() const;
	void UpdateInstability(float DeltaTime, const FVector& DesiredDirection);
	void UpdateAnimationRate();
	float GetWalkSpeed() const;
	float GetMinWalkSpeed() const;
	float GetFastWalkSpeed() const;
	float GetAcceleration() const;
	float GetCoastBraking() const;
	float GetManualBrakeMultiplier() const;
	float GetBodyTurnRateDegrees() const;
	float GetCorrectionStepAngleDegrees() const;
	void UpdateSpeed(float DeltaTime, const FVector& DesiredDirection);
	void ApplyMovement(const FVector& DesiredDirection);
	void UpdateBodyFacing(float DeltaTime, const FVector& DesiredDirection);
};
