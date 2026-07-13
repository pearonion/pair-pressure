#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "PPCarryComponent.generated.h"

class UPPPhysicalStateComponent;

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FPPAssistTargetChanged, AActor*, NewAssistTarget);

UCLASS(ClassGroup = (PairPressure), BlueprintType, Blueprintable, meta = (BlueprintSpawnableComponent))
class VNHSIMULATOR_API UPPCarryComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UPPCarryComponent();

	virtual void BeginPlay() override;
	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	UFUNCTION(BlueprintCallable, Category = "Pair Pressure|Carry")
	void BeginAssist();

	UFUNCTION(BlueprintCallable, Category = "Pair Pressure|Carry")
	void EndAssist();

	UFUNCTION(BlueprintPure, Category = "Pair Pressure|Carry")
	AActor* GetAssistTarget() const { return AssistTarget; }

	UPROPERTY(BlueprintAssignable, Category = "Pair Pressure|Carry")
	FPPAssistTargetChanged OnAssistTargetChanged;

private:
	UFUNCTION(Server, Reliable)
	void ServerSetAssistActive(bool bNewAssistActive);

	UFUNCTION()
	void OnRep_AssistTarget();

	AActor* FindBestAssistTarget() const;
	void SetAssistTargetAuthoritative(AActor* NewAssistTarget);
	void UpdateAssist(float DeltaTime);

	UPROPERTY(ReplicatedUsing = OnRep_AssistTarget, VisibleInstanceOnly, Category = "Pair Pressure|Carry")
	TObjectPtr<AActor> AssistTarget = nullptr;

	UPROPERTY(Replicated, VisibleInstanceOnly, Category = "Pair Pressure|Carry")
	bool bAssistActive = false;

	UPROPERTY(EditDefaultsOnly, Category = "Pair Pressure|Carry", meta = (ClampMin = "50.0"))
	float AssistReach = 240.0f;

	UPROPERTY(EditDefaultsOnly, Category = "Pair Pressure|Carry", meta = (ClampMin = "10.0"))
	float AssistRadius = 85.0f;

	UPROPERTY(EditDefaultsOnly, Category = "Pair Pressure|Carry", meta = (ClampMin = "0.0"))
	float DragTargetDistance = 105.0f;

	UPROPERTY(EditDefaultsOnly, Category = "Pair Pressure|Carry", meta = (ClampMin = "0.0"))
	float DragAcceleration = 900.0f;

	UPROPERTY(EditDefaultsOnly, Category = "Pair Pressure|Carry", meta = (ClampMin = "0.0"))
	float ReviveRange = 150.0f;
};
