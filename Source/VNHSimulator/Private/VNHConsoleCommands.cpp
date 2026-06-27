#include "VNHGameMode.h"

#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "HAL/IConsoleManager.h"
#include "VNHAlienLocomotionComponent.h"
#include "VNHGameplayTypes.h"
#include "VNHLog.h"
#include "VNHPlayerController.h"
#include "VNHShopperCharacter.h"

namespace
{
AVNHGameMode* GetVNHGameMode(UWorld* World)
{
	return World ? World->GetAuthGameMode<AVNHGameMode>() : nullptr;
}

UVNHAlienLocomotionComponent* GetAlienLocomotionComponent(UWorld* World)
{
	APlayerController* PlayerController = World ? World->GetFirstPlayerController() : nullptr;
	AVNHShopperCharacter* Shopper = PlayerController ? Cast<AVNHShopperCharacter>(PlayerController->GetPawn()) : nullptr;
	return Shopper ? Shopper->GetAlienLocomotionComponent() : nullptr;
}

AVNHPlayerController* GetVNHPlayerController(UWorld* World)
{
	return World ? Cast<AVNHPlayerController>(World->GetFirstPlayerController()) : nullptr;
}

EVNHPlayerRole ParseRole(const FString& RoleName)
{
	if (RoleName.Equals(TEXT("Hunter"), ESearchCase::IgnoreCase))
	{
		return EVNHPlayerRole::Hunter;
	}

	if (RoleName.Equals(TEXT("Alien"), ESearchCase::IgnoreCase))
	{
		return EVNHPlayerRole::Alien;
	}

	return EVNHPlayerRole::Unassigned;
}

EVNHPublicTestType ParsePublicTest(const FString& TestName)
{
	if (TestName.Equals(TEXT("LookToEntrance"), ESearchCase::IgnoreCase))
	{
		return EVNHPublicTestType::LookToEntrance;
	}

	if (TestName.Equals(TEXT("ClearAisle"), ESearchCase::IgnoreCase))
	{
		return EVNHPublicTestType::ClearAisle;
	}

	if (TestName.Equals(TEXT("CheckoutOpen"), ESearchCase::IgnoreCase))
	{
		return EVNHPublicTestType::CheckoutOpen;
	}

	return EVNHPublicTestType::Freeze;
}

FAutoConsoleCommandWithWorld VNHStartRoundCommand(
	TEXT("vnh.StartRound"),
	TEXT("Debug: start a VNH round, allowing single-player Alien testing when fewer than the required player count is present."),
	FConsoleCommandWithWorldDelegate::CreateStatic([](UWorld* World)
	{
		if (AVNHGameMode* GameMode = GetVNHGameMode(World))
		{
			GameMode->DebugStartRound();
		}
		else
		{
			UE_LOG(LogVNH, Warning, TEXT("vnh.StartRound failed: no VNH auth game mode in current world."));
		}
	}));

FAutoConsoleCommandWithWorld VNHSkipPhaseCommand(
	TEXT("vnh.SkipPhase"),
	TEXT("Debug: advance the VNH round phase."),
	FConsoleCommandWithWorldDelegate::CreateStatic([](UWorld* World)
	{
		if (AVNHGameMode* GameMode = GetVNHGameMode(World))
		{
			GameMode->AdvanceRoundPhase();
			UE_LOG(LogVNH, Display, TEXT("vnh.SkipPhase: advanced round phase."));
		}
		else
		{
			UE_LOG(LogVNH, Warning, TEXT("vnh.SkipPhase failed: no VNH auth game mode in current world."));
		}
	}));

FAutoConsoleCommandWithWorldAndArgs VNHForceRoleCommand(
	TEXT("vnh.ForceRole"),
	TEXT("Debug: force the first local player role. Usage: vnh.ForceRole Alien|Hunter"),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic([](const TArray<FString>& Args, UWorld* World)
	{
		if (AVNHGameMode* GameMode = GetVNHGameMode(World))
		{
			const EVNHPlayerRole Role = Args.IsValidIndex(0) ? ParseRole(Args[0]) : EVNHPlayerRole::Unassigned;
			GameMode->DebugForceRole(World ? World->GetFirstPlayerController() : nullptr, Role);
		}
		else
		{
			UE_LOG(LogVNH, Warning, TEXT("vnh.ForceRole failed: no VNH auth game mode in current world."));
		}
	}));

FAutoConsoleCommandWithWorldAndArgs VNHTriggerTestCommand(
	TEXT("vnh.TriggerTest"),
	TEXT("Debug: trigger a public test. Usage: vnh.TriggerTest Freeze|LookToEntrance|ClearAisle|CheckoutOpen"),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic([](const TArray<FString>& Args, UWorld* World)
	{
		if (AVNHGameMode* GameMode = GetVNHGameMode(World))
		{
			GameMode->DebugTriggerPublicTest(Args.IsValidIndex(0) ? ParsePublicTest(Args[0]) : EVNHPublicTestType::Freeze);
		}
		else
		{
			UE_LOG(LogVNH, Warning, TEXT("vnh.TriggerTest failed: no VNH auth game mode in current world."));
		}
	}));

FAutoConsoleCommandWithWorldAndArgs VNHPossessHumanCommand(
	TEXT("vnh.PossessHuman"),
	TEXT("Debug: possess a shopper by zero-based index. Usage: vnh.PossessHuman 0"),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic([](const TArray<FString>& Args, UWorld* World)
	{
		if (AVNHGameMode* GameMode = GetVNHGameMode(World))
		{
			const int32 ShopperIndex = Args.IsValidIndex(0) ? FCString::Atoi(*Args[0]) : 0;
			GameMode->DebugPossessShopperByIndex(ShopperIndex);
		}
		else
		{
			UE_LOG(LogVNH, Warning, TEXT("vnh.PossessHuman failed: no VNH auth game mode in current world."));
		}
	}));

FAutoConsoleCommandWithWorldAndArgs VNHSpawnTestHumanCommand(
	TEXT("vnh.SpawnTestHuman"),
	TEXT("Debug: spawn a visible shopper test pawn and possess it. Usage: vnh.SpawnTestHuman [X Y Z]"),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic([](const TArray<FString>& Args, UWorld* World)
	{
		AVNHGameMode* GameMode = GetVNHGameMode(World);
		if (!GameMode)
		{
			UE_LOG(LogVNH, Warning, TEXT("vnh.SpawnTestHuman failed: no VNH auth game mode in current world."));
			return;
		}

		FVector SpawnLocation(0.0f, 0.0f, 140.0f);
		if (Args.Num() >= 3)
		{
			SpawnLocation.X = FCString::Atof(*Args[0]);
			SpawnLocation.Y = FCString::Atof(*Args[1]);
			SpawnLocation.Z = FCString::Atof(*Args[2]);
		}

		GameMode->DebugSpawnAndPossessTestShopper(SpawnLocation);
	}));

FAutoConsoleCommandWithWorld VNHSetupTestArenaCommand(
	TEXT("vnh.SetupTestArena"),
	TEXT("Debug: create a visible lit floor for movement testing."),
	FConsoleCommandWithWorldDelegate::CreateStatic([](UWorld* World)
	{
		AVNHGameMode* GameMode = GetVNHGameMode(World);
		if (!GameMode)
		{
			UE_LOG(LogVNH, Warning, TEXT("vnh.SetupTestArena failed: no VNH auth game mode in current world."));
			return;
		}

		GameMode->DebugSetupVisibleTestArena();
	}));

FAutoConsoleCommandWithWorld VNHStartRoutinesCommand(
	TEXT("vnh.StartRoutines"),
	TEXT("Debug: request all non-possessed shoppers to start or resume routine movement."),
	FConsoleCommandWithWorldDelegate::CreateStatic([](UWorld* World)
	{
		AVNHGameMode* GameMode = GetVNHGameMode(World);
		if (!GameMode)
		{
			UE_LOG(LogVNH, Warning, TEXT("vnh.StartRoutines failed: no VNH auth game mode in current world."));
			return;
		}

		GameMode->DebugStartShopperRoutines();
	}));

FAutoConsoleCommandWithWorld VNHInteractCommand(
	TEXT("vnh.Interact"),
	TEXT("Debug: question the currently focused shopper."),
	FConsoleCommandWithWorldDelegate::CreateStatic([](UWorld* World)
	{
		AVNHPlayerController* PlayerController = GetVNHPlayerController(World);
		if (!PlayerController)
		{
			UE_LOG(LogVNH, Warning, TEXT("vnh.Interact failed: no VNH player controller."));
			return;
		}

		PlayerController->RequestInteract();
	}));

FAutoConsoleCommandWithWorld VNHMarkTargetCommand(
	TEXT("vnh.MarkTarget"),
	TEXT("Debug: mark the currently focused shopper as a suspect."),
	FConsoleCommandWithWorldDelegate::CreateStatic([](UWorld* World)
	{
		AVNHPlayerController* PlayerController = GetVNHPlayerController(World);
		if (!PlayerController)
		{
			UE_LOG(LogVNH, Warning, TEXT("vnh.MarkTarget failed: no VNH player controller."));
			return;
		}

		PlayerController->MarkFocusedShopper();
	}));

FAutoConsoleCommandWithWorld VNHFakeAccuseCommand(
	TEXT("vnh.FakeAccuse"),
	TEXT("Debug: fake-accuse the currently focused shopper."),
	FConsoleCommandWithWorldDelegate::CreateStatic([](UWorld* World)
	{
		AVNHPlayerController* PlayerController = GetVNHPlayerController(World);
		if (!PlayerController)
		{
			UE_LOG(LogVNH, Warning, TEXT("vnh.FakeAccuse failed: no VNH player controller."));
			return;
		}

		PlayerController->FakeAccuseFocusedShopper();
	}));

FAutoConsoleCommandWithWorld VNHAccuseTargetCommand(
	TEXT("vnh.AccuseTarget"),
	TEXT("Debug: resolve accusation against the currently focused shopper."),
	FConsoleCommandWithWorldDelegate::CreateStatic([](UWorld* World)
	{
		AVNHPlayerController* PlayerController = GetVNHPlayerController(World);
		if (!PlayerController)
		{
			UE_LOG(LogVNH, Warning, TEXT("vnh.AccuseTarget failed: no VNH player controller."));
			return;
		}

		PlayerController->DebugAccuseFocusedShopper();
	}));

FAutoConsoleCommandWithWorld VNHLogAlienLocomotionCommand(
	TEXT("vnh.LogAlienLocomotion"),
	TEXT("Debug: log the current possessed shopper's alien locomotion state."),
	FConsoleCommandWithWorldDelegate::CreateStatic([](UWorld* World)
	{
		UVNHAlienLocomotionComponent* LocomotionComponent = GetAlienLocomotionComponent(World);
		if (!LocomotionComponent)
		{
			UE_LOG(LogVNH, Warning, TEXT("vnh.LogAlienLocomotion failed: first player is not possessing a shopper with alien locomotion."));
			return;
		}

		UE_LOG(LogVNH, Display, TEXT("vnh.LogAlienLocomotion: %s"), *LocomotionComponent->DescribeLocomotionState());
	}));

FAutoConsoleCommandWithWorld VNHLogAlienInputCommand(
	TEXT("vnh.LogAlienInput"),
	TEXT("Debug: log the first local player's alien input binding/counter state."),
	FConsoleCommandWithWorldDelegate::CreateStatic([](UWorld* World)
	{
		AVNHPlayerController* PlayerController = GetVNHPlayerController(World);
		if (!PlayerController)
		{
			APlayerController* RawPlayerController = World ? World->GetFirstPlayerController() : nullptr;
			UE_LOG(LogVNH, Warning, TEXT("vnh.LogAlienInput failed: first player controller is %s, not AVNHPlayerController."),
				RawPlayerController ? *RawPlayerController->GetClass()->GetName() : TEXT("None"));
			return;
		}

		UE_LOG(LogVNH, Display, TEXT("vnh.LogAlienInput: %s"), *PlayerController->DescribeAlienInputDebugState());
	}));

FAutoConsoleCommandWithWorldAndArgs VNHSetAlienMoveCommand(
	TEXT("vnh.SetAlienMove"),
	TEXT("Debug: set alien movement input directly. Usage: vnh.SetAlienMove X Y, e.g. vnh.SetAlienMove 0 1"),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic([](const TArray<FString>& Args, UWorld* World)
	{
		UVNHAlienLocomotionComponent* LocomotionComponent = GetAlienLocomotionComponent(World);
		if (!LocomotionComponent)
		{
			UE_LOG(LogVNH, Warning, TEXT("vnh.SetAlienMove failed: first player is not possessing a shopper with alien locomotion."));
			return;
		}

		const float X = Args.IsValidIndex(0) ? FCString::Atof(*Args[0]) : 0.0f;
		const float Y = Args.IsValidIndex(1) ? FCString::Atof(*Args[1]) : 0.0f;
		LocomotionComponent->SetMoveInput(FVector2D(X, Y));
		UE_LOG(LogVNH, Display, TEXT("vnh.SetAlienMove: X=%.2f Y=%.2f | %s"), X, Y, *LocomotionComponent->DescribeLocomotionState());
	}));

FAutoConsoleCommandWithWorld VNHClearAlienMoveCommand(
	TEXT("vnh.ClearAlienMove"),
	TEXT("Debug: clear alien movement input directly."),
	FConsoleCommandWithWorldDelegate::CreateStatic([](UWorld* World)
	{
		UVNHAlienLocomotionComponent* LocomotionComponent = GetAlienLocomotionComponent(World);
		if (!LocomotionComponent)
		{
			UE_LOG(LogVNH, Warning, TEXT("vnh.ClearAlienMove failed: first player is not possessing a shopper with alien locomotion."));
			return;
		}

		LocomotionComponent->ClearInput();
		UE_LOG(LogVNH, Display, TEXT("vnh.ClearAlienMove: %s"), *LocomotionComponent->DescribeLocomotionState());
	}));

FAutoConsoleCommandWithWorldAndArgs VNHSetAlienFastWalkCommand(
	TEXT("vnh.SetAlienFastWalk"),
	TEXT("Debug: set alien fast-walk input directly. Usage: vnh.SetAlienFastWalk 0|1"),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic([](const TArray<FString>& Args, UWorld* World)
	{
		UVNHAlienLocomotionComponent* LocomotionComponent = GetAlienLocomotionComponent(World);
		if (!LocomotionComponent)
		{
			UE_LOG(LogVNH, Warning, TEXT("vnh.SetAlienFastWalk failed: first player is not possessing a shopper with alien locomotion."));
			return;
		}

		const bool bFastWalk = Args.IsValidIndex(0) && FCString::Atoi(*Args[0]) != 0;
		LocomotionComponent->SetFastWalkRequested(bFastWalk);
		UE_LOG(LogVNH, Display, TEXT("vnh.SetAlienFastWalk: %s | %s"), bFastWalk ? TEXT("true") : TEXT("false"), *LocomotionComponent->DescribeLocomotionState());
	}));
}
