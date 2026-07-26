#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "TTTMatchHUDStyleData.generated.h"

class UFont;

UCLASS(BlueprintType)
class VNHSIMULATOR_API UTTTMatchHUDStyleData : public UDataAsset
{
	GENERATED_BODY()

public:
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Palette") FLinearColor SurfaceColor = FLinearColor(0.018f, 0.032f, 0.06f, 0.88f);
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Palette") FLinearColor CreamTextColor = FLinearColor(0.96f, 0.92f, 0.82f, 1.0f);
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Palette") FLinearColor EmphasisColor = FLinearColor(1.0f, 0.42f, 0.06f, 1.0f);
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Palette") FLinearColor MutedTextColor = FLinearColor(0.62f, 0.68f, 0.76f, 1.0f);
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Shape", meta = (ClampMin = "0.0")) float CornerRadius = 6.0f;
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Shape", meta = (ClampMin = "0.0")) float BorderThickness = 1.0f;
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Shape", meta = (ClampMin = "0.0")) float ContentPadding = 12.0f;
};
