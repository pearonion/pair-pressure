#include "VNHAlienLocomotionComponent.h"

#include "GameFramework/Character.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/Controller.h"
#include "Components/SkeletalMeshComponent.h"
#include "EngineUtils.h"
#include "PairPressure/PPGrabberComponent.h"
#include "PairPressure/PPPhysicalStateComponent.h"
#include "PairPressure/PPPlayerActionRouterComponent.h"
#include "PairPressure/PPTeamMemberComponent.h"
#include "UObject/StructOnScope.h"
#include "UObject/UnrealType.h"
#include "VNHMovementTuningData.h"

UVNHAlienLocomotionComponent::UVNHAlienLocomotionComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	SetIsReplicatedByDefault(false);
}

void UVNHAlienLocomotionComponent::BeginPlay()
{
	Super::BeginPlay();

	OwnerCharacter = Cast<ACharacter>(GetOwner());
	if (OwnerCharacter.IsValid())
	{
		MovementComponent = OwnerCharacter->GetCharacterMovement();
		OwnerCharacter->bUseControllerRotationYaw = false;
		if (MovementComponent.IsValid())
		{
			MovementComponent->bOrientRotationToMovement = true;
			MovementComponent->bUseControllerDesiredRotation = false;
			MovementComponent->RotationRate = FRotator(0.0f, GetBodyTurnRateDegrees() * GrabTurnMultiplier, 0.0f);
			MovementComponent->AirControl = DefaultAirControl;
			MovementComponent->AirControlBoostMultiplier = 1.0f;
		}
	}
}

void UVNHAlienLocomotionComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

	if (!ShouldDriveLocomotion())
	{
		ClearInput();
		LocomotionState.CurrentSpeed = 0.0f;
		LocomotionState.DesiredSpeed = 0.0f;
		LocomotionState.DesiredWorldDirection = FVector::ZeroVector;
		LocomotionState.bHasMoveInput = false;
		LocomotionState.BodyYawDeltaDegrees = 0.0f;
		LocomotionState.Stability = 1.0f;
		LocomotionState.Instability = 0.0f;
		LocomotionState.WobbleDegrees = 0.0f;
		LastDesiredDirection = FVector::ZeroVector;
		UpdateAnimationRate();
		return;
	}

	const bool bIsFalling = MovementComponent->IsFalling();
	if (bIsFalling)
	{
		UpdateTetherTensionFromWorld();
	}
	else
	{
		TetherTensionNormalized = 0.0f;
	}
	const float HorizontalSpeed = OwnerCharacter->GetVelocity().Size2D();
	if (bIsFalling && !bWasFalling)
	{
		AirborneHorizontalSpeedCap = FMath::Max(HorizontalSpeed, GetWalkSpeed() * 0.35f);
		PreviousAirborneHorizontalSpeed = HorizontalSpeed;
		bExternalLaunchDetected = false;
	}
	else if (bIsFalling)
	{
		if (HorizontalSpeed - PreviousAirborneHorizontalSpeed > ExternalLaunchVelocityDeltaThreshold)
		{
			bExternalLaunchDetected = true;
			AirborneHorizontalSpeedCap = FMath::Max(AirborneHorizontalSpeedCap, HorizontalSpeed);
		}
		PreviousAirborneHorizontalSpeed = HorizontalSpeed;
	}
	else if (bWasFalling)
	{
		AirborneHorizontalSpeedCap = 0.0f;
		PreviousAirborneHorizontalSpeed = 0.0f;
		bExternalLaunchDetected = false;
		MovementComponent->AirControl = DefaultAirControl;
	}
	bWasFalling = bIsFalling;

	const FVector DesiredDirection = BuildCameraRelativeMoveDirection();
	UpdateInstability(DeltaTime, DesiredDirection);
	UpdateSpeed(DeltaTime, DesiredDirection);
	ApplyMovement(DeltaTime, DesiredDirection);
	UpdateBodyFacing(DeltaTime, DesiredDirection);
	UpdateAnimationRate();
}

void UVNHAlienLocomotionComponent::SetMoveInput(FVector2D NewMoveInput)
{
	MoveInput = NewMoveInput.GetClampedToMaxSize(1.0f);
}

void UVNHAlienLocomotionComponent::SetFastWalkRequested(bool bNewFastWalkRequested)
{
	LocomotionState.bFastWalkRequested = bNewFastWalkRequested;
}

void UVNHAlienLocomotionComponent::ClearInput()
{
	MoveInput = FVector2D::ZeroVector;
	LocomotionState.bFastWalkRequested = false;
}

void UVNHAlienLocomotionComponent::SetCameraOrbitActive(bool bNewCameraOrbitActive)
{
	bCameraOrbitActive = bNewCameraOrbitActive;
	if (MovementComponent.IsValid())
	{
		MovementComponent->bOrientRotationToMovement = true;
		MovementComponent->bUseControllerDesiredRotation = false;
		MovementComponent->RotationRate = FRotator(0.0f, GetBodyTurnRateDegrees() * GrabTurnMultiplier, 0.0f);
	}
}

void UVNHAlienLocomotionComponent::SetGrabMovementMultiplier(float NewGrabMovementMultiplier)
{
	GrabMovementMultiplier = FMath::Clamp(NewGrabMovementMultiplier, 0.0f, 1.0f);
	if (MovementComponent.IsValid())
	{
		MovementComponent->MaxWalkSpeed = GetWalkSpeed() * GrabMovementMultiplier;
	}
}

void UVNHAlienLocomotionComponent::SetGrabTurnMultiplier(float NewGrabTurnMultiplier)
{
	GrabTurnMultiplier = FMath::Clamp(NewGrabTurnMultiplier, 0.05f, 1.0f);
	if (MovementComponent.IsValid())
	{
		MovementComponent->RotationRate = FRotator(0.0f, GetBodyTurnRateDegrees() * GrabTurnMultiplier, 0.0f);
	}
}

void UVNHAlienLocomotionComponent::SetTetherTensionNormalized(float NewTetherTensionNormalized)
{
	TetherTensionNormalized = FMath::Clamp(NewTetherTensionNormalized, 0.0f, 1.0f);
}

FString UVNHAlienLocomotionComponent::DescribeLocomotionState() const
{
	return FString::Printf(
		TEXT("DesiredSpeed=%.1f CurrentSpeed=%.1f HasInput=%s FastWalk=%s ManualBrake=%s CorrectionStep=%s BodyYawDelta=%.1f Stability=%.2f Instability=%.2f Wobble=%.1f DesiredDir=(%.2f, %.2f, %.2f)"),
		LocomotionState.DesiredSpeed,
		LocomotionState.CurrentSpeed,
		LocomotionState.bHasMoveInput ? TEXT("true") : TEXT("false"),
		LocomotionState.bFastWalkRequested ? TEXT("true") : TEXT("false"),
		LocomotionState.bManualBrake ? TEXT("true") : TEXT("false"),
		LocomotionState.bCorrectionStepRequested ? TEXT("true") : TEXT("false"),
		LocomotionState.BodyYawDeltaDegrees,
		LocomotionState.Stability,
		LocomotionState.Instability,
		LocomotionState.WobbleDegrees,
		LocomotionState.DesiredWorldDirection.X,
		LocomotionState.DesiredWorldDirection.Y,
		LocomotionState.DesiredWorldDirection.Z);
}

FVector UVNHAlienLocomotionComponent::GetCameraRelativeMoveDirection() const
{
	return BuildCameraRelativeMoveDirection();
}

bool UVNHAlienLocomotionComponent::ShouldDriveLocomotion() const
{
	if (!OwnerCharacter.IsValid() || !MovementComponent.IsValid())
	{
		return false;
	}

	const AController* Controller = OwnerCharacter->GetController();
	const UPPPlayerActionRouterComponent* ActionRouter = GetOwner()
		? GetOwner()->FindComponentByClass<UPPPlayerActionRouterComponent>()
		: nullptr;
	return Controller && Controller->IsPlayerController() && OwnerCharacter->IsLocallyControlled()
		&& (!ActionRouter || !ActionRouter->IsAirDiveActive());
}

FVector UVNHAlienLocomotionComponent::BuildCameraRelativeMoveDirection() const
{
	if (!OwnerCharacter.IsValid())
	{
		return FVector::ZeroVector;
	}

	const AController* Controller = OwnerCharacter->GetController();
	const FRotator ReferenceRotation = Controller
		? Controller->GetControlRotation()
		: OwnerCharacter->GetActorRotation();
	const FRotator YawRotation(0.0f, ReferenceRotation.Yaw, 0.0f);

	const FVector Forward = FRotationMatrix(YawRotation).GetUnitAxis(EAxis::X);
	const FVector Right = FRotationMatrix(YawRotation).GetUnitAxis(EAxis::Y);
	const FVector DesiredDirection = Forward * MoveInput.Y + Right * MoveInput.X;
	if (const UPPGrabberComponent* GrabberComponent = GetOwner()->FindComponentByClass<UPPGrabberComponent>())
	{
		const EPPGrabState CurrentGrabState = GrabberComponent->GetGrabState_Implementation();
		if (CurrentGrabState == EPPGrabState::HangingFromLedge)
		{
			return FVector::ZeroVector;
		}
		if (CurrentGrabState == EPPGrabState::PushingObject)
		{
			const FVector LockedForward = OwnerCharacter->GetActorForwardVector().GetSafeNormal2D();
			const float ForwardAmount = FVector::DotProduct(DesiredDirection, LockedForward);
			return FMath::Abs(ForwardAmount) >= 0.2f
				? LockedForward * FMath::Sign(ForwardAmount)
				: FVector::ZeroVector;
		}
	}

	return DesiredDirection.GetSafeNormal();
}

void UVNHAlienLocomotionComponent::UpdateInstability(float DeltaTime, const FVector& DesiredDirection)
{
	const bool bHasInput = MoveInput.SizeSquared() > KINDA_SMALL_NUMBER && !DesiredDirection.IsNearlyZero();
	if (!bHasInput)
	{
		LocomotionState.Stability = FMath::FInterpConstantTo(LocomotionState.Stability, 0.0f, DeltaTime, StabilityDecayRate);
		LocomotionState.Instability = 1.0f - LocomotionState.Stability;
		LocomotionState.WobbleDegrees = 0.0f;
		LastDesiredDirection = FVector::ZeroVector;
		return;
	}

	float AbruptTurnAmount = 0.0f;
	if (!LastDesiredDirection.IsNearlyZero())
	{
		const float DirectionDot = FMath::Clamp(FVector::DotProduct(LastDesiredDirection, DesiredDirection), -1.0f, 1.0f);
		AbruptTurnAmount = 1.0f - FMath::Max(0.0f, DirectionDot);
	}

	const float BuildRate = StabilityBuildRate * (LocomotionState.bFastWalkRequested ? 0.55f : 1.0f);
	LocomotionState.Stability = FMath::FInterpConstantTo(LocomotionState.Stability, 1.0f, DeltaTime, BuildRate);
	LocomotionState.Stability = FMath::Clamp(LocomotionState.Stability - AbruptTurnAmount * AbruptTurnInstability - (LocomotionState.bFastWalkRequested ? FastWalkInstability * DeltaTime : 0.0f), 0.0f, 1.0f);
	LocomotionState.Instability = 1.0f - LocomotionState.Stability;

	constexpr float TwoPi = 2.0f * PI;
	WobblePhaseRadians = FMath::Fmod(WobblePhaseRadians + DeltaTime * WobbleFrequencyHz * TwoPi * FMath::Lerp(1.25f, 0.65f, LocomotionState.Stability), TwoPi);
	LocomotionState.WobbleDegrees = FMath::Sin(WobblePhaseRadians) * WobbleYawDegrees * LocomotionState.Instability;
	LastDesiredDirection = DesiredDirection;
}

void UVNHAlienLocomotionComponent::UpdateSpeed(float DeltaTime, const FVector& DesiredDirection)
{
	const float InputMagnitude = MoveInput.Size();
	const bool bHasInput = InputMagnitude > KINDA_SMALL_NUMBER && !DesiredDirection.IsNearlyZero();
	const FVector CurrentVelocity = OwnerCharacter.IsValid() ? OwnerCharacter->GetVelocity().GetSafeNormal2D() : FVector::ZeroVector;
	const float DirectionDot = bHasInput && !CurrentVelocity.IsNearlyZero() ? FVector::DotProduct(CurrentVelocity, DesiredDirection) : 1.0f;

	LocomotionState.bHasMoveInput = bHasInput;
	LocomotionState.bManualBrake = bHasInput && DirectionDot < -0.2f && LocomotionState.CurrentSpeed > KINDA_SMALL_NUMBER;
	LocomotionState.bCorrectionStepRequested = bHasInput && DirectionDot < FMath::Cos(FMath::DegreesToRadians(GetCorrectionStepAngleDegrees()));
	LocomotionState.DesiredWorldDirection = DesiredDirection;
	if (!bHasInput)
	{
		LocomotionState.BodyYawDeltaDegrees = 0.0f;
	}

	const float MaxRequestedSpeed = LocomotionState.bFastWalkRequested ? GetFastWalkSpeed() : GetWalkSpeed();
	LocomotionState.DesiredSpeed = bHasInput ? FMath::Lerp(GetMinWalkSpeed(), MaxRequestedSpeed, InputMagnitude) : 0.0f;

	const float Rate = LocomotionState.DesiredSpeed > LocomotionState.CurrentSpeed
		? GetAcceleration()
		: GetCoastBraking() * (LocomotionState.bManualBrake ? GetManualBrakeMultiplier() : 1.0f);

	LocomotionState.CurrentSpeed = FMath::FInterpConstantTo(LocomotionState.CurrentSpeed, LocomotionState.DesiredSpeed, DeltaTime, Rate);

	if (MovementComponent.IsValid())
	{
		MovementComponent->MaxAcceleration = MovementComponent->IsFalling() ? AirborneAcceleration : GetAcceleration();
		MovementComponent->BrakingDecelerationWalking = GetCoastBraking() * (LocomotionState.bManualBrake ? GetManualBrakeMultiplier() : 1.0f);
	}
}

void UVNHAlienLocomotionComponent::ApplyMovement(float DeltaTime, const FVector& DesiredDirection)
{
	if (!OwnerCharacter.IsValid() || !MovementComponent.IsValid() || DesiredDirection.IsNearlyZero()
		|| GrabMovementMultiplier <= KINDA_SMALL_NUMBER)
	{
		return;
	}

	if (MovementComponent->IsFalling())
	{
		if (!IsControlledAirSteeringAllowed())
		{
			MovementComponent->AirControl = 0.0f;
			return;
		}

		const float AirControlMultiplier = GetAirborneControlMultiplier();
		MovementComponent->AirControl = DefaultAirControl * AirControlMultiplier;
		MovementComponent->MaxAcceleration = AirborneAcceleration;
		MovementComponent->MaxWalkSpeed = FMath::Max(1.0f, AirborneHorizontalSpeedCap);

		const FVector HorizontalVelocity = OwnerCharacter->GetVelocity().GetSafeNormal2D();
		FVector LimitedDirection = DesiredDirection.GetSafeNormal2D();
		if (!HorizontalVelocity.IsNearlyZero())
		{
			const float CurrentYaw = HorizontalVelocity.Rotation().Yaw;
			const float DesiredYaw = LimitedDirection.Rotation().Yaw;
			const float MaxYawStep = AirborneSteeringDegreesPerSecond * AirControlMultiplier * DeltaTime;
			const float LimitedYaw = CurrentYaw + FMath::Clamp(
				FMath::FindDeltaAngleDegrees(CurrentYaw, DesiredYaw),
				-MaxYawStep,
				MaxYawStep);
			LimitedDirection = FRotator(0.0f, LimitedYaw, 0.0f).Vector();
		}
		OwnerCharacter->AddMovementInput(LimitedDirection, AirControlMultiplier);
		return;
	}

	MovementComponent->AirControl = DefaultAirControl;
	const FVector DriftRight = FVector::CrossProduct(FVector::UpVector, DesiredDirection).GetSafeNormal();
	const float DriftAmount = FMath::Sin(WobblePhaseRadians) * LateralDriftStrength * LocomotionState.Instability;
	const FVector UnstableDirection = (DesiredDirection + DriftRight * DriftAmount).GetSafeNormal();
	const float UnevenStride = 1.0f + FMath::Sin(WobblePhaseRadians * 1.7f) * UnevenStrideStrength * LocomotionState.Instability;

	MovementComponent->MaxWalkSpeed = FMath::Max(LocomotionState.CurrentSpeed * UnevenStride * GrabMovementMultiplier, 1.0f);
	OwnerCharacter->AddMovementInput(UnstableDirection, 1.0f);
}

void UVNHAlienLocomotionComponent::UpdateBodyFacing(float /*DeltaTime*/, const FVector& DesiredDirection)
{
	if (!OwnerCharacter.IsValid() || !MovementComponent.IsValid())
	{
		return;
	}
	if (const UPPGrabberComponent* GrabberComponent = GetOwner()->FindComponentByClass<UPPGrabberComponent>())
	{
		const EPPGrabState CurrentGrabState = GrabberComponent->GetGrabState_Implementation();
		if (CurrentGrabState == EPPGrabState::PushingObject || CurrentGrabState == EPPGrabState::HangingFromLedge)
		{
			MovementComponent->bOrientRotationToMovement = false;
			MovementComponent->RotationRate = FRotator::ZeroRotator;
			LocomotionState.BodyYawDeltaDegrees = 0.0f;
			return;
		}
	}
	MovementComponent->bOrientRotationToMovement = true;

	if (GrabMovementMultiplier <= KINDA_SMALL_NUMBER)
	{
		LocomotionState.BodyYawDeltaDegrees = FMath::FindDeltaAngleDegrees(
			OwnerCharacter->GetActorRotation().Yaw,
			OwnerCharacter->GetControlRotation().Yaw);
		return;
	}

	if (DesiredDirection.IsNearlyZero())
	{
		LocomotionState.BodyYawDeltaDegrees = FMath::FindDeltaAngleDegrees(
			OwnerCharacter->GetActorRotation().Yaw,
			OwnerCharacter->GetControlRotation().Yaw);
		return;
	}

	const float WobbleYaw = !DesiredDirection.IsNearlyZero() ? LocomotionState.WobbleDegrees : 0.0f;
	const float TargetBodyYaw = DesiredDirection.Rotation().Yaw + WobbleYaw;
	const float AirborneTurnMultiplier = MovementComponent->IsFalling() ? 0.30f * GetAirborneControlMultiplier() : 1.0f;
	MovementComponent->RotationRate = FRotator(0.0f, GetBodyTurnRateDegrees() * GrabTurnMultiplier * AirborneTurnMultiplier, 0.0f);
	LocomotionState.BodyYawDeltaDegrees = FMath::FindDeltaAngleDegrees(
		OwnerCharacter->GetActorRotation().Yaw,
		TargetBodyYaw);
}

float UVNHAlienLocomotionComponent::GetAirborneControlMultiplier() const
{
	float ControlMultiplier = bExternalLaunchDetected ? ExternalLaunchAirControlMultiplier : 1.0f;
	ControlMultiplier *= FMath::Lerp(1.0f, FullTetherTensionAirControlMultiplier, TetherTensionNormalized);

	if (const AActor* OwnerActor = GetOwner())
	{
		if (const UPPGrabberComponent* GrabberComponent = OwnerActor->FindComponentByClass<UPPGrabberComponent>())
		{
			const EPPGrabState CurrentGrabState = GrabberComponent->GetGrabState_Implementation();
			const FPPGrabProfile GrabProfile = GrabberComponent->GetActiveGrabProfile();
			if ((CurrentGrabState == EPPGrabState::HoldingItem || CurrentGrabState == EPPGrabState::PushingObject)
				&& (GrabProfile.bRequiresTwoHands || GrabProfile.MovementSpeedMultiplier <= 0.70f))
			{
				ControlMultiplier *= HeavyCarryAirControlMultiplier;
			}
		}
	}
	return FMath::Clamp(ControlMultiplier, 0.0f, 1.0f);
}

bool UVNHAlienLocomotionComponent::IsControlledAirSteeringAllowed() const
{
	const AActor* OwnerActor = GetOwner();
	const UPPPhysicalStateComponent* PhysicalState = UPPPhysicalStateComponent::FindPhysicalStateComponent(OwnerActor);
	const UPPPlayerActionRouterComponent* ActionRouter = OwnerActor
		? OwnerActor->FindComponentByClass<UPPPlayerActionRouterComponent>()
		: nullptr;
	return (!PhysicalState || (!PhysicalState->IsRagdolled() && !PhysicalState->IsUnconscious()))
		&& (!ActionRouter || !ActionRouter->IsAirDiveActive());
}

void UVNHAlienLocomotionComponent::UpdateTetherTensionFromWorld()
{
	TetherTensionNormalized = 0.0f;
	UWorld* World = GetWorld();
	const UPPTeamMemberComponent* TeamMember = GetOwner()
		? GetOwner()->FindComponentByClass<UPPTeamMemberComponent>()
		: nullptr;
	if (!World || !TeamMember)
	{
		return;
	}

	const int32 OwnerTeamId = TeamMember->GetTeamId_Implementation();
	if (OwnerTeamId == INDEX_NONE)
	{
		return;
	}
	for (TActorIterator<AActor> ActorIterator(World); ActorIterator; ++ActorIterator)
	{
		AActor* CandidateActor = *ActorIterator;
		UFunction* PresentationFunction = CandidateActor
			? CandidateActor->FindFunction(TEXT("GetTetherPresentation"))
			: nullptr;
		if (!PresentationFunction)
		{
			continue;
		}

		FStructOnScope Parameters(PresentationFunction);
		CandidateActor->ProcessEvent(PresentationFunction, Parameters.GetStructMemory());
		const FIntProperty* TeamIdProperty = FindFProperty<FIntProperty>(PresentationFunction, TEXT("TeamId"));
		const FFloatProperty* TensionProperty = FindFProperty<FFloatProperty>(PresentationFunction, TEXT("TensionNormalized"));
		const FBoolProperty* ConnectedProperty = FindFProperty<FBoolProperty>(PresentationFunction, TEXT("Connected"));
		if (!TeamIdProperty || !TensionProperty || !ConnectedProperty)
		{
			continue;
		}

		const uint8* ParameterMemory = Parameters.GetStructMemory();
		const int32 TetherTeamId = TeamIdProperty->GetPropertyValue_InContainer(ParameterMemory);
		const bool bConnected = ConnectedProperty->GetPropertyValue_InContainer(ParameterMemory);
		if (bConnected && TetherTeamId == OwnerTeamId)
		{
			TetherTensionNormalized = FMath::Clamp(
				TensionProperty->GetPropertyValue_InContainer(ParameterMemory),
				0.0f,
				1.0f);
			return;
		}
	}
}

void UVNHAlienLocomotionComponent::UpdateAnimationRate()
{
	if (!OwnerCharacter.IsValid() || !OwnerCharacter->GetMesh())
	{
		return;
	}

	const float UnstableAnimRate = FMath::Lerp(MinAlienAnimRate, MaxAlienAnimRate, 0.5f + 0.5f * FMath::Sin(WobblePhaseRadians * 2.3f));
	const float TargetAnimRate = FMath::Lerp(1.0f, UnstableAnimRate, LocomotionState.Instability);
	OwnerCharacter->GetMesh()->GlobalAnimRateScale = TargetAnimRate;
}

float UVNHAlienLocomotionComponent::GetWalkSpeed() const
{
	return TuningData ? TuningData->WalkSpeed : DefaultWalkSpeed;
}

float UVNHAlienLocomotionComponent::GetMinWalkSpeed() const
{
	return TuningData ? TuningData->MinWalkSpeed : DefaultMinWalkSpeed;
}

float UVNHAlienLocomotionComponent::GetFastWalkSpeed() const
{
	return TuningData ? TuningData->FastWalkSpeed : DefaultFastWalkSpeed;
}

float UVNHAlienLocomotionComponent::GetAcceleration() const
{
	return TuningData ? TuningData->Acceleration : DefaultAcceleration;
}

float UVNHAlienLocomotionComponent::GetCoastBraking() const
{
	return TuningData ? TuningData->CoastBraking : DefaultCoastBraking;
}

float UVNHAlienLocomotionComponent::GetManualBrakeMultiplier() const
{
	return TuningData ? TuningData->ManualBrakeMultiplier : DefaultManualBrakeMultiplier;
}

float UVNHAlienLocomotionComponent::GetBodyTurnRateDegrees() const
{
	return TuningData ? TuningData->BodyTurnRateDegrees : DefaultBodyTurnRateDegrees;
}

float UVNHAlienLocomotionComponent::GetCorrectionStepAngleDegrees() const
{
	return TuningData ? TuningData->CorrectionStepAngleDegrees : DefaultCorrectionStepAngleDegrees;
}
