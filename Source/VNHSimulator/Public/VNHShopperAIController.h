#pragma once

#include "CoreMinimal.h"
#include "AIController.h"
#include "VNHShopperAIController.generated.h"

class AVNHShopperCharacter;
class AVNHShopperWaypoint;
struct FAIRequestID;
struct FPathFollowingResult;

UCLASS()
class VNHSIMULATOR_API AVNHShopperAIController : public AAIController
{
	GENERATED_BODY()

public:
	AVNHShopperAIController();

	virtual void Tick(float DeltaSeconds) override;
	virtual void OnPossess(APawn* InPawn) override;
	virtual void OnUnPossess() override;

	UFUNCTION(BlueprintCallable, Category = "VNH|Routine")
	void StartRoutineMove();

protected:
	virtual void OnMoveCompleted(FAIRequestID RequestID, const FPathFollowingResult& Result) override;

private:
	FTimerHandle RoutineWaitTimerHandle;
	bool bUsingFallbackRoutineMovement = false;
	float FallbackAcceptanceRadius = 75.0f;
	float FallbackWalkSpeed = 125.0f;

	AVNHShopperCharacter* GetShopper() const;
	void WaitThenAdvanceRoutine();
	void TickFallbackRoutineMovement(float DeltaSeconds);
};
