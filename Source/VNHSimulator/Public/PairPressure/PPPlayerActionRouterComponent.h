#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "PPPlayerActionRouterComponent.generated.h"

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FPPAirDiveStateChanged, bool, bIsDiving);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FPPAirDiveRecoveryStateChanged, bool, bIsRecovering);

UCLASS(ClassGroup = (PairPressure), BlueprintType, Blueprintable, meta = (BlueprintSpawnableComponent))
class VNHSIMULATOR_API UPPPlayerActionRouterComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UPPPlayerActionRouterComponent();
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	UFUNCTION(BlueprintCallable, Category = "Pair Pressure|Actions")
	void RequestDive();

	UFUNCTION(BlueprintCallable, Category = "Pair Pressure|Actions")
	void NotifyJumpStarted();

	UFUNCTION(BlueprintCallable, Category = "Pair Pressure|Actions")
	void NotifyLanded();

	UFUNCTION(BlueprintPure, Category = "Pair Pressure|Actions")
	bool CanAirDive() const;

	UFUNCTION(BlueprintPure, Category = "Pair Pressure|Actions")
	bool IsAirDiveActive() const { return bAirDiveActive; }

	UFUNCTION(BlueprintPure, Category = "Pair Pressure|Actions")
	bool IsAirDiveRecovering() const { return bAirDiveRecoveryActive; }

	UPROPERTY(BlueprintAssignable, Category = "Pair Pressure|Actions")
	FPPAirDiveStateChanged OnAirDiveStateChanged;

	UPROPERTY(BlueprintAssignable, Category = "Pair Pressure|Actions")
	FPPAirDiveRecoveryStateChanged OnAirDiveRecoveryStateChanged;

	UFUNCTION(BlueprintCallable, Category = "Pair Pressure|Actions")
	void BeginAssist();

	UFUNCTION(BlueprintCallable, Category = "Pair Pressure|Actions")
	void EndAssist();

private:
	UFUNCTION(Server, Reliable)
	void ServerNotifyJumpStarted();

	UFUNCTION(Server, Reliable)
	void ServerRequestDive();

	UFUNCTION(Client, Reliable)
	void ClientDiveRejected();

	UFUNCTION()
	void OnRep_AirDiveActive();

	UFUNCTION()
	void OnRep_AirDiveRecoveryActive();

	void ArmAirDive();
	void PerformDiveAuthoritative();
	void BeginDiveRecovery();
	void FinishDiveRecovery();

	UPROPERTY(Replicated)
	bool bAirDiveArmed = false;

	UPROPERTY(ReplicatedUsing = OnRep_AirDiveActive)
	bool bAirDiveActive = false;

	UPROPERTY(ReplicatedUsing = OnRep_AirDiveRecoveryActive)
	bool bAirDiveRecoveryActive = false;

	UPROPERTY(EditAnywhere, Category = "Pair Pressure|Actions|Dive", meta = (ClampMin = "0.0"))
	float DiveHorizontalSpeed = 680.0f;

	UPROPERTY(EditAnywhere, Category = "Pair Pressure|Actions|Dive")
	float DiveVerticalSpeed = 120.0f;

	UPROPERTY(EditAnywhere, Category = "Pair Pressure|Actions|Dive", meta = (ClampMin = "0.0"))
	float MinimumDivePresentationSeconds = 0.56f;

	UPROPERTY(EditAnywhere, Category = "Pair Pressure|Actions|Dive", meta = (ClampMin = "0.0"))
	float DiveLandingRecoverySeconds = 0.52f;

	UPROPERTY(EditAnywhere, Category = "Pair Pressure|Actions|Dive", meta = (ClampMin = "0.0"))
	float DiveLandingGetUpDelaySeconds = 0.75f;

	double DiveStartTimeSeconds = -1.0;
	FTimerHandle DiveRecoveryTimerHandle;
};
