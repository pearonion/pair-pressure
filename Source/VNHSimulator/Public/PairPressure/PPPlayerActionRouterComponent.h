#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "PPPlayerActionRouterComponent.generated.h"

UCLASS(ClassGroup = (PairPressure), BlueprintType, Blueprintable, meta = (BlueprintSpawnableComponent))
class VNHSIMULATOR_API UPPPlayerActionRouterComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UPPPlayerActionRouterComponent();

	UFUNCTION(BlueprintCallable, Category = "Pair Pressure|Actions")
	void RequestDive();

	UFUNCTION(BlueprintCallable, Category = "Pair Pressure|Actions")
	void BeginAssist();

	UFUNCTION(BlueprintCallable, Category = "Pair Pressure|Actions")
	void EndAssist();
};
