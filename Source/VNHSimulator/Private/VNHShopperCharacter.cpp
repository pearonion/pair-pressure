#include "VNHShopperCharacter.h"

#include "Camera/CameraComponent.h"
#include "Animation/AnimInstance.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMesh.h"
#include "Animation/AnimTypes.h"
#include "GameFramework/Controller.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/SpringArmComponent.h"
#include "TimerManager.h"
#include "UObject/ConstructorHelpers.h"
#include "VNHAlienLocomotionComponent.h"
#include "VNHShopperAIController.h"
#include "VNHShopperWaypoint.h"
#include "Net/UnrealNetwork.h"

AVNHShopperCharacter::AVNHShopperCharacter()
{
	bReplicates = true;
	AIControllerClass = AVNHShopperAIController::StaticClass();
	AutoPossessAI = EAutoPossessAI::PlacedInWorldOrSpawned;
	bUseControllerRotationYaw = false;

	if (UCharacterMovementComponent* MovementComponent = GetCharacterMovement())
	{
		MovementComponent->bOrientRotationToMovement = false;
		MovementComponent->RotationRate = FRotator(0.0f, 150.0f, 0.0f);
		MovementComponent->MaxWalkSpeed = 135.0f;
		MovementComponent->MaxAcceleration = 420.0f;
		MovementComponent->BrakingDecelerationWalking = 360.0f;
		MovementComponent->GroundFriction = 6.0f;
	}

	RoutineComponent = CreateDefaultSubobject<UVNHRoutineComponent>(TEXT("RoutineComponent"));
	AlienLocomotionComponent = CreateDefaultSubobject<UVNHAlienLocomotionComponent>(TEXT("AlienLocomotionComponent"));

	FollowCameraBoom = CreateDefaultSubobject<USpringArmComponent>(TEXT("FollowCameraBoom"));
	FollowCameraBoom->SetupAttachment(RootComponent);
	FollowCameraBoom->TargetArmLength = 420.0f;
	FollowCameraBoom->SetRelativeRotation(FRotator(-18.0f, 0.0f, 0.0f));
	FollowCameraBoom->bUsePawnControlRotation = true;
	FollowCameraBoom->bEnableCameraLag = true;
	FollowCameraBoom->CameraLagSpeed = 12.0f;

	FollowCamera = CreateDefaultSubobject<UCameraComponent>(TEXT("FollowCamera"));
	FollowCamera->SetupAttachment(FollowCameraBoom, USpringArmComponent::SocketName);
	FollowCamera->bUsePawnControlRotation = false;

	DebugBodyMesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("DebugBodyMesh"));
	DebugBodyMesh->SetupAttachment(RootComponent);
	DebugBodyMesh->SetRelativeLocation(FVector(0.0f, 0.0f, -35.0f));
	DebugBodyMesh->SetRelativeScale3D(FVector(0.55f, 0.55f, 1.55f));
	DebugBodyMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	DebugBodyMesh->SetHiddenInGame(false);

	static ConstructorHelpers::FObjectFinder<USkeletalMesh> MannyMeshFinder(TEXT("/Game/Characters/Mannequins/Meshes/SKM_Manny_Simple.SKM_Manny_Simple"));
	if (MannyMeshFinder.Succeeded())
	{
		GetMesh()->SetSkeletalMesh(MannyMeshFinder.Object);
		GetMesh()->SetRelativeLocation(FVector(0.0f, 0.0f, -88.0f));
		GetMesh()->SetRelativeRotation(FRotator(0.0f, -90.0f, 0.0f));
		GetMesh()->SetAnimationMode(EAnimationMode::AnimationBlueprint);
		GetMesh()->VisibilityBasedAnimTickOption = EVisibilityBasedAnimTickOption::AlwaysTickPoseAndRefreshBones;
		GetMesh()->bEnableUpdateRateOptimizations = false;
		DebugBodyMesh->SetHiddenInGame(true);

		static ConstructorHelpers::FClassFinder<UAnimInstance> MannyAnimFinder(TEXT("/Game/Characters/Mannequins/Anims/Unarmed/ABP_Unarmed"));
		if (MannyAnimFinder.Succeeded())
		{
			GetMesh()->SetAnimInstanceClass(MannyAnimFinder.Class);
		}
	}
	else
	{
		static ConstructorHelpers::FObjectFinder<UStaticMesh> DebugBodyMeshFinder(TEXT("/Engine/BasicShapes/Cylinder.Cylinder"));
		if (DebugBodyMeshFinder.Succeeded())
		{
			DebugBodyMesh->SetStaticMesh(DebugBodyMeshFinder.Object);
		}
	}
}

void AVNHShopperCharacter::BeginPlay()
{
	Super::BeginPlay();

	if (HasAuthority() && !bPossessedByAlien && RoutineComponent && RoutineComponent->GetCurrentWaypoint())
	{
		if (!GetController())
		{
			SpawnDefaultController();
		}

		FTimerHandle RoutineStartupTimerHandle;
		GetWorldTimerManager().SetTimer(RoutineStartupTimerHandle, this, &AVNHShopperCharacter::StartRoutineMovement, 2.0f, false);
	}
}

void AVNHShopperCharacter::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);

	DOREPLIFETIME(AVNHShopperCharacter, bPossessedByAlien);
	DOREPLIFETIME(AVNHShopperCharacter, bActNaturalAvailable);
	DOREPLIFETIME(AVNHShopperCharacter, bFrozenByPublicTest);
}

void AVNHShopperCharacter::SetPossessedByAlien(bool bNewPossessedByAlien)
{
	if (!HasAuthority() || bPossessedByAlien == bNewPossessedByAlien)
	{
		return;
	}

	bPossessedByAlien = bNewPossessedByAlien;
	bActNaturalAvailable = bNewPossessedByAlien;

	if (RoutineComponent)
	{
		if (bPossessedByAlien)
		{
			RoutineComponent->PauseRoutineForPossession();
		}
		else
		{
			RoutineComponent->ResumeRoutineAfterPossession();
		}
	}

	OnRep_PossessedByAlien();
}

bool AVNHShopperCharacter::UseActNatural()
{
	if (!HasAuthority() || !bPossessedByAlien || !bActNaturalAvailable || !RoutineComponent)
	{
		return false;
	}

	bActNaturalAvailable = false;
	OnActNaturalUsed.Broadcast(RoutineComponent->ChooseActNaturalRecovery());
	return true;
}

void AVNHShopperCharacter::ApplyPublicTest(EVNHPublicTestType TestType)
{
	if (!HasAuthority())
	{
		return;
	}

	if (RoutineComponent)
	{
		RoutineComponent->SetContext(EVNHShopperContext::Reacting, RoutineComponent->GetSnapshot().SuggestedNextActivity);
	}

	if (TestType == EVNHPublicTestType::Freeze && !bPossessedByAlien)
	{
		SetFrozenByPublicTest(true);
		GetWorldTimerManager().SetTimer(FreezeTestTimerHandle, this, &AVNHShopperCharacter::ClearPublicTestFreeze, FreezeTestHoldSeconds, false);
	}
	else if (TestType == EVNHPublicTestType::LookToEntrance && !bPossessedByAlien)
	{
		ApplyLookToEntranceReaction();
	}
	else if (TestType == EVNHPublicTestType::ClearAisle && !bPossessedByAlien)
	{
		ApplyClearAisleReaction();
	}

	OnPublicTestReceived.Broadcast(TestType);
}

void AVNHShopperCharacter::OnRep_PossessedByAlien()
{
}

void AVNHShopperCharacter::OnRep_FrozenByPublicTest()
{
	ApplyFrozenVisualState();
}

void AVNHShopperCharacter::SetFrozenByPublicTest(bool bNewFrozen)
{
	if (!HasAuthority() || bFrozenByPublicTest == bNewFrozen)
	{
		return;
	}

	bFrozenByPublicTest = bNewFrozen;
	ApplyFrozenVisualState();
}

void AVNHShopperCharacter::ClearPublicTestFreeze()
{
	SetFrozenByPublicTest(false);
	ResumeRoutineMovement();
}

void AVNHShopperCharacter::ApplyFrozenVisualState()
{
	if (USkeletalMeshComponent* MeshComponent = GetMesh())
	{
		MeshComponent->bPauseAnims = bFrozenByPublicTest;
		MeshComponent->GlobalAnimRateScale = bFrozenByPublicTest ? 0.0f : 1.0f;
	}

	if (UCharacterMovementComponent* MovementComponent = GetCharacterMovement())
	{
		if (bFrozenByPublicTest)
		{
			MovementComponent->StopMovementImmediately();
			MovementComponent->DisableMovement();
		}
		else
		{
			MovementComponent->SetMovementMode(MOVE_Walking);
		}
	}

	if (bFrozenByPublicTest)
	{
		if (AController* CurrentController = GetController())
		{
			CurrentController->StopMovement();
		}
	}
}

void AVNHShopperCharacter::ApplyLookToEntranceReaction()
{
	if (AController* CurrentController = GetController())
	{
		CurrentController->StopMovement();
	}

	const FVector EntranceLocation(0.0f, -760.0f, GetActorLocation().Z);
	const FVector ToEntrance = (EntranceLocation - GetActorLocation()).GetSafeNormal2D();
	if (!ToEntrance.IsNearlyZero())
	{
		SetActorRotation(FRotator(0.0f, ToEntrance.Rotation().Yaw, 0.0f));
	}

	GetWorldTimerManager().ClearTimer(PublicTestReactionTimerHandle);
	GetWorldTimerManager().SetTimer(PublicTestReactionTimerHandle, this, &AVNHShopperCharacter::ResumeRoutineMovement, 0.85f, false);
}

void AVNHShopperCharacter::ApplyClearAisleReaction()
{
	if (AController* CurrentController = GetController())
	{
		CurrentController->StopMovement();
	}

	if (UCharacterMovementComponent* MovementComponent = GetCharacterMovement())
	{
		MovementComponent->SetMovementMode(MOVE_Walking);
		MovementComponent->MaxWalkSpeed = 125.0f;
	}

	const float SideSign = GetActorLocation().Y >= 0.0f ? 1.0f : -1.0f;
	const FVector SideDirection(0.0f, SideSign, 0.0f);
	AddMovementInput(SideDirection, 1.0f);
	LaunchCharacter(SideDirection * 115.0f, false, false);
	SetActorRotation(FRotator(0.0f, SideDirection.Rotation().Yaw, 0.0f));

	GetWorldTimerManager().ClearTimer(PublicTestReactionTimerHandle);
	GetWorldTimerManager().SetTimer(PublicTestReactionTimerHandle, this, &AVNHShopperCharacter::ResumeRoutineMovement, 1.1f, false);
}

void AVNHShopperCharacter::ResumeRoutineMovement()
{
	if (!HasAuthority() || bPossessedByAlien || !RoutineComponent || RoutineComponent->IsRoutinePaused() || !RoutineComponent->GetCurrentWaypoint())
	{
		return;
	}

	AVNHShopperWaypoint* Waypoint = RoutineComponent->GetCurrentWaypoint();
	AVNHShopperAIController* ShopperAIController = Cast<AVNHShopperAIController>(GetController());
	if (Waypoint && ShopperAIController)
	{
		ShopperAIController->StartRoutineMove();
	}
}

void AVNHShopperCharacter::StartRoutineMovement()
{
	if (!HasAuthority() || bPossessedByAlien || !RoutineComponent || RoutineComponent->IsRoutinePaused())
	{
		return;
	}

	if (!GetController())
	{
		SpawnDefaultController();
	}

	if (AVNHShopperAIController* ShopperAIController = Cast<AVNHShopperAIController>(GetController()))
	{
		ShopperAIController->StartRoutineMove();
	}
}

FString AVNHShopperCharacter::BuildQuestionResponse() const
{
	if (!RoutineComponent)
	{
		return TEXT("I am just browsing.");
	}

	const FVNHShopperRoutineSnapshot Snapshot = RoutineComponent->GetSnapshot();
	const TCHAR* ContextText = TEXT("shopping");
	switch (Snapshot.Context)
	{
	case EVNHShopperContext::Browsing:
		ContextText = TEXT("looking at clothes");
		break;
	case EVNHShopperContext::Mirror:
		ContextText = TEXT("using the mirror");
		break;
	case EVNHShopperContext::Checkout:
		ContextText = TEXT("waiting to pay");
		break;
	case EVNHShopperContext::Walking:
		ContextText = TEXT("walking to another aisle");
		break;
	case EVNHShopperContext::Idle:
		ContextText = TEXT("taking a second");
		break;
	case EVNHShopperContext::Reacting:
		ContextText = TEXT("reacting to that announcement");
		break;
	default:
		break;
	}

	const FString HeldPropText = Snapshot.HeldProp.IsNone() ? TEXT("nothing") : Snapshot.HeldProp.ToString();
	const FString NextText = Snapshot.SuggestedNextActivity.IsNone() ? TEXT("keep shopping") : Snapshot.SuggestedNextActivity.ToString();
	return FString::Printf(TEXT("\"I'm %s. I have %s. I was about to %s.\""), ContextText, *HeldPropText, *NextText);
}
