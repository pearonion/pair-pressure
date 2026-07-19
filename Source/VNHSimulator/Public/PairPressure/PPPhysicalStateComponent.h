#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "PairPressure/PPGameplayInterfaces.h"
#include "PPPhysicalStateComponent.generated.h"

class UPrimitiveComponent;

DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FPPPhysicalStateChanged, EPPPhysicalState, NewState, float, DazeNormalized);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FPPDazeChanged, float, DazeNormalized);

// Compact server-authoritative root-body snapshot. The two-bone mascot ragdoll
// remains locally simulated for visual smoothness while this state prevents
// different machines from settling in different places or orientations.
USTRUCT()
struct FPPRagdollNetworkState
{
	GENERATED_BODY()

	UPROPERTY()
	FVector_NetQuantize100 RootLocation = FVector::ZeroVector;

	UPROPERTY()
	FRotator RootRotation = FRotator::ZeroRotator;

	UPROPERTY()
	FVector_NetQuantize100 LinearVelocity = FVector::ZeroVector;

	UPROPERTY()
	FVector_NetQuantize10 AngularVelocityDegrees = FVector::ZeroVector;

	UPROPERTY()
	uint16 Sequence = 0;

	UPROPERTY()
	bool bActive = false;
};

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

	// Camera presentation uses the authored recovery blend rather than tracking
	// individual physics bones after the standing handoff has begun.
	bool IsRagdollRecoveryBlendActive() const { return bRagdollRecoveryBlendActive; }
	float GetRagdollRecoveryBlendAlpha() const;

	UFUNCTION(BlueprintPure, Category = "Pair Pressure|Physical State")
	bool IsUnconscious() const { return PhysicalState == EPPPhysicalState::Unconscious; }

	UFUNCTION(BlueprintCallable, Category = "Pair Pressure|Physical State")
	void AddReviveProgress(float DeltaSeconds, AActor* Reviver);

	UFUNCTION(BlueprintCallable, Category = "Pair Pressure|Physical State|Debug")
	void RequestDebugRagdoll();

	UFUNCTION(BlueprintCallable, Category = "Pair Pressure|Physical State|Debug")
	void RequestDebugRecovery();

	// Authority-only throw entry point used by the dedicated grab dummy. It
	// starts the normal ragdoll/recovery path while preserving an authored launch.
	void RequestThrownRagdoll(
		const FVector& InitialVelocity,
		const FVector& InitialAngularVelocity,
		bool bClearExistingBodyVelocities = false,
		bool bCourseLaunch = false);

	// Builds the exact charged dummy launch before entering the shared thrown
	// ragdoll path. Course obstacles use zero inherited velocity so their authored
	// speed tier cannot be amplified by runtime collision velocity.
	void RequestDummyThrowProfileRagdoll(
		const FVector& ThrowDirection,
		float ChargeAlpha,
		const FVector& InheritedVelocity,
		float BaseThrowSpeed,
		bool bClearExistingBodyVelocities = false,
		bool bCourseLaunch = false);

	// Authority-only course-hazard entry point. Lobby hazards use a grounded,
	// low-travel knockdown; non-Lobby courses retain authored speed tiers.
	void RequestCourseObstacleRagdoll(
		const FVector& ImpactDirection,
		float AuthoredObstacleSpeed,
		const AActor* ObstacleActor,
		UPrimitiveComponent* ObstacleComponent,
		const FVector& ImpactPoint);

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

	UFUNCTION()
	void OnRep_RagdollNetworkState();

	void ApplyImpactAuthoritative(const FPPImpactData& ImpactData);
	void SetPhysicalStateAuthoritative(
		EPPPhysicalState NewState,
		float StateDurationSeconds = 0.0f,
		bool bDeferInitialNetworkPublish = false,
		bool bCourseRagdoll = false);
	void EnterRagdollVisualState();
	void ExitRagdollVisualState();
	void UpdateRagdollRecoveryBlend(float DeltaTime);
	void PublishRagdollNetworkState(bool bActive);
	void ApplyRemoteRagdollNetworkState(bool bSnapToAuthoritativeState);
	void UpdateRemoteRagdollNetworkState(float DeltaTime);
	void BeginGroundedRecovery(float RequiredGroundedSeconds);
	bool IsRagdollRestingOnGround() const;
	void PlayGetUpAnimation();
	void RecoverFromCurrentState();
	void AddDazeAuthoritative(float DazeAmount);

	UPROPERTY(ReplicatedUsing = OnRep_PhysicalState, VisibleInstanceOnly, Category = "Pair Pressure|Physical State")
	EPPPhysicalState PhysicalState = EPPPhysicalState::Grounded;

	UPROPERTY(ReplicatedUsing = OnRep_Daze, VisibleInstanceOnly, Category = "Pair Pressure|Physical State", meta = (ClampMin = "0.0", ClampMax = "100.0"))
	float Daze = 0.0f;

	UPROPERTY(ReplicatedUsing = OnRep_RagdollNetworkState)
	FPPRagdollNetworkState RagdollNetworkState;

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
	FVector LastRecoveryBodyUp = FVector::UpVector;
	FVector LastRecoveryBodyRight = FVector::RightVector;
	FTransform RagdollRecoveryBlendStartTransform;
	float RagdollRecoveryBlendElapsedSeconds = 0.0f;
	bool bRagdollRecoveryBlendActive = false;
	bool bHasAppliedRagdollNetworkSequence = false;
	uint16 LastAppliedRagdollNetworkSequence = 0;
	double LastRagdollNetworkPublishTimeSeconds = -100.0;
	double LastRagdollNetworkReceiptTimeSeconds = -100.0;
	FTimerHandle RecoveryTimerHandle;
};
