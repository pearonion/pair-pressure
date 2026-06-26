#pragma once

#include "CoreMinimal.h"
#include "GameFramework/PlayerState.h"
#include "VNHGameplayTypes.h"
#include "VNHPlayerState.generated.h"

UCLASS()
class VNHSIMULATOR_API AVNHPlayerState : public APlayerState
{
	GENERATED_BODY()

public:
	AVNHPlayerState();

	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	UFUNCTION(BlueprintPure, Category = "VNH|Player")
	EVNHPlayerRole GetRole() const { return Role; }

	UFUNCTION(BlueprintPure, Category = "VNH|Player")
	bool IsAlien() const { return Role == EVNHPlayerRole::Alien; }

	UFUNCTION(BlueprintPure, Category = "VNH|Player")
	bool IsHunter() const { return Role == EVNHPlayerRole::Hunter; }

	void SetRole(EVNHPlayerRole NewRole);

private:
	UPROPERTY(ReplicatedUsing = OnRep_Role, BlueprintReadOnly, Category = "VNH|Player", meta = (AllowPrivateAccess = "true"))
	EVNHPlayerRole Role = EVNHPlayerRole::Unassigned;

	UFUNCTION()
	void OnRep_Role();
};
