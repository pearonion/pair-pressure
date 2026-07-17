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

	UFUNCTION(BlueprintCallable, Category = "VNH|Alien Locomotion|Camera")
	void SetCameraOrbitActive(bool bNewCameraOrbitActive);

	UFUNCTION(BlueprintPure, Category = "VNH|Alien Locomotion|Camera")
	bool IsCameraOrbitDetached() const { return bCameraOrbitActive; }

	UFUNCTION(BlueprintCallable, Category = "VNH|Alien Locomotion")
	void SetGrabMovementMultiplier(float NewGrabMovementMultiplier);

	UFUNCTION(BlueprintCallable, Category = "VNH|Alien Locomotion")
	void SetGrabTurnMultiplier(float NewGrabTurnMultiplier);

	UFUNCTION(BlueprintCallable, Category = "VNH|Alien Locomotion|Airborne")
	void SetTetherTensionNormalized(float NewTetherTensionNormalized);

	UFUNCTION(BlueprintPure, Category = "VNH|Alien Locomotion")
	FVNHAlienLocomotionState GetLocomotionState() const { return LocomotionState; }

	UFUNCTION(BlueprintPure, Category = "VNH|Alien Locomotion")
	FVector GetCameraRelativeMoveDirection() const;

	UFUNCTION(BlueprintPure, Category = "VNH|Alien Locomotion")
	FString DescribeLocomotionState() const;

protected:
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "VNH|Alien Locomotion|Tuning")
	TObjectPtr<UVNHMovementTuningData> TuningData;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "VNH|Alien Locomotion|Tuning", meta = (ClampMin = "0.0"))
	float DefaultWalkSpeed = 500.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "VNH|Alien Locomotion|Tuning", meta = (ClampMin = "0.0"))
	float DefaultMinWalkSpeed = 0.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "VNH|Alien Locomotion|Tuning", meta = (ClampMin = "0.0"))
	float DefaultFastWalkSpeed = 500.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "VNH|Alien Locomotion|Tuning", meta = (ClampMin = "0.0"))
	float DefaultAcceleration = 850.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "VNH|Alien Locomotion|Tuning", meta = (ClampMin = "0.0"))
	float DefaultCoastBraking = 520.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "VNH|Alien Locomotion|Tuning", meta = (ClampMin = "1.0"))
	float DefaultManualBrakeMultiplier = 1.65f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "VNH|Alien Locomotion|Tuning", meta = (ClampMin = "0.0"))
	float DefaultBodyTurnRateDegrees = 360.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "VNH|Alien Locomotion|Tuning", meta = (ClampMin = "0.0", ClampMax = "180.0"))
	float DefaultCorrectionStepAngleDegrees = 120.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "VNH|Alien Locomotion|Airborne", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float DefaultAirControl = 0.24f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "VNH|Alien Locomotion|Airborne", meta = (ClampMin = "0.0"))
	float AirborneAcceleration = 320.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "VNH|Alien Locomotion|Airborne", meta = (ClampMin = "0.0"))
	float AirborneSteeringDegreesPerSecond = 78.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "VNH|Alien Locomotion|Airborne", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float ExternalLaunchAirControlMultiplier = 0.35f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "VNH|Alien Locomotion|Airborne", meta = (ClampMin = "0.0"))
	float ExternalLaunchVelocityDeltaThreshold = 275.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "VNH|Alien Locomotion|Airborne", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float HeavyCarryAirControlMultiplier = 0.45f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "VNH|Alien Locomotion|Airborne", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float FullTetherTensionAirControlMultiplier = 0.40f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "VNH|Alien Locomotion|Instability", meta = (ClampMin = "0.0"))
	float StabilityBuildRate = 1.25f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "VNH|Alien Locomotion|Instability", meta = (ClampMin = "0.0"))
	float StabilityDecayRate = 1.6f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "VNH|Alien Locomotion|Instability", meta = (ClampMin = "0.0"))
	float AbruptTurnInstability = 0.12f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "VNH|Alien Locomotion|Instability", meta = (ClampMin = "0.0"))
	float FastWalkInstability = 0.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "VNH|Alien Locomotion|Instability", meta = (ClampMin = "0.0"))
	float WobbleFrequencyHz = 1.85f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "VNH|Alien Locomotion|Instability", meta = (ClampMin = "0.0"))
	float WobbleYawDegrees = 3.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "VNH|Alien Locomotion|Instability", meta = (ClampMin = "0.0"))
	float LateralDriftStrength = 0.04f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "VNH|Alien Locomotion|Instability", meta = (ClampMin = "0.0"))
	float UnevenStrideStrength = 0.03f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "VNH|Alien Locomotion|Instability", meta = (ClampMin = "0.1"))
	float MinAlienAnimRate = 0.96f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "VNH|Alien Locomotion|Instability", meta = (ClampMin = "0.1"))
	float MaxAlienAnimRate = 1.04f;

private:
	UPROPERTY(BlueprintReadOnly, Category = "VNH|Alien Locomotion", meta = (AllowPrivateAccess = "true"))
	FVNHAlienLocomotionState LocomotionState;

	FVector2D MoveInput = FVector2D::ZeroVector;
	FVector LastDesiredDirection = FVector::ZeroVector;
	float WobblePhaseRadians = 0.0f;
	bool bCameraOrbitActive = false;
	float GrabMovementMultiplier = 1.0f;
	float GrabTurnMultiplier = 1.0f;
	float TetherTensionNormalized = 0.0f;
	float AirborneHorizontalSpeedCap = 0.0f;
	float PreviousAirborneHorizontalSpeed = 0.0f;
	bool bWasFalling = false;
	bool bExternalLaunchDetected = false;

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
	void ApplyMovement(float DeltaTime, const FVector& DesiredDirection);
	void UpdateBodyFacing(float DeltaTime, const FVector& DesiredDirection);
	float GetAirborneControlMultiplier() const;
	bool IsControlledAirSteeringAllowed() const;
	void UpdateTetherTensionFromWorld();
};
