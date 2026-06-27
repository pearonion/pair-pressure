#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "VNHMovementTuningData.generated.h"

UCLASS(BlueprintType)
class VNHSIMULATOR_API UVNHMovementTuningData : public UDataAsset
{
	GENERATED_BODY()

public:
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "VNH|Alien Locomotion", meta = (ClampMin = "0.0"))
	float WalkSpeed = 330.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "VNH|Alien Locomotion", meta = (ClampMin = "0.0"))
	float MinWalkSpeed = 120.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "VNH|Alien Locomotion", meta = (ClampMin = "0.0"))
	float FastWalkSpeed = 460.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "VNH|Alien Locomotion", meta = (ClampMin = "0.0"))
	float Acceleration = 800.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "VNH|Alien Locomotion", meta = (ClampMin = "0.0"))
	float CoastBraking = 450.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "VNH|Alien Locomotion", meta = (ClampMin = "1.0"))
	float ManualBrakeMultiplier = 2.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "VNH|Alien Locomotion", meta = (ClampMin = "0.0"))
	float BodyTurnRateDegrees = 150.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "VNH|Alien Locomotion", meta = (ClampMin = "0.0", ClampMax = "180.0"))
	float CorrectionStepAngleDegrees = 120.0f;
};
