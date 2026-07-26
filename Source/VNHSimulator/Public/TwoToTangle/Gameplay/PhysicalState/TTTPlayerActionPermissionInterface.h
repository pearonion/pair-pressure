#pragma once

#include "CoreMinimal.h"
#include "UObject/Interface.h"
#include "TTTPlayerActionPermissionInterface.generated.h"

UINTERFACE(BlueprintType)
class VNHSIMULATOR_API UTTTPlayerActionPermissionInterface : public UInterface
{
	GENERATED_BODY()
};

class VNHSIMULATOR_API ITTTPlayerActionPermissionInterface
{
	GENERATED_BODY()

public:
	UFUNCTION(BlueprintNativeEvent, BlueprintCallable, Category = "Two to Tangle|Actions") bool CanMove() const;
	UFUNCTION(BlueprintNativeEvent, BlueprintCallable, Category = "Two to Tangle|Actions") bool CanJump() const;
	UFUNCTION(BlueprintNativeEvent, BlueprintCallable, Category = "Two to Tangle|Actions") bool CanDive() const;
	UFUNCTION(BlueprintNativeEvent, BlueprintCallable, Category = "Two to Tangle|Actions") bool CanGrab() const;
	UFUNCTION(BlueprintNativeEvent, BlueprintCallable, Category = "Two to Tangle|Actions") bool CanUseItem() const;
	UFUNCTION(BlueprintNativeEvent, BlueprintCallable, Category = "Two to Tangle|Actions") bool CanDismountCarry() const;
};
