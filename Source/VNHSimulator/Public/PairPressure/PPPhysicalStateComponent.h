#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "PairPressure/PPGameplayInterfaces.h"
#include "PPPhysicalStateComponent.generated.h"

DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FPPPhysicalStateChanged, EPPPhysicalState, NewState, float, DazeNormalized);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FPPDazeChanged, float, DazeNormalized);

UCLASS(ClassGroup = (PairPressure), BlueprintType, Blueprintable, meta = (BlueprintSpawnableComponent))
class VNHSIMULATOR_API UPPPhysicalStateComponent : public UActorComponent,
	public IPPPhysicalStateInterface,
	public IPPImpactReceiver,
	public IPPCarryable
{
	GENERATED_BODY()

public:
	UPPPhysicalStateComponent();

	virtual void BeginPlay() override;
	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	virtual EPPPhysicalState GetPhysicalState_Implementation() const override { return PhysicalState; }
	virtual float GetDazeNormalized_Implementation() const override;
	virtual void RequestRagdoll_Implementation(float DazeAmount) override;
	virtual void RequestRecovery_Implementation() override;
	virtual void ReceiveImpactData_Implementation(const FPPImpactData& ImpactData) override;
	virtual bool CanCarry_Implementation(AActor* RequestedCarrier) const override;
	virtual void OnCarryStarted_Implementation(AActor* NewCarrier) override;
	virtual void OnCarryEnded_Implementation(AActor* PreviousCarrier) override;

	UFUNCTION(BlueprintPure, Category = "Pair Pressure|Physical State")
	EPPPhysicalState GetCurrentPhysicalState() const { return PhysicalState; }

	UFUNCTION(BlueprintPure, Category = "Pair Pressure|Physical State")
	float GetDaze() const { return Daze; }

	UFUNCTION(BlueprintPure, Category = "Pair Pressure|Physical State")
	bool IsRagdolled() const;

	UFUNCTION(BlueprintPure, Category = "Pair Pressure|Physical State")
	bool IsUnconscious() const { return PhysicalState == EPPPhysicalState::Unconscious; }

	UFUNCTION(BlueprintCallable, Category = "Pair Pressure|Physical State")
	void AddReviveProgress(float DeltaSeconds, AActor* Reviver);

	UFUNCTION(BlueprintCallable, Category = "Pair Pressure|Physical State|Debug")
	void RequestDebugRagdoll();

	UFUNCTION(BlueprintCallable, Category = "Pair Pressure|Physical State|Debug")
	void RequestDebugRecovery();

	UPROPERTY(BlueprintAssignable, Category = "Pair Pressure|Physical State")
	FPPPhysicalStateChanged OnPhysicalStateChanged;

	UPROPERTY(BlueprintAssignable, Category = "Pair Pressure|Physical State")
	FPPDazeChanged OnDazeChanged;

	static UPPPhysicalStateComponent* FindPhysicalStateComponent(const AActor* Actor);

private:
	UFUNCTION(Server, Reliable)
	void ServerRequestRagdoll(float DazeAmount);

	UFUNCTION(Server, Reliable)
	void ServerRequestRecovery();

	UFUNCTION(Server, Reliable)
	void ServerRequestDebugRagdoll();

	UFUNCTION(Server, Reliable)
	void ServerRequestDebugRecovery();

	UFUNCTION()
	void OnRep_PhysicalState();

	UFUNCTION()
	void OnRep_Daze();

	void ApplyImpactAuthoritative(const FPPImpactData& ImpactData);
	void SetPhysicalStateAuthoritative(EPPPhysicalState NewState, float StateDurationSeconds = 0.0f);
	void EnterRagdollVisualState();
	void ExitRagdollVisualState();
	void BeginGroundedRecovery(float RequiredGroundedSeconds);
	bool IsRagdollRestingOnGround() const;
	void ApplyRagdollPropulsion(const FPPImpactData& ImpactData, float EffectiveSeverity);
	void PlayGetUpFrontAnimation();
	void RecoverFromCurrentState();
	void AddDazeAuthoritative(float DazeAmount);

	UPROPERTY(ReplicatedUsing = OnRep_PhysicalState, VisibleInstanceOnly, Category = "Pair Pressure|Physical State")
	EPPPhysicalState PhysicalState = EPPPhysicalState::Grounded;

	UPROPERTY(ReplicatedUsing = OnRep_Daze, VisibleInstanceOnly, Category = "Pair Pressure|Physical State", meta = (ClampMin = "0.0", ClampMax = "100.0"))
	float Daze = 0.0f;

	UPROPERTY(EditDefaultsOnly, Category = "Pair Pressure|Physical State", meta = (ClampMin = "0.0"))
	float MaxDaze = 100.0f;

	UPROPERTY(EditDefaultsOnly, Category = "Pair Pressure|Physical State", meta = (ClampMin = "0.0"))
	float SafeDazeDecayPerSecond = 3.0f;

	UPROPERTY(EditDefaultsOnly, Category = "Pair Pressure|Physical State", meta = (ClampMin = "0.1"))
	float NormalRagdollSeconds = 2.0f;

	UPROPERTY(EditDefaultsOnly, Category = "Pair Pressure|Physical State", meta = (ClampMin = "0.1"))
	float NaturalUnconsciousSeconds = 10.0f;

	UPROPERTY(EditDefaultsOnly, Category = "Pair Pressure|Physical State", meta = (ClampMin = "0.1"))
	float TeammateReviveSeconds = 3.0f;

	float ReviveProgressSeconds = 0.0f;
	double LastKnockdownTimeSeconds = -100.0;
	FTransform InitialMeshRelativeTransform;
	FTimerHandle RecoveryTimerHandle;
};
