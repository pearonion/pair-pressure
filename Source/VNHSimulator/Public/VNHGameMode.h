#pragma once

#include "CoreMinimal.h"
#include "GameFramework/GameModeBase.h"
#include "VNHGameplayTypes.h"
#include "VNHGameMode.generated.h"

class AVNHGameState;
class AVNHPlayerController;
class AVNHPlayerState;
class AVNHShopperCharacter;

UCLASS()
class VNHSIMULATOR_API AVNHGameMode : public AGameModeBase
{
	GENERATED_BODY()

public:
	AVNHGameMode();

	virtual void InitGame(const FString& MapName, const FString& Options, FString& ErrorMessage) override;
	virtual void BeginPlay() override;
	virtual void PostLogin(APlayerController* NewPlayer) override;
	virtual void Logout(AController* Exiting) override;

	UFUNCTION(BlueprintCallable, Category = "VNH|Round")
	void TryStartRound();

	UFUNCTION(BlueprintCallable, Category = "VNH|Lobby")
	bool StartRoundFromLobby(APlayerController* RequestingPlayer);

	UFUNCTION(BlueprintCallable, Category = "VNH|Round")
	void AdvanceRoundPhase();

	void RequestPublicTest(AVNHPlayerController* RequestingPlayer, EVNHPublicTestType TestType);
	bool RequestDirectQuestion(AVNHPlayerController* RequestingPlayer, AVNHShopperCharacter* QuestionedShopper, FString& OutResponseText);
	bool RequestQuestion(AVNHPlayerController* RequestingPlayer, AVNHShopperCharacter* QuestionedShopper);
	void RequestAccusation(AVNHPlayerController* RequestingPlayer, AVNHShopperCharacter* AccusedShopper);

	void DebugStartRound();
	void DebugForceRole(APlayerController* TargetController, EVNHPlayerRole NewRole);
	void DebugTriggerPublicTest(EVNHPublicTestType TestType);
	void DebugJumpToInvestigation();
	bool DebugPossessShopperByIndex(int32 ShopperIndex, EVNHPlayerRole ForcedRole = EVNHPlayerRole::Alien);
	bool DebugResolveAccusation(AVNHShopperCharacter* AccusedShopper);
	bool DebugSetupVisibleTestArena();
	bool DebugSpawnAndPossessTestShopper(const FVector& SpawnLocation);
	void DebugStartShopperRoutines();

protected:
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "VNH|Round")
	int32 RequiredPlayers = 3;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "VNH|Lobby")
	bool bAutoStartRoundOnPlayerJoin = false;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "VNH|Round")
	int32 PublicTestsPerRound = 2;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "VNH|Round")
	int32 QuestionsPerRound = 3;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "VNH|Round")
	FVNHPhaseTiming PhaseTiming;

private:
	FTimerHandle PhaseTimerHandle;
	bool bStartRoundWhenPlayersReady = false;

	void ConfigureRuntimeClasses();
	AVNHGameState* GetVNHGameState() const;
	void EnsureLobbyRuntimeActors();
	void EnsureMvpInteractionProps();
	void AssignRoles();
	void EnterPhase(EVNHRoundPhase NewPhase, float DurationSeconds);
	int32 CountConnectedPlayers() const;
	bool IsHunterController(const APlayerController* PlayerController) const;
	bool IsHostController(const APlayerController* PlayerController) const;
	APlayerController* FindControllerForRole(EVNHPlayerRole TargetRole) const;
	AVNHShopperCharacter* SelectAlienShopper() const;
	void ResetShopperPossessionState();
	void PossessAlienShopper();
	void StartShopperRoutines();
	void ApplyPublicTestToShoppers(EVNHPublicTestType TestType);
};
