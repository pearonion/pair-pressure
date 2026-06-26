#pragma once

#include "CoreMinimal.h"
#include "GameFramework/PlayerController.h"
#include "VNHGameplayTypes.h"
#include "VNHPlayerController.generated.h"

class AVNHShopperCharacter;

UCLASS()
class VNHSIMULATOR_API AVNHPlayerController : public APlayerController
{
	GENERATED_BODY()

public:
	UFUNCTION(BlueprintCallable, Category = "VNH|Hunter")
	void RequestPublicTest(EVNHPublicTestType TestType);

	UFUNCTION(BlueprintCallable, Category = "VNH|Hunter")
	void RequestAccusation(AVNHShopperCharacter* AccusedShopper);

	UFUNCTION(BlueprintCallable, Category = "VNH|Alien")
	void RequestActNatural();

	UFUNCTION(Server, Reliable)
	void ServerRequestPublicTest(EVNHPublicTestType TestType);

	UFUNCTION(Server, Reliable)
	void ServerRequestAccusation(AVNHShopperCharacter* AccusedShopper);

	UFUNCTION(Server, Reliable)
	void ServerRequestActNatural();
};
