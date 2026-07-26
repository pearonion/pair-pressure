#pragma once

#include "CoreMinimal.h"
#include "UObject/Interface.h"
#include "TwoToTangle/UI/Data/TTTMatchHUDTypes.h"
#include "TTTThrowableItemInterface.generated.h"

UINTERFACE(BlueprintType)
class VNHSIMULATOR_API UTTTThrowableItemInterface : public UInterface
{
	GENERATED_BODY()
};
class VNHSIMULATOR_API ITTTThrowableItemInterface
{
	GENERATED_BODY()

public:
	UFUNCTION(BlueprintNativeEvent, BlueprintCallable, Category = "Two to Tangle|Item") FTTTHeldItemPresentation GetHeldItemPresentation() const;
	UFUNCTION(BlueprintNativeEvent, BlueprintCallable, Category = "Two to Tangle|Item") float GetMinimumChargeTime() const;
	UFUNCTION(BlueprintNativeEvent, BlueprintCallable, Category = "Two to Tangle|Item") float GetMaximumChargeTime() const;
};
