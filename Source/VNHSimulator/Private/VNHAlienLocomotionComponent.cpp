#include "VNHAlienLocomotionComponent.h"

#include "GameFramework/Character.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/Controller.h"
#include "Components/SkeletalMeshComponent.h"
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

	const FVector DesiredDirection = BuildCameraRelativeMoveDirection();
	UpdateInstability(DeltaTime, DesiredDirection);
	UpdateSpeed(DeltaTime, DesiredDirection);
	ApplyMovement(DesiredDirection);
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

bool UVNHAlienLocomotionComponent::ShouldDriveLocomotion() const
{
	if (!OwnerCharacter.IsValid() || !MovementComponent.IsValid())
	{
		return false;
	}

	const AController* Controller = OwnerCharacter->GetController();
	return Controller && Controller->IsPlayerController() && OwnerCharacter->IsLocallyControlled();
}

FVector UVNHAlienLocomotionComponent::BuildCameraRelativeMoveDirection() const
{
	if (!OwnerCharacter.IsValid())
	{
		return FVector::ZeroVector;
	}

	const AController* Controller = OwnerCharacter->GetController();
	const FRotator ControlRotation = Controller ? Controller->GetControlRotation() : OwnerCharacter->GetActorRotation();
	const FRotator YawRotation(0.0f, ControlRotation.Yaw, 0.0f);

	const FVector Forward = FRotationMatrix(YawRotation).GetUnitAxis(EAxis::X);
	const FVector Right = FRotationMatrix(YawRotation).GetUnitAxis(EAxis::Y);
	const FVector DesiredDirection = Forward * MoveInput.Y + Right * MoveInput.X;

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
		MovementComponent->MaxAcceleration = GetAcceleration();
		MovementComponent->BrakingDecelerationWalking = GetCoastBraking() * (LocomotionState.bManualBrake ? GetManualBrakeMultiplier() : 1.0f);
	}
}

void UVNHAlienLocomotionComponent::ApplyMovement(const FVector& DesiredDirection)
{
	if (!OwnerCharacter.IsValid() || !MovementComponent.IsValid() || DesiredDirection.IsNearlyZero())
	{
		return;
	}

	const FVector DriftRight = FVector::CrossProduct(FVector::UpVector, DesiredDirection).GetSafeNormal();
	const float DriftAmount = FMath::Sin(WobblePhaseRadians) * LateralDriftStrength * LocomotionState.Instability;
	const FVector UnstableDirection = (DesiredDirection + DriftRight * DriftAmount).GetSafeNormal();
	const float UnevenStride = 1.0f + FMath::Sin(WobblePhaseRadians * 1.7f) * UnevenStrideStrength * LocomotionState.Instability;

	MovementComponent->MaxWalkSpeed = FMath::Max(LocomotionState.CurrentSpeed * UnevenStride, 1.0f);
	OwnerCharacter->AddMovementInput(UnstableDirection, 1.0f);
}

void UVNHAlienLocomotionComponent::UpdateBodyFacing(float DeltaTime, const FVector& DesiredDirection)
{
	if (!OwnerCharacter.IsValid() || DesiredDirection.IsNearlyZero())
	{
		return;
	}

	const FRotator CurrentRotation = OwnerCharacter->GetActorRotation();
	const FRotator TargetRotation(0.0f, DesiredDirection.Rotation().Yaw + LocomotionState.WobbleDegrees, 0.0f);
	LocomotionState.BodyYawDeltaDegrees = FMath::FindDeltaAngleDegrees(CurrentRotation.Yaw, TargetRotation.Yaw);
	const FRotator NewRotation = FMath::RInterpConstantTo(CurrentRotation, TargetRotation, DeltaTime, GetBodyTurnRateDegrees());
	OwnerCharacter->SetActorRotation(FRotator(0.0f, NewRotation.Yaw, 0.0f));
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
