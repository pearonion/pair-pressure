#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "PairPressure/PPGameplayInterfaces.h"
#include "PPGrabberComponent.generated.h"

class UPhysicsHandleComponent;
class UPPGrabbableComponent;
class USkeletalMeshComponent;

DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FPPGrabStateChanged, EPPGrabState, NewGrabState, AActor*, NewTarget);
DECLARE_DYNAMIC_MULTICAST_DELEGATE(FPPGrabFailed);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FPPGrabReleasedPresentation, bool, bDroppedItem, bool, bLedgeClimb);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FPPGrabThrowPresentation, bool, bChargedThrow);

UCLASS(ClassGroup = (PairPressure), BlueprintType, Blueprintable, meta = (BlueprintSpawnableComponent))
class VNHSIMULATOR_API UPPGrabberComponent : public UActorComponent, public IPPGrabber
{
	GENERATED_BODY()

public:
	UPPGrabberComponent();

	virtual void BeginPlay() override;
	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	virtual void BeginGrab_Implementation(const FVector& CameraForward) override;
	virtual void ReleaseGrab_Implementation() override;
	virtual EPPGrabState GetGrabState_Implementation() const override { return GrabState; }

	UFUNCTION(BlueprintPure, Category = "Pair Pressure|Grab")
	AActor* GetGrabTarget() const { return GrabTarget; }

	UFUNCTION(BlueprintPure, Category = "Pair Pressure|Grab")
	float GetConstraintForceEstimate() const { return ConstraintForceEstimate; }

	UFUNCTION(BlueprintPure, Category = "Pair Pressure|Grab")
	FVector GetPresentationGrabPoint() const { return PresentationGrabPoint; }

	UFUNCTION(BlueprintPure, Category = "Pair Pressure|Grab")
	FPPGrabProfile GetActiveGrabProfile() const { return ActiveProfile; }

	UFUNCTION(BlueprintPure, Category = "Pair Pressure|Grab")
	AActor* GetIncomingGrabber() const { return IncomingGrabber; }

	UFUNCTION(BlueprintPure, Category = "Pair Pressure|Grab")
	bool IsStandingGrabRestricted() const { return IncomingGrabber != nullptr; }

	UFUNCTION(BlueprintPure, Category = "Pair Pressure|Grab")
	bool CanJumpOrDive() const { return IncomingGrabber == nullptr && GrabState != EPPGrabState::HangingFromLedge; }

	UFUNCTION(BlueprintPure, Category = "Pair Pressure|Grab|Ledge")
	bool IsHangingFromLedge() const { return GrabState == EPPGrabState::HangingFromLedge; }

	UFUNCTION(BlueprintCallable, Category = "Pair Pressure|Grab")
	void RequestHeldItemThrow(const FVector& ThrowDirection);

	void RequestChargedThrow(const FVector& ThrowDirection, float ChargeAlpha);

	UFUNCTION(BlueprintCallable, Category = "Pair Pressure|Grab")
	void RequestDirectionalEscape(const FVector& EscapeDirection);

	UFUNCTION(BlueprintCallable, Category = "Pair Pressure|Grab|Ledge")
	void RequestLedgeClimb();

	UPROPERTY(BlueprintAssignable, Category = "Pair Pressure|Grab")
	FPPGrabStateChanged OnGrabStateChanged;

	UPROPERTY(BlueprintAssignable, Category = "Pair Pressure|Grab")
	FPPGrabFailed OnGrabFailed;

	UPROPERTY(BlueprintAssignable, Category = "Pair Pressure|Grab|Presentation")
	FPPGrabReleasedPresentation OnGrabReleasedPresentation;

	UPROPERTY(BlueprintAssignable, Category = "Pair Pressure|Grab|Presentation")
	FPPGrabThrowPresentation OnGrabThrowPresentation;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Pair Pressure|Grab|Detection", meta = (ClampMin = "25.0"))
	float SearchReach = 250.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Pair Pressure|Grab|Detection", meta = (ClampMin = "5.0"))
	float SearchRadius = 70.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Pair Pressure|Grab|Detection")
	FVector GrabOriginOffset = FVector(35.0f, 0.0f, 55.0f);

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Pair Pressure|Grab|Debug")
	bool bDrawGrabDebug = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Pair Pressure|Grab|Player", meta = (ClampMin = "0.0"))
	float OpponentEscapeSeconds = 0.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Pair Pressure|Grab|Player", meta = (ClampMin = "0.0"))
	float SameOpponentImmunitySeconds = 1.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Pair Pressure|Grab|Player", meta = (ClampMin = "0.1", ClampMax = "1.0"))
	float GrabberMovementMultiplier = 0.72f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Pair Pressure|Grab|Player", meta = (ClampMin = "0.0", ClampMax = "0.25"))
	float VictimMovementMultiplier = 0.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Pair Pressure|Grab|Player", meta = (ClampMin = "0.05", ClampMax = "1.0"))
	float MutualMovementMultiplier = 0.46f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Pair Pressure|Grab|Player", meta = (ClampMin = "0.05", ClampMax = "1.0"))
	float GrabberTurnMultiplier = 0.55f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Pair Pressure|Grab|Player", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float AirborneHorizontalMomentumMultiplier = 0.55f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Pair Pressure|Grab|Player", meta = (ClampMin = "0.0"))
	float PlayerConstraintAcceleration = 1250.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Pair Pressure|Grab|Player", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float EscapeBlockedTowardDot = 0.25f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Pair Pressure|Grab|Player", meta = (ClampMin = "0.0"))
	float EscapeJumpHorizontalSpeed = 360.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Pair Pressure|Grab|Player", meta = (ClampMin = "0.0"))
	float EscapeJumpVerticalSpeed = 520.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Pair Pressure|Grab|Item", meta = (ClampMin = "0.0"))
	float HeldItemThrowSpeed = 1050.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Pair Pressure|Grab|Player", meta = (ClampMin = "20.0"))
	float PlayerHoldDistance = 105.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Pair Pressure|Grab|Player", meta = (ClampMin = "0.0"))
	float PlayerHoldHeight = 165.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Pair Pressure|Grab|Player", meta = (ClampMin = "0.0"))
	float TeammateRescueStrengthMultiplier = 1.35f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Pair Pressure|Grab|Ledge", meta = (ClampMin = "0.0"))
	float LedgeConstraintAcceleration = 1550.0f;

private:
	UFUNCTION(Server, Reliable)
	void ServerBeginGrab(FVector_NetQuantizeNormal CameraForward);

	UFUNCTION(Server, Reliable)
	void ServerReleaseGrab();

	UFUNCTION(Server, Reliable)
	void ServerThrowHeldItem(FVector_NetQuantizeNormal ThrowDirection);

	UFUNCTION(Server, Reliable)
	void ServerThrowHeldItemCharged(FVector_NetQuantizeNormal ThrowDirection, float ChargeAlpha);

	UFUNCTION(Server, Reliable)
	void ServerRequestDirectionalEscape(FVector_NetQuantizeNormal EscapeDirection);

	UFUNCTION(Server, Reliable)
	void ServerRequestLedgeClimb();

	UFUNCTION(Client, Reliable)
	void ClientGrabRejected();

	UFUNCTION(NetMulticast, Reliable)
	void MulticastGrabReleasedPresentation(bool bDroppedItem, bool bLedgeClimb);

	UFUNCTION(NetMulticast, Reliable)
	void MulticastGrabThrowPresentation(bool bChargedThrow);

	UFUNCTION()
	void OnRep_GrabPresentation();

	FPPGrabTargetData FindBestTarget(const FVector& CameraForward) const;
	UPPGrabbableComponent* FindGrabbableComponent(AActor* CandidateActor) const;
	bool ImplementsBlueprintGrabbableContract(AActor* CandidateActor) const;
	bool IsBlueprintGrabEnabled(AActor* CandidateActor) const;
	FPPGrabProfile GetBlueprintGrabProfile(AActor* CandidateActor) const;
	void NotifyBlueprintGrabEvent(AActor* CandidateActor, bool bGrabStarted) const;
	bool BuildTargetData(AActor* CandidateActor, const FVector& TraceStart, const FVector& CameraForward, FPPGrabTargetData& OutTargetData) const;
	bool HasClearLineOfSight(const FVector& TraceStart, const FPPGrabTargetData& TargetData) const;
	void StartGrabAuthoritative(const FPPGrabTargetData& TargetData);
	void ReleaseGrabAuthoritative(bool bPlayRecoveryAnimation = true, bool bPlayLedgeClimbAnimation = false);
	void UpdateReachSearch(float DeltaTime);
	void UpdateHeldItem(float DeltaTime);
	void UpdatePlayerGrab(float DeltaTime);
	void UpdatePushable(float DeltaTime);
	void UpdateLedgeGrab(float DeltaTime);
	bool IsGrabDummyPenguin(const ACharacter* TargetCharacter) const;
	void BeginGrabDummyCarry(ACharacter* TargetCharacter);
	void EndGrabDummyCarry();
	void BeginIncomingPlayerGrab(AActor* NewIncomingGrabber);
	void EndIncomingPlayerGrab(AActor* PreviousIncomingGrabber, bool bApplyImmunity);
	void ApplyIncomingGrabRagdoll(bool bEnableRagdoll);
	FName ResolvePlayerGrabBone(const USkeletalMeshComponent* TargetMesh) const;
	FName ResolvePlayerRecoveryBone(const USkeletalMeshComponent* TargetMesh) const;
	void PerformHeldItemThrow(const FVector& ThrowDirection, float ChargeAlpha);
	void PlayGetUpFrontAnimation(ACharacter* RecoveringCharacter);
	void PerformDirectionalEscape(const FVector& EscapeDirection);
	void PerformLedgeClimb();
	void CancelReciprocalGrab(UPPGrabberComponent* OtherGrabber);
	void ApplyMovementPresentation();
	void ForceDropTargetItem(AActor* TargetActor) const;
	bool IsFriendlyPlayer(const AActor* OtherActor) const;
	bool IsStandingPlayerStateValid(const AActor* PlayerActor) const;
	void ApplyBreakFreePenalty(AActor* EscapedPlayer);
	int32 GetDefaultPriority(EPPGrabTargetType TargetType) const;
	FVector GetGrabOrigin() const;
	FVector GetHandCarryLocation(float ForwardDistance) const;
	FVector GetPlayerCarryLocation() const;
	FVector GetGrabDummyCarryLocation() const;
	void SetGrabPresentation(EPPGrabState NewGrabState, AActor* NewGrabTarget);

	UPROPERTY(Transient)
	TObjectPtr<UPhysicsHandleComponent> PhysicsHandle = nullptr;

	UPROPERTY(ReplicatedUsing = OnRep_GrabPresentation, VisibleInstanceOnly, Category = "Pair Pressure|Grab")
	EPPGrabState GrabState = EPPGrabState::None;

	UPROPERTY(ReplicatedUsing = OnRep_GrabPresentation, VisibleInstanceOnly, Category = "Pair Pressure|Grab")
	TObjectPtr<AActor> GrabTarget = nullptr;

	UPROPERTY(ReplicatedUsing = OnRep_GrabPresentation, VisibleInstanceOnly, Category = "Pair Pressure|Grab")
	TObjectPtr<AActor> IncomingGrabber = nullptr;

	UPROPERTY(ReplicatedUsing = OnRep_GrabPresentation, VisibleInstanceOnly, Category = "Pair Pressure|Grab")
	FVector_NetQuantize PresentationGrabPoint = FVector::ZeroVector;

	UPROPERTY(Transient)
	TObjectPtr<UPrimitiveComponent> GrabbedPrimitive = nullptr;

	UPROPERTY(Replicated)
	FPPGrabProfile ActiveProfile;
	FVector LastValidatedCameraForward = FVector::ForwardVector;
	FVector FixedWorldGrabPoint = FVector::ZeroVector;
	FVector LockedPushAxis = FVector::ForwardVector;
	FRotator LockedOwnerRotation = FRotator::ZeroRotator;
	FRotator LockedGrabbedRotation = FRotator::ZeroRotator;
	FRotator LockedGrabDummyCarryRotation = FRotator::ZeroRotator;
	FTransform IncomingGrabInitialMeshRelativeTransform;
	TWeakObjectPtr<ACharacter> CarriedGrabDummy;
	FName ActivePlayerGrabBone = NAME_None;
	float ConstraintForceEstimate = 0.0f;
	float SustainedGrabSeconds = 0.0f;
	TWeakObjectPtr<AActor> ImmuneGrabber;
	double ImmunityEndTimeSeconds = 0.0;
	bool bReleasingPairedGrab = false;
	bool bIncomingGrabRagdollApplied = false;
};
