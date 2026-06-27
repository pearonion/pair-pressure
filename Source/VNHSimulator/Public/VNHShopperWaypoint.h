#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "VNHRoutineComponent.h"
#include "VNHShopperWaypoint.generated.h"

UCLASS()
class VNHSIMULATOR_API AVNHShopperWaypoint : public AActor
{
	GENERATED_BODY()

public:
	AVNHShopperWaypoint();

	UFUNCTION(BlueprintPure, Category = "VNH|Routine")
	EVNHShopperContext GetContext() const { return Context; }

	UFUNCTION(BlueprintPure, Category = "VNH|Routine")
	FName GetSuggestedNextActivity() const { return SuggestedNextActivity; }

	UFUNCTION(BlueprintPure, Category = "VNH|Routine")
	FName GetHeldProp() const { return HeldProp; }

	UFUNCTION(BlueprintPure, Category = "VNH|Routine")
	float GetWaitSeconds() const { return WaitSeconds; }

private:
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "VNH|Routine", meta = (AllowPrivateAccess = "true"))
	TObjectPtr<USceneComponent> SceneRoot;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "VNH|Routine", meta = (AllowPrivateAccess = "true"))
	EVNHShopperContext Context = EVNHShopperContext::Browsing;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "VNH|Routine", meta = (AllowPrivateAccess = "true"))
	FName SuggestedNextActivity = TEXT("Mirror");

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "VNH|Routine", meta = (AllowPrivateAccess = "true"))
	FName HeldProp = TEXT("Jacket");

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "VNH|Routine", meta = (AllowPrivateAccess = "true", ClampMin = "0.0"))
	float WaitSeconds = 2.0f;
};
