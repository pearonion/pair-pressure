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
	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	UFUNCTION(BlueprintCallable, Category = "Pair Pressure|Actions")
	void RequestDive();

	UFUNCTION(BlueprintCallable, Category = "Pair Pressure|Actions")
	void NotifyJumpStarted();

	UFUNCTION(BlueprintCallable, Category = "Pair Pressure|Actions")
	void NotifyLanded();

	// Server-side handoff used when an active air dive catches a ledge.
	void CancelAirDiveForLedgeGrab();

	UFUNCTION(BlueprintPure, Category = "Pair Pressure|Actions")
	bool CanAirDive() const;

	UFUNCTION(BlueprintPure, Category = "Pair Pressure|Actions")
	bool IsAirDiveActive() const { return bAirDiveActive; }

	UFUNCTION(BlueprintPure, Category = "Pair Pressure|Actions")
	bool IsAirDiveRecovering() const { return bAirDiveRecoveryActive; }

	// Server-synchronized offsets let simulated proxies join an in-progress
	// action at the correct animation point instead of replaying from zero.
	float GetDivePresentationElapsedSeconds() const;
	float GetDiveRecoveryPresentationElapsedSeconds() const;

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
	void ApplyDiveLaunch();
	void PerformDiveAuthoritative();
	void BeginDiveRecovery();
	void FinishDiveRecovery();
	void CancelInterruptedDive();

	UPROPERTY(Replicated)
	bool bAirDiveArmed = false;

	UPROPERTY(ReplicatedUsing = OnRep_AirDiveActive)
	bool bAirDiveActive = false;

	UPROPERTY(ReplicatedUsing = OnRep_AirDiveRecoveryActive)
	bool bAirDiveRecoveryActive = false;

	UPROPERTY(Replicated)
	float DiveServerStartTimeSeconds = -1.0f;

	UPROPERTY(Replicated)
	float DiveRecoveryServerStartTimeSeconds = -1.0f;

	UPROPERTY(EditAnywhere, Category = "Pair Pressure|Actions|Dive", meta = (ClampMin = "0.0"))
	float DiveHorizontalSpeed = 680.0f;

	UPROPERTY(EditAnywhere, Category = "Pair Pressure|Actions|Dive")
	float DiveVerticalSpeed = 120.0f;

	UPROPERTY(EditAnywhere, Category = "Pair Pressure|Actions|Dive", meta = (ClampMin = "0.0"))
	float MinimumDivePresentationSeconds = 0.56f;

	UPROPERTY(EditAnywhere, Category = "Pair Pressure|Actions|Dive", meta = (ClampMin = "0.0"))
	float DiveLandingRecoverySeconds = 0.42f;

	UPROPERTY(EditAnywhere, Category = "Pair Pressure|Actions|Dive", meta = (ClampMin = "0.0"))
	float DiveLandingGetUpDelaySeconds = 0.60f;

	double DiveStartTimeSeconds = -1.0;
	FTimerHandle DiveRecoveryTimerHandle;
};
