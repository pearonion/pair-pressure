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

	virtual void BeginPlay() override;
	virtual void PostLogin(APlayerController* NewPlayer) override;
	virtual void Logout(AController* Exiting) override;

	UFUNCTION(BlueprintCallable, Category = "VNH|Round")
	void TryStartRound();

	UFUNCTION(BlueprintCallable, Category = "VNH|Round")
	void AdvanceRoundPhase();

	void RequestPublicTest(AVNHPlayerController* RequestingPlayer, EVNHPublicTestType TestType);
	void RequestAccusation(AVNHPlayerController* RequestingPlayer, AVNHShopperCharacter* AccusedShopper);

protected:
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "VNH|Round")
	int32 RequiredPlayers = 2;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "VNH|Round")
	FVNHPhaseTiming PhaseTiming;

private:
	FTimerHandle PhaseTimerHandle;

	AVNHGameState* GetVNHGameState() const;
	void AssignRoles();
	void EnterPhase(EVNHRoundPhase NewPhase, float DurationSeconds);
	int32 CountConnectedPlayers() const;
	bool IsHunterController(const APlayerController* PlayerController) const;
	APlayerController* FindControllerForRole(EVNHPlayerRole Role) const;
	AVNHShopperCharacter* SelectAlienShopper() const;
	void PossessAlienShopper();
	void ApplyPublicTestToShoppers(EVNHPublicTestType TestType);
};
