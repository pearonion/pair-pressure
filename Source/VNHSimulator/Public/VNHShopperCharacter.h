#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Character.h"
#include "VNHRoutineComponent.h"
#include "VNHShopperCharacter.generated.h"

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FVNHActNaturalUsed, EVNHActNaturalRecovery, Recovery);

UCLASS()
class VNHSIMULATOR_API AVNHShopperCharacter : public ACharacter
{
	GENERATED_BODY()

public:
	AVNHShopperCharacter();

	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	UFUNCTION(BlueprintPure, Category = "VNH|Shopper")
	UVNHRoutineComponent* GetRoutineComponent() const { return RoutineComponent; }

	UFUNCTION(BlueprintPure, Category = "VNH|Shopper")
	bool IsPossessedByAlien() const { return bPossessedByAlien; }

	UFUNCTION(BlueprintPure, Category = "VNH|Shopper")
	bool HasActNaturalAvailable() const { return bActNaturalAvailable; }

	UFUNCTION(BlueprintCallable, Category = "VNH|Shopper")
	void SetPossessedByAlien(bool bNewPossessedByAlien);

	UFUNCTION(BlueprintCallable, Category = "VNH|Shopper")
	bool UseActNatural();

	UPROPERTY(BlueprintAssignable, Category = "VNH|Shopper")
	FVNHActNaturalUsed OnActNaturalUsed;

private:
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "VNH|Shopper", meta = (AllowPrivateAccess = "true"))
	TObjectPtr<UVNHRoutineComponent> RoutineComponent;

	UPROPERTY(ReplicatedUsing = OnRep_PossessedByAlien, BlueprintReadOnly, Category = "VNH|Shopper", meta = (AllowPrivateAccess = "true"))
	bool bPossessedByAlien = false;

	UPROPERTY(Replicated, BlueprintReadOnly, Category = "VNH|Shopper", meta = (AllowPrivateAccess = "true"))
	bool bActNaturalAvailable = true;

	UFUNCTION()
	void OnRep_PossessedByAlien();
};
