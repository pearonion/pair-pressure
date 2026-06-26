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
	virtual void OnPossess(APawn* InPawn) override;
	virtual void OnUnPossess() override;

protected:
	virtual void OnMoveCompleted(FAIRequestID RequestID, const FPathFollowingResult& Result) override;

private:
	FTimerHandle RoutineWaitTimerHandle;

	AVNHShopperCharacter* GetShopper() const;
	void StartRoutineMove();
	void WaitThenAdvanceRoutine();
};
