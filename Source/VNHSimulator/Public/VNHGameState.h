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
	int32 GetDirectQuestionsRemaining() const { return DirectQuestionsRemaining; }

	UFUNCTION(BlueprintPure, Category = "VNH|Round")
	int32 GetAccusationsRemaining() const { return AccusationsRemaining; }

	UFUNCTION(BlueprintPure, Category = "VNH|Round")
	int32 GetQuestionsRemaining() const { return DirectQuestionsRemaining; }

	UFUNCTION(BlueprintPure, Category = "VNH|Round")
	EVNHPublicTestType GetActivePublicTest() const { return ActivePublicTest; }

	UFUNCTION(BlueprintPure, Category = "VNH|Round")
	FVNHAccusationResult GetAccusationResult() const { return AccusationResult; }

	UFUNCTION(BlueprintPure, Category = "VNH|Round")
	FText GetRevealSummaryText() const;

	UFUNCTION(BlueprintPure, Category = "VNH|Round")
	FText GetHunterToolsText() const;

	UFUNCTION(BlueprintPure, Category = "VNH|Quick Chat")
	FVNHQuickChatMessage GetLastQuickChatMessage() const { return LastQuickChatMessage; }

	UFUNCTION(BlueprintPure, Category = "VNH|Human Drill")
	FVNHHumanDrillPrompt GetHumanDrillPrompt() const { return HumanDrillPrompt; }

	UFUNCTION(BlueprintPure, Category = "VNH|Round")
	AActor* GetPossessedShopper() const { return PossessedShopper; }

	void SetRoundPhase(EVNHRoundPhase NewPhase, float NewPhaseEndsAtServerTime);
	void SetRoundNumber(int32 NewRoundNumber);
	void SetTestsRemaining(int32 NewTestsRemaining);
	void SetDirectQuestionsRemaining(int32 NewDirectQuestionsRemaining);
	void SetAccusationsRemaining(int32 NewAccusationsRemaining);
	void SetQuestionsRemaining(int32 NewQuestionsRemaining);
	void SetActivePublicTest(EVNHPublicTestType NewActivePublicTest);
	void SetAccusationResult(const FVNHAccusationResult& NewAccusationResult);
	void SetPossessedShopper(AActor* NewPossessedShopper);
	void SetHumanDrillPrompt(EVNHHumanDrillAction Action, float PromptEndsAtServerTime, float CooldownEndsAtServerTime);
	void PublishQuickChat(APlayerState* Speaker, EVNHQuickChatLine Line);
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

	UPROPERTY(Replicated, BlueprintReadOnly, Category = "VNH|Round", meta = (AllowPrivateAccess = "true"))
	int32 DirectQuestionsRemaining = 3;

	UPROPERTY(Replicated, BlueprintReadOnly, Category = "VNH|Round", meta = (AllowPrivateAccess = "true"))
	int32 AccusationsRemaining = 1;

	UPROPERTY(ReplicatedUsing = OnRep_ActivePublicTest, BlueprintReadOnly, Category = "VNH|Round", meta = (AllowPrivateAccess = "true"))
	EVNHPublicTestType ActivePublicTest = EVNHPublicTestType::Freeze;

	UPROPERTY(ReplicatedUsing = OnRep_AccusationResult, BlueprintReadOnly, Category = "VNH|Round", meta = (AllowPrivateAccess = "true"))
	FVNHAccusationResult AccusationResult;

	UPROPERTY(ReplicatedUsing = OnRep_PossessedShopper, BlueprintReadOnly, Category = "VNH|Round", meta = (AllowPrivateAccess = "true"))
	TObjectPtr<AActor> PossessedShopper = nullptr;

	UPROPERTY(ReplicatedUsing = OnRep_LastQuickChatMessage, BlueprintReadOnly, Category = "VNH|Quick Chat", meta = (AllowPrivateAccess = "true"))
	FVNHQuickChatMessage LastQuickChatMessage;

	UPROPERTY(ReplicatedUsing = OnRep_HumanDrillPrompt, BlueprintReadOnly, Category = "VNH|Human Drill", meta = (AllowPrivateAccess = "true"))
	FVNHHumanDrillPrompt HumanDrillPrompt;

	UFUNCTION()
	void OnRep_RoundPhase();

	UFUNCTION()
	void OnRep_ActivePublicTest();

	UFUNCTION()
	void OnRep_AccusationResult();

	UFUNCTION()
	void OnRep_PossessedShopper();

	UFUNCTION()
	void OnRep_LastQuickChatMessage();

	UFUNCTION()
	void OnRep_HumanDrillPrompt();
};
