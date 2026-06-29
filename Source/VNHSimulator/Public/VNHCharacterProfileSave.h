#pragma once

#include "CoreMinimal.h"
#include "GameFramework/SaveGame.h"
#include "VNHGameplayTypes.h"
#include "VNHCharacterProfileSave.generated.h"

UCLASS()
class VNHSIMULATOR_API UVNHCharacterProfileSave : public USaveGame
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintReadWrite, SaveGame, Category = "VNH|Customization")
	int32 ActivePresetIndex = 0;

	UPROPERTY(BlueprintReadWrite, SaveGame, Category = "VNH|Customization")
	TArray<FVNHCharacterCustomization> Presets;

	UPROPERTY(BlueprintReadWrite, SaveGame, Category = "VNH|Stats")
	int32 AlienWins = 0;

	UPROPERTY(BlueprintReadWrite, SaveGame, Category = "VNH|Stats")
	int32 HunterWins = 0;

	UPROPERTY(BlueprintReadWrite, SaveGame, Category = "VNH|Stats")
	int32 Accusations = 0;

	UPROPERTY(BlueprintReadWrite, SaveGame, Category = "VNH|Stats")
	int32 CorrectAccusations = 0;
};
