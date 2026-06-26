#pragma once

#include "CoreMinimal.h"
#include "GameFramework/GameStateBase.h"
#include "VNHGameplayTypes.h"
#include "VNHGameState.generated.h"

UCLASS()
class VNHSIMULATOR_API AVNHGameState : public AGameStateBase
{
	GENERATED_BODY()

public:
	AVNHGameState();

	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	UFUNCTION(BlueprintPure, Category = "VNH|Round")
	EVNHRoundPhase GetRoundPhase() const { return RoundPhase; }

	UFUNCTION(BlueprintPure, Category = "VNH|Round")
	float GetPhaseEndsAtServerTime() const { return PhaseEndsAtServerTime; }

	UFUNCTION(BlueprintPure, Category = "VNH|Round")
	int32 GetRoundNumber() const { return RoundNumber; }

	UFUNCTION(BlueprintPure, Category = "VNH|Round")
	int32 GetTestsRemaining() const { return TestsRemaining; }

	void SetRoundPhase(EVNHRoundPhase NewPhase, float NewPhaseEndsAtServerTime);
	void SetRoundNumber(int32 NewRoundNumber);
	void SetTestsRemaining(int32 NewTestsRemaining);

private:
	UPROPERTY(ReplicatedUsing = OnRep_RoundPhase, BlueprintReadOnly, Category = "VNH|Round", meta = (AllowPrivateAccess = "true"))
	EVNHRoundPhase RoundPhase = EVNHRoundPhase::WaitingForPlayers;

	UPROPERTY(Replicated, BlueprintReadOnly, Category = "VNH|Round", meta = (AllowPrivateAccess = "true"))
	float PhaseEndsAtServerTime = 0.0f;

	UPROPERTY(Replicated, BlueprintReadOnly, Category = "VNH|Round", meta = (AllowPrivateAccess = "true"))
	int32 RoundNumber = 0;

	UPROPERTY(Replicated, BlueprintReadOnly, Category = "VNH|Round", meta = (AllowPrivateAccess = "true"))
	int32 TestsRemaining = 2;

	UFUNCTION()
	void OnRep_RoundPhase();
};
