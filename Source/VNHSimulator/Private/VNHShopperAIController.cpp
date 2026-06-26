#include "VNHShopperAIController.h"

#include "Navigation/PathFollowingComponent.h"
#include "TimerManager.h"
#include "VNHShopperCharacter.h"
#include "VNHShopperWaypoint.h"

void AVNHShopperAIController::OnPossess(APawn* InPawn)
{
	Super::OnPossess(InPawn);
	StartRoutineMove();
}

void AVNHShopperAIController::OnUnPossess()
{
	GetWorldTimerManager().ClearTimer(RoutineWaitTimerHandle);
	Super::OnUnPossess();
}

void AVNHShopperAIController::OnMoveCompleted(FAIRequestID RequestID, const FPathFollowingResult& Result)
{
	Super::OnMoveCompleted(RequestID, Result);
	WaitThenAdvanceRoutine();
}

AVNHShopperCharacter* AVNHShopperAIController::GetShopper() const
{
	return Cast<AVNHShopperCharacter>(GetPawn());
}

void AVNHShopperAIController::StartRoutineMove()
{
	AVNHShopperCharacter* Shopper = GetShopper();
	if (!Shopper || !Shopper->GetRoutineComponent() || Shopper->GetRoutineComponent()->IsRoutinePaused())
	{
		return;
	}

	AVNHShopperWaypoint* Waypoint = Shopper->GetRoutineComponent()->GetCurrentWaypoint();
	if (!Waypoint)
	{
		return;
	}

	MoveToActor(Waypoint, 75.0f);
}

void AVNHShopperAIController::WaitThenAdvanceRoutine()
{
	AVNHShopperCharacter* Shopper = GetShopper();
	if (!Shopper || !Shopper->GetRoutineComponent() || Shopper->GetRoutineComponent()->IsRoutinePaused())
	{
		return;
	}

	AVNHShopperWaypoint* Waypoint = Shopper->GetRoutineComponent()->GetCurrentWaypoint();
	const float WaitSeconds = Waypoint ? Waypoint->GetWaitSeconds() : 1.0f;

	GetWorldTimerManager().SetTimer(
		RoutineWaitTimerHandle,
		[this]()
		{
			if (AVNHShopperCharacter* ShopperAfterWait = GetShopper())
			{
				if (UVNHRoutineComponent* Routine = ShopperAfterWait->GetRoutineComponent())
				{
					Routine->AdvanceToNextWaypoint();
				}
			}

			StartRoutineMove();
		},
		WaitSeconds,
		false);
}
