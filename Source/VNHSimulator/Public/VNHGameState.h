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

	UFUNCTION(BlueprintPure, Category = "VNH|Round")
	EVNHPublicTestType GetActivePublicTest() const { return ActivePublicTest; }

	UFUNCTION(BlueprintPure, Category = "VNH|Round")
	FVNHAccusationResult GetAccusationResult() const { return AccusationResult; }

	void SetRoundPhase(EVNHRoundPhase NewPhase, float NewPhaseEndsAtServerTime);
	void SetRoundNumber(int32 NewRoundNumber);
	void SetTestsRemaining(int32 NewTestsRemaining);
	void SetActivePublicTest(EVNHPublicTestType NewActivePublicTest);
	void SetAccusationResult(const FVNHAccusationResult& NewAccusationResult);
	void ClearRoundOutcome();

private:
	UPROPERTY(ReplicatedUsing = OnRep_RoundPhase, BlueprintReadOnly, Category = "VNH|Round", meta = (AllowPrivateAccess = "true"))
	EVNHRoundPhase RoundPhase = EVNHRoundPhase::WaitingForPlayers;

	UPROPERTY(Replicated, BlueprintReadOnly, Category = "VNH|Round", meta = (AllowPrivateAccess = "true"))
	float PhaseEndsAtServerTime = 0.0f;

	UPROPERTY(Replicated, BlueprintReadOnly, Category = "VNH|Round", meta = (AllowPrivateAccess = "true"))
	int32 RoundNumber = 0;

	UPROPERTY(Replicated, BlueprintReadOnly, Category = "VNH|Round", meta = (AllowPrivateAccess = "true"))
	int32 TestsRemaining = 2;

	UPROPERTY(ReplicatedUsing = OnRep_ActivePublicTest, BlueprintReadOnly, Category = "VNH|Round", meta = (AllowPrivateAccess = "true"))
	EVNHPublicTestType ActivePublicTest = EVNHPublicTestType::Freeze;

	UPROPERTY(ReplicatedUsing = OnRep_AccusationResult, BlueprintReadOnly, Category = "VNH|Round", meta = (AllowPrivateAccess = "true"))
	FVNHAccusationResult AccusationResult;

	UFUNCTION()
	void OnRep_RoundPhase();

	UFUNCTION()
	void OnRep_ActivePublicTest();

	UFUNCTION()
	void OnRep_AccusationResult();
};
