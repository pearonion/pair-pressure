#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "PairPressure/PPGameplayInterfaces.h"
#include "PPGrabbableComponent.generated.h"

UCLASS(ClassGroup = (PairPressure), BlueprintType, Blueprintable, meta = (BlueprintSpawnableComponent))
class VNHSIMULATOR_API UPPGrabbableComponent : public UActorComponent, public IPPGrabbable
{
	GENERATED_BODY()

public:
	UPPGrabbableComponent();

	virtual void BeginPlay() override;
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	virtual bool CanBeGrabbed_Implementation(AActor* RequestingGrabber) const override;
	virtual FPPGrabProfile GetGrabProfile_Implementation() const override;
	virtual UPrimitiveComponent* GetGrabPrimitive_Implementation() const override;
	virtual FVector GetGrabPoint_Implementation(AActor* RequestingGrabber) const override;
	virtual void OnGrabStarted_Implementation(AActor* NewGrabber) override;
	virtual void OnGrabEnded_Implementation(AActor* PreviousGrabber) override;

	UFUNCTION(BlueprintPure, Category = "Pair Pressure|Grab")
	AActor* GetCurrentGrabber() const { return CurrentGrabber; }

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Pair Pressure|Grab")
	FPPGrabProfile GrabProfile;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Pair Pressure|Grab")
	FName GripPointComponentName = TEXT("GripPoint");

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Pair Pressure|Grab")
	bool bGrabEnabled = true;

private:
	UPROPERTY(Replicated, VisibleInstanceOnly, Category = "Pair Pressure|Grab")
	TObjectPtr<AActor> CurrentGrabber = nullptr;
};
