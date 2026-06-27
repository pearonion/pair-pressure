#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Character.h"
#include "TimerManager.h"
#include "VNHRoutineComponent.h"
#include "VNHShopperCharacter.generated.h"

class UVNHAlienLocomotionComponent;
class UCameraComponent;
class USpringArmComponent;
class UStaticMeshComponent;

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FVNHActNaturalUsed, EVNHActNaturalRecovery, Recovery);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FVNHPublicTestReceived, EVNHPublicTestType, TestType);

UCLASS()
class VNHSIMULATOR_API AVNHShopperCharacter : public ACharacter
{
	GENERATED_BODY()

public:
	AVNHShopperCharacter();

	virtual void BeginPlay() override;
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	UFUNCTION(BlueprintPure, Category = "VNH|Shopper")
	UVNHRoutineComponent* GetRoutineComponent() const { return RoutineComponent; }

	UFUNCTION(BlueprintPure, Category = "VNH|Shopper")
	UVNHAlienLocomotionComponent* GetAlienLocomotionComponent() const { return AlienLocomotionComponent; }

	UFUNCTION(BlueprintPure, Category = "VNH|Shopper")
	USpringArmComponent* GetFollowCameraBoom() const { return FollowCameraBoom; }

	UFUNCTION(BlueprintPure, Category = "VNH|Shopper")
	UCameraComponent* GetFollowCamera() const { return FollowCamera; }

	UFUNCTION(BlueprintPure, Category = "VNH|Shopper")
	bool IsPossessedByAlien() const { return bPossessedByAlien; }

	UFUNCTION(BlueprintPure, Category = "VNH|Shopper")
	bool HasActNaturalAvailable() const { return bActNaturalAvailable; }

	UFUNCTION(BlueprintPure, Category = "VNH|Shopper")
	bool IsFrozenByPublicTest() const { return bFrozenByPublicTest; }

	UFUNCTION(BlueprintPure, Category = "VNH|Shopper")
	TSoftObjectPtr<UObject> GetShopperStateTree() const { return ShopperStateTree; }

	UFUNCTION(BlueprintCallable, Category = "VNH|Shopper")
	void SetPossessedByAlien(bool bNewPossessedByAlien);

	UFUNCTION(BlueprintCallable, Category = "VNH|Shopper")
	bool UseActNatural();

	UFUNCTION(BlueprintCallable, Category = "VNH|Shopper")
	void ApplyPublicTest(EVNHPublicTestType TestType);

	UFUNCTION(BlueprintCallable, Category = "VNH|Shopper")
	void StartRoutineMovement();

	UFUNCTION(BlueprintPure, Category = "VNH|Shopper|Interaction")
	FString BuildQuestionResponse() const;

	UPROPERTY(BlueprintAssignable, Category = "VNH|Shopper")
	FVNHActNaturalUsed OnActNaturalUsed;

	UPROPERTY(BlueprintAssignable, Category = "VNH|Shopper")
	FVNHPublicTestReceived OnPublicTestReceived;

private:
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "VNH|Shopper", meta = (AllowPrivateAccess = "true"))
	TObjectPtr<UVNHRoutineComponent> RoutineComponent;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "VNH|Shopper", meta = (AllowPrivateAccess = "true"))
	TObjectPtr<UVNHAlienLocomotionComponent> AlienLocomotionComponent;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "VNH|Shopper", meta = (AllowPrivateAccess = "true"))
	TObjectPtr<USpringArmComponent> FollowCameraBoom;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "VNH|Shopper", meta = (AllowPrivateAccess = "true"))
	TObjectPtr<UCameraComponent> FollowCamera;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "VNH|Shopper", meta = (AllowPrivateAccess = "true"))
	TObjectPtr<UStaticMeshComponent> DebugBodyMesh;

	UPROPERTY(ReplicatedUsing = OnRep_PossessedByAlien, BlueprintReadOnly, Category = "VNH|Shopper", meta = (AllowPrivateAccess = "true"))
	bool bPossessedByAlien = false;

	UPROPERTY(Replicated, BlueprintReadOnly, Category = "VNH|Shopper", meta = (AllowPrivateAccess = "true"))
	bool bActNaturalAvailable = true;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "VNH|Shopper|Public Tests", meta = (AllowPrivateAccess = "true", ClampMin = "0.0"))
	float FreezeTestHoldSeconds = 4.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "VNH|Shopper|AI", meta = (AllowPrivateAccess = "true", AllowedClasses = "/Script/StateTreeModule.StateTree"))
	TSoftObjectPtr<UObject> ShopperStateTree = TSoftObjectPtr<UObject>(FSoftObjectPath(TEXT("/Game/AI/ST_Shoppers.ST_Shoppers")));

	UPROPERTY(ReplicatedUsing = OnRep_FrozenByPublicTest, BlueprintReadOnly, Category = "VNH|Shopper|Public Tests", meta = (AllowPrivateAccess = "true"))
	bool bFrozenByPublicTest = false;

	FTimerHandle FreezeTestTimerHandle;
	FTimerHandle PublicTestReactionTimerHandle;

	UFUNCTION()
	void OnRep_PossessedByAlien();

	UFUNCTION()
	void OnRep_FrozenByPublicTest();

	void SetFrozenByPublicTest(bool bNewFrozen);
	void ClearPublicTestFreeze();
	void ApplyFrozenVisualState();
	void ApplyLookToEntranceReaction();
	void ApplyClearAisleReaction();
	void ResumeRoutineMovement();
};
