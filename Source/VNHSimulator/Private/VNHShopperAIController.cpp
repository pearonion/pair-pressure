#include "VNHShopperAIController.h"

#include "GameFramework/CharacterMovementComponent.h"
#include "Navigation/PathFollowingComponent.h"
#include "TimerManager.h"
#include "VNHLog.h"
#include "VNHShopperCharacter.h"
#include "VNHShopperWaypoint.h"

AVNHShopperAIController::AVNHShopperAIController()
{
	PrimaryActorTick.bCanEverTick = true;
}

void AVNHShopperAIController::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	if (bUsingFallbackRoutineMovement)
	{
		TickFallbackRoutineMovement(DeltaSeconds);
	}
}

void AVNHShopperAIController::OnPossess(APawn* InPawn)
{
	Super::OnPossess(InPawn);
	StartRoutineMove();
}

void AVNHShopperAIController::OnUnPossess()
{
	GetWorldTimerManager().ClearTimer(RoutineWaitTimerHandle);
	bUsingFallbackRoutineMovement = false;
	Super::OnUnPossess();
}

void AVNHShopperAIController::OnMoveCompleted(FAIRequestID RequestID, const FPathFollowingResult& Result)
{
	Super::OnMoveCompleted(RequestID, Result);
	if (Result.IsSuccess())
	{
		WaitThenAdvanceRoutine();
	}
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
		UE_LOG(LogVNH, Warning, TEXT("ShopperRoutine: %s has no current waypoint."), *GetNameSafe(Shopper));
		return;
	}

	bUsingFallbackRoutineMovement = false;
	const EPathFollowingRequestResult::Type MoveResult = MoveToActor(Waypoint, 75.0f);
	if (MoveResult == EPathFollowingRequestResult::AlreadyAtGoal)
	{
		WaitThenAdvanceRoutine();
		return;
	}

	if (MoveResult == EPathFollowingRequestResult::Failed)
	{
		bUsingFallbackRoutineMovement = true;
		UE_LOG(LogVNH, Warning, TEXT("ShopperRoutine: %s failed to move to %s; using direct fallback movement."), *GetNameSafe(Shopper), *GetNameSafe(Waypoint));
	}
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

void AVNHShopperAIController::TickFallbackRoutineMovement(float DeltaSeconds)
{
	AVNHShopperCharacter* Shopper = GetShopper();
	if (!Shopper || Shopper->IsPossessedByAlien() || Shopper->IsFrozenByPublicTest() || !Shopper->GetRoutineComponent() || Shopper->GetRoutineComponent()->IsRoutinePaused())
	{
		return;
	}

	AVNHShopperWaypoint* Waypoint = Shopper->GetRoutineComponent()->GetCurrentWaypoint();
	if (!Waypoint)
	{
		bUsingFallbackRoutineMovement = false;
		return;
	}

	const FVector CurrentLocation = Shopper->GetActorLocation();
	FVector TargetLocation = Waypoint->GetActorLocation();
	TargetLocation.Z = CurrentLocation.Z;

	const FVector ToTarget = TargetLocation - CurrentLocation;
	if (ToTarget.Size2D() <= FallbackAcceptanceRadius)
	{
		bUsingFallbackRoutineMovement = false;
		WaitThenAdvanceRoutine();
		return;
	}

	if (UCharacterMovementComponent* MovementComponent = Shopper->GetCharacterMovement())
	{
		if (MovementComponent->MovementMode == MOVE_None)
		{
			return;
		}

		MovementComponent->MaxWalkSpeed = FallbackWalkSpeed;
	}

	const FVector MoveDirection = ToTarget.GetSafeNormal2D();
	Shopper->AddMovementInput(MoveDirection, 1.0f);

	if (!MoveDirection.IsNearlyZero())
	{
		const FRotator TargetRotation = MoveDirection.Rotation();
		const FRotator NewRotation = FMath::RInterpTo(Shopper->GetActorRotation(), FRotator(0.0f, TargetRotation.Yaw, 0.0f), DeltaSeconds, 6.0f);
		Shopper->SetActorRotation(NewRotation);
	}
}
