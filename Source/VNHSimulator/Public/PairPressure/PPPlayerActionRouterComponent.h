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
	void NotifyJumpStarted();

	UFUNCTION(BlueprintCallable, Category = "Pair Pressure|Actions")
	void NotifyLanded();

	UFUNCTION(BlueprintPure, Category = "Pair Pressure|Actions")
	bool CanAirDive() const;

	UFUNCTION(BlueprintCallable, Category = "Pair Pressure|Actions")
	void BeginAssist();

	UFUNCTION(BlueprintCallable, Category = "Pair Pressure|Actions")
	void EndAssist();

private:
	UFUNCTION(Server, Reliable)
	void ServerNotifyJumpStarted();

	UFUNCTION(Server, Reliable)
	void ServerRequestDive();

	void ArmAirDive();
	void PerformDiveAuthoritative();

	bool bAirDiveArmed = false;
};
