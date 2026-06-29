#include "VNHGameMode.h"

#include "Components/PointLightComponent.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/HUD.h"
#include "GameFramework/PlayerController.h"
#include "Kismet/GameplayStatics.h"
#include "TimerManager.h"
#include "VNHGameInstance.h"
#include "VNHGameState.h"
#include "VNHLobbyPlayButton.h"
#include "VNHLog.h"
#include "VNHPlayerController.h"
#include "VNHPlayerState.h"
#include "VNHShopperCharacter.h"

namespace
{
const FName VNHDebugArenaTag(TEXT("VNHDebugArena"));
const TCHAR* VNHStoreMapTravelURL = TEXT("/Game/Maps/MVP_ClothingStore?StartRound=1");

const FText& GetVNHLightErrand(int32 Index)
{
	static const TArray<FText> LightErrands = {
		NSLOCTEXT("VNH", "ErrandBrowseRacks", "Browse two clothing racks without looking rushed."),
		NSLOCTEXT("VNH", "ErrandInspectMirror", "Inspect the mirror, then return to shopping."),
		NSLOCTEXT("VNH", "ErrandWaitCheckout", "Wait near checkout like you are about to pay."),
		NSLOCTEXT("VNH", "ErrandFindShirt", "Look for a shirt and act like you found the wrong size."),
		NSLOCTEXT("VNH", "ErrandCheckPhone", "Check your phone, then keep browsing.")
	};

	return LightErrands[Index % LightErrands.Num()];
}

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
	HUDClass = AHUD::StaticClass();
	DefaultPawnClass = AVNHShopperCharacter::StaticClass();
}

void AVNHGameMode::InitGame(const FString& MapName, const FString& Options, FString& ErrorMessage)
{
	Super::InitGame(MapName, Options, ErrorMessage);
	bStartRoundWhenPlayersReady = UGameplayStatics::HasOption(Options, TEXT("StartRound"));
}

void AVNHGameMode::BeginPlay()
{
	Super::BeginPlay();

	const FString MapName = GetWorld() ? GetWorld()->GetMapName() : FString();
	if (MapName.Contains(TEXT("MainMenu")))
	{
		if (UVNHGameInstance* VNHGameInstance = GetGameInstance<UVNHGameInstance>())
		{
			VNHGameInstance->ShowMainMenu();
		}
		return;
	}

	if (MapName.Contains(TEXT("Lobby")))
	{
		EnsureLobbyRuntimeActors();
	}

	if (bAutoStartRoundOnPlayerJoin)
	{
		TryStartRound();
	}
}

void AVNHGameMode::PostLogin(APlayerController* NewPlayer)
{
	Super::PostLogin(NewPlayer);

	const FString MapName = GetWorld() ? GetWorld()->GetMapName() : FString();
	if (MapName.Contains(TEXT("Lobby")))
	{
		if (AVNHShopperCharacter* LobbyPawn = NewPlayer ? Cast<AVNHShopperCharacter>(NewPlayer->GetPawn()) : nullptr)
		{
			LobbyPawn->SetPossessedByAlien(true);
		}
	}

	if (bAutoStartRoundOnPlayerJoin || bStartRoundWhenPlayersReady)
	{
		TryStartRound();
	}
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
		if (bStartRoundWhenPlayersReady && CountConnectedPlayers() > 0)
		{
			bStartRoundWhenPlayersReady = false;
			DebugStartRound();
			UE_LOG(LogVNH, Display, TEXT("LobbyStart: used debug solo start path with %d connected player(s)."), CountConnectedPlayers());
			return;
		}

		if (AVNHGameState* VNHGameState = GetVNHGameState())
		{
			VNHGameState->SetRoundPhase(EVNHRoundPhase::WaitingForPlayers, 0.0f);
		}
		return;
	}

	AssignRoles();
	bStartRoundWhenPlayersReady = false;

	if (AVNHGameState* VNHGameState = GetVNHGameState())
	{
		VNHGameState->SetRoundNumber(VNHGameState->GetRoundNumber() + 1);
		VNHGameState->SetTestsRemaining(PublicTestsPerRound);
		VNHGameState->SetDirectQuestionsRemaining(QuestionsPerRound);
		VNHGameState->SetAccusationsRemaining(1);
		VNHGameState->ClearRoundOutcome();
	}

	ResetShopperPossessionState();
	StartShopperRoutines();
	EnterPhase(EVNHRoundPhase::AssigningRoles, 3.0f);
}

bool AVNHGameMode::StartRoundFromLobby(APlayerController* RequestingPlayer)
{
	if (!IsHostController(RequestingPlayer))
	{
		UE_LOG(LogVNH, Warning, TEXT("LobbyStart: rejected non-host start request from %s."), *GetNameSafe(RequestingPlayer));
		return false;
	}

	if (CountConnectedPlayers() < RequiredPlayers)
	{
		if (CountConnectedPlayers() <= 0)
		{
			UE_LOG(LogVNH, Warning, TEXT("LobbyStart: need at least one connected player."));
			return false;
		}

		UE_LOG(LogVNH, Display, TEXT("LobbyStart: continuing with %d/%d players for local test flow."), CountConnectedPlayers(), RequiredPlayers);
	}

	if (UWorld* World = GetWorld())
	{
		const FString MapName = World->GetMapName();
		if (MapName.Contains(TEXT("MVP_ClothingStore")))
		{
			TryStartRound();
			UE_LOG(LogVNH, Display, TEXT("LobbyStart: host started round with %d connected players in the store."), CountConnectedPlayers());
			return true;
		}

		World->ServerTravel(VNHStoreMapTravelURL);
		UE_LOG(LogVNH, Display, TEXT("LobbyStart: travelling %d connected players to %s."), CountConnectedPlayers(), VNHStoreMapTravelURL);
		return true;
	}

	return false;
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
		if (CountConnectedPlayers() >= RequiredPlayers)
		{
			TryStartRound();
		}
		else if (CountConnectedPlayers() > 0)
		{
			DebugStartRound();
		}
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

bool AVNHGameMode::RequestDirectQuestion(AVNHPlayerController* RequestingPlayer, AVNHShopperCharacter* QuestionedShopper, FString& OutResponseText)
{
	OutResponseText.Reset();

	AVNHGameState* VNHGameState = GetVNHGameState();
	if (!VNHGameState || !IsHunterController(RequestingPlayer) || !QuestionedShopper)
	{
		return false;
	}

	if (VNHGameState->GetRoundPhase() != EVNHRoundPhase::Investigation || VNHGameState->GetDirectQuestionsRemaining() <= 0)
	{
		return false;
	}

	VNHGameState->SetDirectQuestionsRemaining(VNHGameState->GetDirectQuestionsRemaining() - 1);
	OutResponseText = FString::Printf(TEXT("%s: %s"), *GetNameSafe(QuestionedShopper), *QuestionedShopper->BuildQuestionResponse());
	return true;
}

bool AVNHGameMode::RequestQuestion(AVNHPlayerController* RequestingPlayer, AVNHShopperCharacter* QuestionedShopper)
{
	FString UnusedResponse;
	return RequestDirectQuestion(RequestingPlayer, QuestionedShopper, UnusedResponse);
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

	if (VNHGameState->GetAccusationsRemaining() <= 0)
	{
		return;
	}

	VNHGameState->SetAccusationsRemaining(VNHGameState->GetAccusationsRemaining() - 1);

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
		VNHGameState->SetTestsRemaining(PublicTestsPerRound);
		VNHGameState->SetDirectQuestionsRemaining(QuestionsPerRound);
		VNHGameState->SetAccusationsRemaining(1);
		VNHGameState->ClearRoundOutcome();
	}

	ResetShopperPossessionState();
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
		VNHGameState->SetTestsRemaining(FMath::Max(0, VNHGameState->GetTestsRemaining() - 1));
		VNHGameState->SetActivePublicTest(TestType);
	}

	ApplyPublicTestToShoppers(TestType);
	UE_LOG(LogVNH, Display, TEXT("vnh.TriggerTest: applied public test %d."), static_cast<int32>(TestType));
}

void AVNHGameMode::DebugJumpToInvestigation()
{
	if (CountConnectedPlayers() <= 0)
	{
		UE_LOG(LogVNH, Warning, TEXT("vnh.JumpInvestigation failed: no connected players."));
		return;
	}

	APlayerController* LocalController = GetWorld() ? GetWorld()->GetFirstPlayerController() : nullptr;
	if (AVNHPlayerState* VNHPlayerState = LocalController ? LocalController->GetPlayerState<AVNHPlayerState>() : nullptr)
	{
		VNHPlayerState->SetRole(EVNHPlayerRole::Alien);
	}

	if (AVNHGameState* VNHGameState = GetVNHGameState())
	{
		VNHGameState->SetRoundNumber(VNHGameState->GetRoundNumber() + 1);
		VNHGameState->SetTestsRemaining(PublicTestsPerRound);
		VNHGameState->SetQuestionsRemaining(QuestionsPerRound);
		VNHGameState->SetAccusationsRemaining(1);
		VNHGameState->ClearRoundOutcome();
	}

	ResetShopperPossessionState();
	StartShopperRoutines();
	if (!DebugPossessShopperByIndex(0))
	{
		PossessAlienShopper();
	}
	EnterPhase(EVNHRoundPhase::Investigation, PhaseTiming.InvestigationSeconds);
	UE_LOG(LogVNH, Display, TEXT("vnh.JumpInvestigation: entered Investigation with two test charges."));
}

bool AVNHGameMode::DebugPossessShopperByIndex(int32 ShopperIndex, EVNHPlayerRole ForcedRole)
{
	APlayerController* AlienController = GetWorld() ? GetWorld()->GetFirstPlayerController() : nullptr;
	if (!AlienController && ForcedRole == EVNHPlayerRole::Alien)
	{
		AlienController = FindControllerForRole(EVNHPlayerRole::Alien);
	}

	if (!AlienController)
	{
		UE_LOG(LogVNH, Warning, TEXT("vnh.PossessHuman failed: no alien or local controller."));
		return false;
	}

	if (AVNHPlayerState* VNHPlayerState = AlienController->GetPlayerState<AVNHPlayerState>())
	{
		VNHPlayerState->SetRole(ForcedRole);
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
	for (AVNHShopperCharacter* ExistingShopper : Shoppers)
	{
		if (ExistingShopper && ExistingShopper != Shopper)
		{
			ExistingShopper->SetPossessedByAlien(false);
		}
	}

	Shopper->SetPossessedByAlien(ForcedRole == EVNHPlayerRole::Alien);
	AlienController->Possess(Shopper);

	if (AVNHGameState* VNHGameState = GetVNHGameState())
	{
		VNHGameState->SetPossessedShopper(ForcedRole == EVNHPlayerRole::Alien ? Shopper : nullptr);
	}

	UE_LOG(LogVNH, Display, TEXT("vnh.PossessHuman: controller possessed shopper index %d (%s) as role %d."), ShopperIndex, *GetNameSafe(Shopper), static_cast<int32>(ForcedRole));
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
	VNHGameState->SetAccusationsRemaining(0);

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

void AVNHGameMode::EnsureLobbyRuntimeActors()
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	for (TActorIterator<AVNHLobbyPlayButton> It(World); It; ++It)
	{
		return;
	}

	FActorSpawnParameters SpawnParameters;
	SpawnParameters.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	AVNHLobbyPlayButton* PlayButton = World->SpawnActor<AVNHLobbyPlayButton>(
		AVNHLobbyPlayButton::StaticClass(),
		FVector(0.0f, 350.0f, 35.0f),
		FRotator::ZeroRotator,
		SpawnParameters);

#if WITH_EDITOR
	if (PlayButton)
	{
		PlayButton->SetActorLabel(TEXT("Lobby_PlayButton_Runtime"));
	}
#endif

	UE_LOG(LogVNH, Display, TEXT("LobbyStart: runtime play button %s."), PlayButton ? TEXT("spawned") : TEXT("failed to spawn"));
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

	const int32 HunterIndex = FMath::RandRange(0, PlayerStates.Num() - 1);
	int32 AlienIndex = FMath::RandRange(0, PlayerStates.Num() - 2);
	if (AlienIndex >= HunterIndex)
	{
		++AlienIndex;
	}

	for (int32 Index = 0; Index < PlayerStates.Num(); ++Index)
	{
		EVNHPlayerRole AssignedRole = EVNHPlayerRole::Human;
		if (Index == HunterIndex)
		{
			AssignedRole = EVNHPlayerRole::Hunter;
		}
		else if (Index == AlienIndex)
		{
			AssignedRole = EVNHPlayerRole::Alien;
		}

		PlayerStates[Index]->SetRole(AssignedRole);
		PlayerStates[Index]->SetLightErrandText(AssignedRole == EVNHPlayerRole::Hunter ? FText::GetEmpty() : GetVNHLightErrand(Index));
	}

	UE_LOG(LogVNH, Display, TEXT("RoleAssign: %d players -> Hunter=%d Alien=%d Humans=%d."),
		PlayerStates.Num(),
		HunterIndex,
		AlienIndex,
		FMath::Max(0, PlayerStates.Num() - 2));
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

bool AVNHGameMode::IsHostController(const APlayerController* PlayerController) const
{
	if (!PlayerController || !GameState || GameState->PlayerArray.IsEmpty())
	{
		return false;
	}

	return PlayerController->PlayerState == GameState->PlayerArray[0];
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

void AVNHGameMode::ResetShopperPossessionState()
{
	for (TActorIterator<AVNHShopperCharacter> It(GetWorld()); It; ++It)
	{
		It->SetPossessedByAlien(false);
	}
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
