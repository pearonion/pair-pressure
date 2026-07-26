#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "TTTMatchHUDConfig.generated.h"

class UTTTMatchHUDWidget;
class UTTTMatchTransitionWidget;
class USoundBase;

UCLASS(BlueprintType)
class VNHSIMULATOR_API UTTTMatchHUDConfig : public UDataAsset
{
	GENERATED_BODY()

public:
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Race", meta = (ClampMin = "1.0")) float CountdownDuration = 3.0f;
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Race", meta = (ClampMin = "0.0")) float GoDisplayDuration = 0.7f;
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Results", meta = (ClampMin = "0.1")) float ResultDisplayDuration = 5.0f;
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Results", meta = (ClampMin = "0.0")) float ResultFadeDuration = 0.5f;
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Timing", meta = (ClampMin = "1.0", ClampMax = "60.0")) float StopwatchRefreshRate = 20.0f;
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Daze", meta = (ClampMin = "0.0", ClampMax = "1.0")) float DazeMeterShowThreshold = 0.05f;
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Daze", meta = (ClampMin = "0.0")) float DazeMeterHideDelay = 2.5f;
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Daze", meta = (ClampMin = "0.0", ClampMax = "1.0")) float DazeRecoveryThreshold = 0.3f;
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Daze", meta = (ClampMin = "0.0")) float CarriedDazeRecoveryRate = 20.0f;
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Status", meta = (ClampMin = "0.0")) float FullyDazedMessageDuration = 2.5f;
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Status", meta = (ClampMin = "0.0")) float StatusMessageFadeDuration = 0.2f;
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Animation", meta = (ClampMin = "0.0")) float PlacementAnimationDuration = 0.2f;
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Animation", meta = (ClampMin = "0.0")) float ThrowBarFadeDuration = 0.1f;
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Classes") TSubclassOf<UTTTMatchHUDWidget> MatchHUDClass;
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Classes") TSubclassOf<UTTTMatchTransitionWidget> DummyMatchOverClass;
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Audio") TSoftObjectPtr<USoundBase> CountdownSound;
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Audio") TSoftObjectPtr<USoundBase> GoSound;
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Audio") TSoftObjectPtr<USoundBase> RecoverySound;
};
