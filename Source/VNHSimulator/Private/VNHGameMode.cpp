#include "VNHGameMode.h"

#include "Components/CapsuleComponent.h"
#include "Components/PointLightComponent.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/HUD.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/PlayerStart.h"
#include "GameFramework/PlayerState.h"
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

FString DecodeTravelOption(const FString& Value)
{
	FString Decoded = Value;
	Decoded.ReplaceInline(TEXT("%20"), TEXT(" "));
	Decoded.ReplaceInline(TEXT("%3F"), TEXT("?"));
	Decoded.ReplaceInline(TEXT("%3D"), TEXT("="));
	Decoded.ReplaceInline(TEXT("%26"), TEXT("&"));
	Decoded.ReplaceInline(TEXT("%25"), TEXT("%"));
	return Decoded;
}
}

AVNHGameMode::AVNHGameMode()
{
	ConfigureRuntimeClasses();
}

void AVNHGameMode::ConfigureRuntimeClasses()
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

	if (MapName.Contains(TEXT("MainMenu")))
	{
		GameStateClass = AVNHGameState::StaticClass();
		PlayerControllerClass = APlayerController::StaticClass();
		PlayerStateClass = APlayerState::StaticClass();
		HUDClass = AHUD::StaticClass();
		DefaultPawnClass = nullptr;
		UE_LOG(LogVNH, Display, TEXT("MainMenu: using menu-only runtime classes; no gameplay pawn/controller."));
	}
	else
	{
		ConfigureRuntimeClasses();
	}

	bStartRoundWhenPlayersReady = UGameplayStatics::HasOption(Options, TEXT("StartRound"));
	const int32 RequestedPlayers = UGameplayStatics::GetIntOption(Options, TEXT("MaxPlayers"), RequiredPlayers);
	RequiredPlayers = FMath::Clamp(RequestedPlayers, 3, 20);

	const int32 RequestedRoundSeconds = UGameplayStatics::GetIntOption(Options, TEXT("RoundSeconds"), FMath::RoundToInt(PhaseTiming.InvestigationSeconds));
	PhaseTiming.InvestigationSeconds = FMath::Clamp(static_cast<float>(RequestedRoundSeconds), 60.0f, 600.0f);
	ServerName = DecodeTravelOption(UGameplayStatics::ParseOption(Options, TEXT("ServerName")));
	if (ServerName.IsEmpty())
	{
		ServerName = TEXT("My Awesome Game");
	}
	bPrivateSession = UGameplayStatics::GetIntOption(Options, TEXT("Private"), 0) != 0;
	ServerPassword = DecodeTravelOption(UGameplayStatics::ParseOption(Options, TEXT("Password")));
}

void AVNHGameMode::PreLogin(const FString& Options, const FString& Address, const FUniqueNetIdRepl& UniqueId, FString& ErrorMessage)
{
	Super::PreLogin(Options, Address, UniqueId, ErrorMessage);
	if (!ErrorMessage.IsEmpty() || !bPrivateSession)
	{
		return;
	}

	const FString SuppliedPassword = DecodeTravelOption(UGameplayStatics::ParseOption(Options, TEXT("Password")));
	if (!ServerPassword.IsEmpty() && SuppliedPassword != ServerPassword)
	{
		ErrorMessage = TEXT("Incorrect server password.");
		UE_LOG(LogVNH, Warning, TEXT("PrivateLobby: rejected login from %s due to incorrect password."), *Address);
	}
}

void AVNHGameMode::BeginPlay()
{
	Super::BeginPlay();

	const FString MapName = GetWorld() ? GetWorld()->GetMapName() : FString();
	if (MapName.Contains(TEXT("MainMenu")))
	{
		return;
	}

	if (MapName.Contains(TEXT("Lobby")))
	{
		EnsureLobbyRuntimeActors();
	}
	else
	{
		EnsureMvpInteractionProps();
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

	for (APlayerState* PlayerState : GameState->PlayerArray)
	{
		if (AVNHPlayerState* VNHPlayerState = Cast<AVNHPlayerState>(PlayerState))
		{
			VNHPlayerState->SetPreRoundReady(false);
		}
	}

	ResetShopperPossessionState();
	StartShopperRoutines();
	EnterPhase(EVNHRoundPhase::AssigningRoles, PhaseTiming.PreRoundCustomizationSeconds);
}

bool AVNHGameMode::StartRoundFromLobby(APlayerController* RequestingPlayer)
{
	if (!IsHostController(RequestingPlayer))
	{
		UE_LOG(LogVNH, Warning, TEXT("LobbyStart: rejected non-host start request from %s."), *GetNameSafe(RequestingPlayer));
		return false;
	}

	UWorld* World = GetWorld();
	const FString MapName = World ? World->GetMapName() : FString();
	if (MapName.Contains(TEXT("Lobby")))
	{
		const APawn* RequestingPawn = RequestingPlayer ? RequestingPlayer->GetPawn() : nullptr;
		bool bNearStartButton = false;
		for (TActorIterator<AVNHLobbyPlayButton> It(World); It; ++It)
		{
			if (It->IsPawnWithinInteractionRange(RequestingPawn))
			{
				bNearStartButton = true;
				break;
			}
		}

		if (!bNearStartButton)
		{
			UE_LOG(LogVNH, Warning, TEXT("LobbyStart: rejected host start request because the host is not near the start console."));
			return false;
		}
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

	if (World)
	{
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
		int32 AssignedIndex = 0;
		for (APlayerState* PlayerState : GameState->PlayerArray)
		{
			if (AVNHPlayerState* VNHPlayerState = Cast<AVNHPlayerState>(PlayerState))
			{
				const EVNHPlayerRole ForcedRole = AssignedIndex == 0
					? EVNHPlayerRole::Alien
					: EVNHPlayerRole::Hunter;
				VNHPlayerState->SetRole(ForcedRole);
				VNHPlayerState->SetLightErrandText(ForcedRole == EVNHPlayerRole::Hunter ? FText::GetEmpty() : GetVNHLightErrand(AssignedIndex));
				++AssignedIndex;
			}
		}
		UE_LOG(LogVNH, Display, TEXT("vnh.StartRound: assigned debug roles for %d connected player(s)."), AssignedIndex);
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

	for (APlayerState* PlayerState : GameState->PlayerArray)
	{
		if (AVNHPlayerState* VNHPlayerState = Cast<AVNHPlayerState>(PlayerState))
		{
			VNHPlayerState->SetPreRoundReady(false);
		}
	}

	ResetShopperPossessionState();
	StartShopperRoutines();
	EnterPhase(EVNHRoundPhase::AssigningRoles, PhaseTiming.PreRoundCustomizationSeconds);
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

void AVNHGameMode::RequestPreRoundReady(AVNHPlayerController* RequestingPlayer)
{
	AVNHGameState* VNHGameState = GetVNHGameState();
	if (!VNHGameState || VNHGameState->GetRoundPhase() != EVNHRoundPhase::AssigningRoles)
	{
		return;
	}

	AVNHPlayerState* VNHPlayerState = RequestingPlayer ? RequestingPlayer->GetPlayerState<AVNHPlayerState>() : nullptr;
	if (!VNHPlayerState)
	{
		return;
	}

	VNHPlayerState->SetPreRoundReady(true);
	UE_LOG(LogVNH, Display, TEXT("PreroundCustomization: %s is ready."), *GetNameSafe(RequestingPlayer));

	if (AreAllConnectedPlayersPreRoundReady())
	{
		UE_LOG(LogVNH, Display, TEXT("PreroundCustomization: all players ready, advancing early."));
		AdvanceRoundPhase();
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
	Shoppers.Sort([](const AVNHShopperCharacter& LeftShopper, const AVNHShopperCharacter& RightShopper)
	{
		return LeftShopper.GetFName().LexicalLess(RightShopper.GetFName());
	});

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
	Shopper->PrepareForPlayerPossession();

	FHitResult GroundHit;
	FCollisionQueryParams GroundQueryParams(SCENE_QUERY_STAT(VNHDebugPossessionGround), false, Shopper);
	const FVector GroundTraceStart = Shopper->GetActorLocation() + FVector(0.0f, 0.0f, 200.0f);
	const FVector GroundTraceEnd = Shopper->GetActorLocation() - FVector(0.0f, 0.0f, 2000.0f);
	if (GetWorld()->LineTraceSingleByChannel(GroundHit, GroundTraceStart, GroundTraceEnd, ECC_Visibility, GroundQueryParams))
	{
		const float CapsuleHalfHeight = Shopper->GetCapsuleComponent() ? Shopper->GetCapsuleComponent()->GetScaledCapsuleHalfHeight() : 88.0f;
		const FVector GroundedLocation = GroundHit.ImpactPoint + FVector(0.0f, 0.0f, CapsuleHalfHeight + 2.0f);
		Shopper->SetActorLocation(GroundedLocation, false, nullptr, ETeleportType::TeleportPhysics);
	}
	AlienController->Possess(Shopper);
	AlienController->SetViewTarget(Shopper);

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

void AVNHGameMode::EnsureMvpInteractionProps()
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	for (TActorIterator<AActor> It(World); It; ++It)
	{
		if (It->ActorHasTag(TEXT("VNH.MVPProp")))
		{
			return;
		}
	}

	AActor* AnchorActor = UGameplayStatics::GetActorOfClass(World, APlayerStart::StaticClass());
	if (!AnchorActor)
	{
		AnchorActor = UGameplayStatics::GetActorOfClass(World, AVNHShopperCharacter::StaticClass());
	}
	const FVector AnchorLocation = AnchorActor ? AnchorActor->GetActorLocation() : FVector::ZeroVector;

	struct FPropSpawnDefinition
	{
		const TCHAR* ClassPath;
		const TCHAR* Label;
		FVector Offset;
		bool bSuspicious;
	};

	const FPropSpawnDefinition Definitions[] = {
		{TEXT("/Game/Interactions/BP_VNHProp_Box.BP_VNHProp_Box_C"), TEXT("MVP_Prop_Box"), FVector(180.0f, -140.0f, 45.0f), false},
		{TEXT("/Game/Interactions/BP_VNHProp_Bag.BP_VNHProp_Bag_C"), TEXT("MVP_Prop_Bag"), FVector(180.0f, -70.0f, 45.0f), false},
		{TEXT("/Game/Interactions/BP_VNHProp_Cup.BP_VNHProp_Cup_C"), TEXT("MVP_Prop_Cup"), FVector(180.0f, 0.0f, 45.0f), false},
		{TEXT("/Game/Interactions/BP_VNHProp_Tool.BP_VNHProp_Tool_C"), TEXT("MVP_Prop_Tool"), FVector(180.0f, 70.0f, 45.0f), false},
		{TEXT("/Game/Interactions/BP_VNHProp_Suspicious.BP_VNHProp_Suspicious_C"), TEXT("MVP_Prop_Suspicious"), FVector(180.0f, 140.0f, 45.0f), true},
	};

	FActorSpawnParameters SpawnParameters;
	SpawnParameters.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	int32 SpawnedCount = 0;
	for (const FPropSpawnDefinition& Definition : Definitions)
	{
		UClass* PropClass = LoadClass<AActor>(nullptr, Definition.ClassPath);
		if (!PropClass)
		{
			UE_LOG(LogVNH, Warning, TEXT("MVPProps: missing %s."), Definition.ClassPath);
			continue;
		}

		AActor* Prop = World->SpawnActor<AActor>(PropClass, AnchorLocation + Definition.Offset, FRotator::ZeroRotator, SpawnParameters);
		if (!Prop)
		{
			continue;
		}

		Prop->Tags.AddUnique(TEXT("VNH.MVPProp"));
		Prop->Tags.AddUnique(TEXT("VNH.Interactable"));
		if (Definition.bSuspicious)
		{
			Prop->Tags.AddUnique(TEXT("VNH.Suspicious"));
		}
		Prop->SetReplicates(true);
		Prop->SetReplicateMovement(true);
#if WITH_EDITOR
		Prop->SetActorLabel(Definition.Label);
#endif
		++SpawnedCount;
	}

	UE_LOG(LogVNH, Display, TEXT("MVPProps: spawned %d reusable interaction props."), SpawnedCount);
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
	if (!PlayerController)
	{
		return false;
	}

	if (PlayerController->IsLocalController() && GetNetMode() != NM_Client)
	{
		return true;
	}

	if (!GameState || GameState->PlayerArray.IsEmpty())
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

bool AVNHGameMode::AreAllConnectedPlayersPreRoundReady() const
{
	if (!GameState || GameState->PlayerArray.IsEmpty())
	{
		return false;
	}

	for (APlayerState* PlayerState : GameState->PlayerArray)
	{
		const AVNHPlayerState* VNHPlayerState = Cast<AVNHPlayerState>(PlayerState);
		if (!VNHPlayerState || !VNHPlayerState->IsPreRoundReady())
		{
			return false;
		}
	}

	return true;
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
