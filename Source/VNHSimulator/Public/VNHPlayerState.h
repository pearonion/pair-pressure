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
	EVNHPlayerRole GetRole() const { return AssignedRole; }

	UFUNCTION(BlueprintPure, Category = "VNH|Player")
	bool IsAlien() const { return AssignedRole == EVNHPlayerRole::Alien; }

	UFUNCTION(BlueprintPure, Category = "VNH|Player")
	bool IsHuman() const { return AssignedRole == EVNHPlayerRole::Human; }

	UFUNCTION(BlueprintPure, Category = "VNH|Player")
	bool IsHunter() const { return AssignedRole == EVNHPlayerRole::Hunter; }

	UFUNCTION(BlueprintPure, Category = "VNH|Player")
	FText GetRoleDisplayText() const;

	UFUNCTION(BlueprintPure, Category = "VNH|Player")
	FText GetPrivateRoleRevealText() const;

	UFUNCTION(BlueprintPure, Category = "VNH|Player")
	FText GetRoleGoalText() const;

	UFUNCTION(BlueprintPure, Category = "VNH|Player")
	FText GetLightErrandText() const { return LightErrandText; }

	void SetRole(EVNHPlayerRole NewRole);
	void SetLightErrandText(const FText& NewLightErrandText);

private:
	UPROPERTY(ReplicatedUsing = OnRep_AssignedRole, BlueprintReadOnly, Category = "VNH|Player", meta = (AllowPrivateAccess = "true"))
	EVNHPlayerRole AssignedRole = EVNHPlayerRole::Unassigned;

	UPROPERTY(Replicated, BlueprintReadOnly, Category = "VNH|Player", meta = (AllowPrivateAccess = "true"))
	FText LightErrandText;

	UFUNCTION()
	void OnRep_AssignedRole();
};
