#include "VNHGameMode.h"

#include "Components/PointLightComponent.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/PlayerController.h"
#include "TimerManager.h"
#include "VNHDebugHUD.h"
#include "VNHGameState.h"
#include "VNHLog.h"
#include "VNHPlayerController.h"
#include "VNHPlayerState.h"
#include "VNHShopperCharacter.h"

namespace
{
const FName VNHDebugArenaTag(TEXT("VNHDebugArena"));

bool HasDebugArena(UWorld* World)
{
	if (!World)
	{
		return false;
	}

	for (TActorIterator<AActor> It(World); It; ++It)
	{
		if (It->ActorHasTag(VNHDebugArenaTag))
		{
			return true;
		}
	}

	return false;
}
}

AVNHGameMode::AVNHGameMode()
{
	GameStateClass = AVNHGameState::StaticClass();
	PlayerControllerClass = AVNHPlayerController::StaticClass();
	PlayerStateClass = AVNHPlayerState::StaticClass();
	HUDClass = AVNHDebugHUD::StaticClass();
}

void AVNHGameMode::BeginPlay()
{
	Super::BeginPlay();
	TryStartRound();
}

void AVNHGameMode::PostLogin(APlayerController* NewPlayer)
{
	Super::PostLogin(NewPlayer);
	TryStartRound();
}

void AVNHGameMode::Logout(AController* Exiting)
{
	Super::Logout(Exiting);

	if (AVNHGameState* VNHGameState = GetVNHGameState())
	{
		VNHGameState->SetRoundPhase(EVNHRoundPhase::WaitingForPlayers, 0.0f);
	}
}

void AVNHGameMode::TryStartRound()
{
	if (CountConnectedPlayers() < RequiredPlayers)
	{
		if (AVNHGameState* VNHGameState = GetVNHGameState())
		{
			VNHGameState->SetRoundPhase(EVNHRoundPhase::WaitingForPlayers, 0.0f);
		}
		return;
	}

	AssignRoles();

	if (AVNHGameState* VNHGameState = GetVNHGameState())
	{
		VNHGameState->SetRoundNumber(VNHGameState->GetRoundNumber() + 1);
		VNHGameState->SetTestsRemaining(2);
		VNHGameState->ClearRoundOutcome();
	}

	StartShopperRoutines();
	EnterPhase(EVNHRoundPhase::AssigningRoles, 3.0f);
}

void AVNHGameMode::AdvanceRoundPhase()
{
	AVNHGameState* VNHGameState = GetVNHGameState();
	if (!VNHGameState)
	{
		return;
	}

	switch (VNHGameState->GetRoundPhase())
	{
	case EVNHRoundPhase::AssigningRoles:
		PossessAlienShopper();
		EnterPhase(EVNHRoundPhase::AlienSetup, PhaseTiming.AlienSetupSeconds);
		break;
	case EVNHRoundPhase::AlienSetup:
		EnterPhase(EVNHRoundPhase::AlienHeadStart, PhaseTiming.AlienHeadStartSeconds);
		break;
	case EVNHRoundPhase::AlienHeadStart:
		EnterPhase(EVNHRoundPhase::Investigation, PhaseTiming.InvestigationSeconds);
		break;
	case EVNHRoundPhase::Investigation:
		EnterPhase(EVNHRoundPhase::Accusation, PhaseTiming.AccusationSeconds);
		break;
	case EVNHRoundPhase::Accusation:
		EnterPhase(EVNHRoundPhase::Reveal, PhaseTiming.RevealSeconds);
		break;
	case EVNHRoundPhase::Reveal:
		EnterPhase(EVNHRoundPhase::Resetting, 3.0f);
		break;
	case EVNHRoundPhase::Resetting:
		TryStartRound();
		break;
	default:
		break;
	}
}

void AVNHGameMode::RequestPublicTest(AVNHPlayerController* RequestingPlayer, EVNHPublicTestType TestType)
{
	AVNHGameState* VNHGameState = GetVNHGameState();
	if (!VNHGameState || !IsHunterController(RequestingPlayer))
	{
		return;
	}

	if (VNHGameState->GetRoundPhase() != EVNHRoundPhase::Investigation || VNHGameState->GetTestsRemaining() <= 0)
	{
		return;
	}

	VNHGameState->SetTestsRemaining(VNHGameState->GetTestsRemaining() - 1);
	VNHGameState->SetActivePublicTest(TestType);
	ApplyPublicTestToShoppers(TestType);
}

void AVNHGameMode::RequestAccusation(AVNHPlayerController* RequestingPlayer, AVNHShopperCharacter* AccusedShopper)
{
	AVNHGameState* VNHGameState = GetVNHGameState();
	if (!VNHGameState || !IsHunterController(RequestingPlayer) || !AccusedShopper)
	{
		return;
	}

	const EVNHRoundPhase CurrentPhase = VNHGameState->GetRoundPhase();
	if (CurrentPhase != EVNHRoundPhase::Investigation && CurrentPhase != EVNHRoundPhase::Accusation)
	{
		return;
	}

	FVNHAccusationResult Result;
	Result.AccusedActor = AccusedShopper;
	Result.bCorrect = AccusedShopper->IsPossessedByAlien();
	Result.bResolved = true;
	VNHGameState->SetAccusationResult(Result);

	EnterPhase(EVNHRoundPhase::Reveal, PhaseTiming.RevealSeconds);
}

void AVNHGameMode::DebugStartRound()
{
	if (CountConnectedPlayers() <= 0)
	{
		UE_LOG(LogVNH, Warning, TEXT("vnh.StartRound failed: no connected players."));
		return;
	}

	if (CountConnectedPlayers() < RequiredPlayers)
	{
		for (APlayerState* PlayerState : GameState->PlayerArray)
		{
			if (AVNHPlayerState* VNHPlayerState = Cast<AVNHPlayerState>(PlayerState))
			{
				VNHPlayerState->SetRole(EVNHPlayerRole::Alien);
				UE_LOG(LogVNH, Display, TEXT("vnh.StartRound: forced single connected player to Alien for debug testing."));
				break;
			}
		}
	}
	else
	{
		AssignRoles();
		UE_LOG(LogVNH, Display, TEXT("vnh.StartRound: assigned roles for %d connected players."), CountConnectedPlayers());
	}

	if (AVNHGameState* VNHGameState = GetVNHGameState())
	{
		VNHGameState->SetRoundNumber(VNHGameState->GetRoundNumber() + 1);
		VNHGameState->SetTestsRemaining(2);
		VNHGameState->ClearRoundOutcome();
	}

	StartShopperRoutines();
	EnterPhase(EVNHRoundPhase::AssigningRoles, 3.0f);
	UE_LOG(LogVNH, Display, TEXT("vnh.StartRound: entered AssigningRoles."));
}

void AVNHGameMode::DebugForceRole(APlayerController* TargetController, EVNHPlayerRole NewRole)
{
	if (!TargetController)
	{
		UE_LOG(LogVNH, Warning, TEXT("vnh.ForceRole failed: no target controller."));
		return;
	}

	if (AVNHPlayerState* VNHPlayerState = TargetController->GetPlayerState<AVNHPlayerState>())
	{
		VNHPlayerState->SetRole(NewRole);
		UE_LOG(LogVNH, Display, TEXT("vnh.ForceRole: set role to %d."), static_cast<int32>(NewRole));
	}
	else
	{
		UE_LOG(LogVNH, Warning, TEXT("vnh.ForceRole failed: target controller has no VNH player state."));
	}
}

void AVNHGameMode::DebugTriggerPublicTest(EVNHPublicTestType TestType)
{
	if (AVNHGameState* VNHGameState = GetVNHGameState())
	{
		VNHGameState->SetActivePublicTest(TestType);
	}

	ApplyPublicTestToShoppers(TestType);
	UE_LOG(LogVNH, Display, TEXT("vnh.TriggerTest: applied public test %d."), static_cast<int32>(TestType));
}

bool AVNHGameMode::DebugPossessShopperByIndex(int32 ShopperIndex)
{
	APlayerController* AlienController = FindControllerForRole(EVNHPlayerRole::Alien);
	if (!AlienController)
	{
		AlienController = GetWorld()->GetFirstPlayerController();
	}

	if (!AlienController)
	{
		UE_LOG(LogVNH, Warning, TEXT("vnh.PossessHuman failed: no alien or local controller."));
		return false;
	}

	TArray<AVNHShopperCharacter*> Shoppers;
	for (TActorIterator<AVNHShopperCharacter> It(GetWorld()); It; ++It)
	{
		Shoppers.Add(*It);
	}

	if (!Shoppers.IsValidIndex(ShopperIndex))
	{
		UE_LOG(LogVNH, Warning, TEXT("vnh.PossessHuman failed: shopper index %d invalid. Found %d shoppers."), ShopperIndex, Shoppers.Num());
		return false;
	}

	AVNHShopperCharacter* Shopper = Shoppers[ShopperIndex];
	Shopper->SetPossessedByAlien(true);
	AlienController->Possess(Shopper);

	if (AVNHGameState* VNHGameState = GetVNHGameState())
	{
		VNHGameState->SetPossessedShopper(Shopper);
	}

	UE_LOG(LogVNH, Display, TEXT("vnh.PossessHuman: controller possessed shopper index %d (%s)."), ShopperIndex, *GetNameSafe(Shopper));
	return true;
}

bool AVNHGameMode::DebugResolveAccusation(AVNHShopperCharacter* AccusedShopper)
{
	AVNHGameState* VNHGameState = GetVNHGameState();
	if (!VNHGameState || !AccusedShopper)
	{
		UE_LOG(LogVNH, Warning, TEXT("vnh.AccuseTarget failed: no game state or accused shopper."));
		return false;
	}

	FVNHAccusationResult Result;
	Result.AccusedActor = AccusedShopper;
	Result.bCorrect = AccusedShopper->IsPossessedByAlien();
	Result.bResolved = true;
	VNHGameState->SetAccusationResult(Result);

	EnterPhase(EVNHRoundPhase::Reveal, PhaseTiming.RevealSeconds);
	UE_LOG(LogVNH, Display, TEXT("vnh.AccuseTarget: accused %s correct=%s."), *GetNameSafe(AccusedShopper), Result.bCorrect ? TEXT("true") : TEXT("false"));
	return true;
}

bool AVNHGameMode::DebugSetupVisibleTestArena()
{
	UWorld* World = GetWorld();
	if (!World)
	{
		UE_LOG(LogVNH, Warning, TEXT("vnh.SetupTestArena failed: no world."));
		return false;
	}

	if (HasDebugArena(World))
	{
		UE_LOG(LogVNH, Display, TEXT("vnh.SetupTestArena: visible test arena already exists."));
		return true;
	}

	UStaticMesh* CubeMesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
	if (!CubeMesh)
	{
		UE_LOG(LogVNH, Warning, TEXT("vnh.SetupTestArena failed: could not load engine cube mesh."));
		return false;
	}

	FActorSpawnParameters SpawnParameters;
	SpawnParameters.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

	AStaticMeshActor* Floor = World->SpawnActor<AStaticMeshActor>(AStaticMeshActor::StaticClass(), FVector(0.0f, 0.0f, -10.0f), FRotator::ZeroRotator, SpawnParameters);
	if (Floor && Floor->GetStaticMeshComponent())
	{
		Floor->Tags.Add(VNHDebugArenaTag);
#if WITH_EDITOR
		Floor->SetActorLabel(TEXT("VNH_Debug_Floor"));
#endif
		Floor->GetStaticMeshComponent()->SetStaticMesh(CubeMesh);
		Floor->GetStaticMeshComponent()->SetWorldScale3D(FVector(24.0f, 24.0f, 0.2f));
		Floor->GetStaticMeshComponent()->SetMobility(EComponentMobility::Movable);
		Floor->GetStaticMeshComponent()->SetCollisionProfileName(TEXT("BlockAll"));

		UPointLightComponent* PointLightComponent = NewObject<UPointLightComponent>(Floor, TEXT("VNH_Debug_PointLight"));
		if (PointLightComponent)
		{
			PointLightComponent->SetupAttachment(Floor->GetRootComponent());
			PointLightComponent->SetRelativeLocation(FVector(0.0f, 0.0f, 500.0f));
			PointLightComponent->SetMobility(EComponentMobility::Movable);
			PointLightComponent->SetIntensity(50000.0f);
			PointLightComponent->SetAttenuationRadius(5000.0f);
			PointLightComponent->RegisterComponent();
		}
	}

	UE_LOG(LogVNH, Display, TEXT("vnh.SetupTestArena: spawned lit floor and point light."));
	return Floor != nullptr;
}

bool AVNHGameMode::DebugSpawnAndPossessTestShopper(const FVector& SpawnLocation)
{
	DebugSetupVisibleTestArena();

	APlayerController* AlienController = FindControllerForRole(EVNHPlayerRole::Alien);
	if (!AlienController)
	{
		AlienController = GetWorld()->GetFirstPlayerController();
	}

	if (!AlienController)
	{
		UE_LOG(LogVNH, Warning, TEXT("vnh.SpawnTestHuman failed: no alien or local controller."));
		return false;
	}

	if (AVNHPlayerState* VNHPlayerState = AlienController->GetPlayerState<AVNHPlayerState>())
	{
		VNHPlayerState->SetRole(EVNHPlayerRole::Alien);
	}

	FActorSpawnParameters SpawnParameters;
	SpawnParameters.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AdjustIfPossibleButAlwaysSpawn;

	const FRotator SpawnRotation(0.0f, AlienController->GetControlRotation().Yaw, 0.0f);
	AVNHShopperCharacter* Shopper = GetWorld()->SpawnActor<AVNHShopperCharacter>(AVNHShopperCharacter::StaticClass(), SpawnLocation, SpawnRotation, SpawnParameters);
	if (!Shopper)
	{
		UE_LOG(LogVNH, Warning, TEXT("vnh.SpawnTestHuman failed: could not spawn shopper."));
		return false;
	}

	Shopper->SetPossessedByAlien(true);
	AlienController->Possess(Shopper);
	AlienController->SetViewTarget(Shopper);

	if (AVNHGameState* VNHGameState = GetVNHGameState())
	{
		VNHGameState->SetPossessedShopper(Shopper);
	}

	UE_LOG(LogVNH, Display, TEXT("vnh.SpawnTestHuman: spawned and possessed %s at %s."), *GetNameSafe(Shopper), *SpawnLocation.ToCompactString());
	return true;
}

void AVNHGameMode::DebugStartShopperRoutines()
{
	StartShopperRoutines();
}

AVNHGameState* AVNHGameMode::GetVNHGameState() const
{
	return GetGameState<AVNHGameState>();
}

void AVNHGameMode::AssignRoles()
{
	TArray<AVNHPlayerState*> PlayerStates;
	for (APlayerState* PlayerState : GameState->PlayerArray)
	{
		if (AVNHPlayerState* VNHPlayerState = Cast<AVNHPlayerState>(PlayerState))
		{
			PlayerStates.Add(VNHPlayerState);
		}
	}

	if (PlayerStates.Num() < RequiredPlayers)
	{
		return;
	}

	const int32 AlienIndex = FMath::RandRange(0, PlayerStates.Num() - 1);
	for (int32 Index = 0; Index < PlayerStates.Num(); ++Index)
	{
		PlayerStates[Index]->SetRole(Index == AlienIndex ? EVNHPlayerRole::Alien : EVNHPlayerRole::Hunter);
	}
}

void AVNHGameMode::EnterPhase(EVNHRoundPhase NewPhase, float DurationSeconds)
{
	AVNHGameState* VNHGameState = GetVNHGameState();
	if (!VNHGameState)
	{
		return;
	}

	GetWorldTimerManager().ClearTimer(PhaseTimerHandle);

	const float EndTime = DurationSeconds > 0.0f ? GetWorld()->GetTimeSeconds() + DurationSeconds : 0.0f;
	VNHGameState->SetRoundPhase(NewPhase, EndTime);

	if (DurationSeconds > 0.0f)
	{
		GetWorldTimerManager().SetTimer(PhaseTimerHandle, this, &AVNHGameMode::AdvanceRoundPhase, DurationSeconds, false);
	}
}

int32 AVNHGameMode::CountConnectedPlayers() const
{
	return GameState ? GameState->PlayerArray.Num() : 0;
}

bool AVNHGameMode::IsHunterController(const APlayerController* PlayerController) const
{
	if (!PlayerController)
	{
		return false;
	}

	const AVNHPlayerState* VNHPlayerState = PlayerController->GetPlayerState<AVNHPlayerState>();
	return VNHPlayerState && VNHPlayerState->IsHunter();
}

APlayerController* AVNHGameMode::FindControllerForRole(EVNHPlayerRole TargetRole) const
{
	for (FConstPlayerControllerIterator It = GetWorld()->GetPlayerControllerIterator(); It; ++It)
	{
		APlayerController* PlayerController = It->Get();
		const AVNHPlayerState* VNHPlayerState = PlayerController ? PlayerController->GetPlayerState<AVNHPlayerState>() : nullptr;
		if (VNHPlayerState && VNHPlayerState->GetRole() == TargetRole)
		{
			return PlayerController;
		}
	}

	return nullptr;
}

AVNHShopperCharacter* AVNHGameMode::SelectAlienShopper() const
{
	TArray<AVNHShopperCharacter*> EligibleShoppers;
	for (TActorIterator<AVNHShopperCharacter> It(GetWorld()); It; ++It)
	{
		if (!It->IsPossessedByAlien())
		{
			EligibleShoppers.Add(*It);
		}
	}

	if (EligibleShoppers.IsEmpty())
	{
		return nullptr;
	}

	return EligibleShoppers[FMath::RandRange(0, EligibleShoppers.Num() - 1)];
}

void AVNHGameMode::PossessAlienShopper()
{
	AVNHGameState* VNHGameState = GetVNHGameState();
	APlayerController* AlienController = FindControllerForRole(EVNHPlayerRole::Alien);
	AVNHShopperCharacter* Shopper = SelectAlienShopper();
	if (!VNHGameState || !AlienController || !Shopper)
	{
		return;
	}

	Shopper->SetPossessedByAlien(true);
	AlienController->Possess(Shopper);
	VNHGameState->SetPossessedShopper(Shopper);
}

void AVNHGameMode::StartShopperRoutines()
{
	int32 StartedCount = 0;
	for (TActorIterator<AVNHShopperCharacter> It(GetWorld()); It; ++It)
	{
		if (!It->IsPossessedByAlien() && It->GetRoutineComponent() && It->GetRoutineComponent()->GetCurrentWaypoint())
		{
			It->StartRoutineMovement();
			++StartedCount;
		}
	}

	UE_LOG(LogVNH, Display, TEXT("ShopperRoutine: requested routine movement for %d shoppers."), StartedCount);
}

void AVNHGameMode::ApplyPublicTestToShoppers(EVNHPublicTestType TestType)
{
	for (TActorIterator<AVNHShopperCharacter> It(GetWorld()); It; ++It)
	{
		It->ApplyPublicTest(TestType);
	}
}
