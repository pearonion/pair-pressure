#include "VNHShopperCharacter.h"

#include "Camera/CameraComponent.h"
#include "Animation/AnimationAsset.h"
#include "Animation/AnimInstance.h"
#include "Animation/AnimMontage.h"
#include "Animation/AnimSequence.h"
#include "Components/AudioComponent.h"
#include "Components/CapsuleComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "DrawDebugHelpers.h"
#include "Engine/DataTable.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMesh.h"
#include "EngineUtils.h"
#include "Animation/AnimTypes.h"
#include "GameFramework/Controller.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/SpringArmComponent.h"
#include "Kismet/GameplayStatics.h"
#include "Sound/SoundAttenuation.h"
#include "Sound/SoundBase.h"
#include "Sound/SoundWaveProcedural.h"
#include "TimerManager.h"
#include "UObject/ConstructorHelpers.h"
#include "UObject/UnrealType.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "PairPressure/PPCarryComponent.h"
#include "PairPressure/PPGrabberComponent.h"
#include "PairPressure/PPGrabbableComponent.h"
#include "PairPressure/PPGameplayTypes.h"
#include "PairPressure/PPImpactSensorComponent.h"
#include "PairPressure/PPPhysicalStateComponent.h"
#include "PairPressure/PPPlayerActionRouterComponent.h"
#include "PairPressure/PPTeamMemberComponent.h"
#include "PhysicsEngine/PhysicsHandleComponent.h"
#include "PhysicsEngine/PhysicsAsset.h"
#include "VNHAlienLocomotionComponent.h"
#include "VNHGameState.h"
#include "VNHLog.h"
#include "VNHPlayerState.h"
#include "VNHShopperAIController.h"
#include "VNHShopperWaypoint.h"
#include "Net/UnrealNetwork.h"

namespace
{
const TCHAR* DefaultBodyMeshPath = TEXT("/Game/CuteChubbyPenguin/Penguin/Meshes/SK_Penguin_UE.SK_Penguin_UE");
const TCHAR* DefaultMascotAnimBlueprintPath = TEXT("/Game/PairPressure/Characters/Penguin/ABP_Penguin.ABP_Penguin_C");
const TCHAR* DefaultMascotIdleAnimPath = TEXT("/Game/CuteChubbyPenguin/Penguin/Animations/AS_Penguin_UE_Anim_idle1.AS_Penguin_UE_Anim_idle1");
const TCHAR* PairPressureMascotPhysicsAssetPath = TEXT("/Game/CuteChubbyPenguin/Penguin/Meshes/PHYS_Penguin_UE_PhysicsAsset.PHYS_Penguin_UE_PhysicsAsset");
constexpr float PPShopperObstacleFallPlayRate = 1.15f;
// Leave several rendered frames before the sequence boundary. A 0.03-second
// lead can expire inside one 30 Hz frame at the authored 1.15 play rate, which
// lets the dynamic montage naturally end before its hold timer can freeze it.
constexpr float PPShopperObstacleFallPoseLeadSeconds = 0.12f;
constexpr int32 PPShopperObstacleFallDirectionCount = 4;
constexpr float CalmFartVolume = 1.4f;
constexpr float PanicFartVolume = 2.6f;
constexpr float CalmFartInnerRadius = 275.0f;
constexpr float PanicFartInnerRadius = 700.0f;
constexpr float FartCloudBackwardOffset = 58.0f;
constexpr float FartCloudHeightOffset = 35.0f;
constexpr float FartCloudKnockdownRadius = 700.0f;
constexpr float ManualFartSpamWindowSeconds = 6.0f;
constexpr float ManualFartCooldownReadyTolerance = 0.05f;
constexpr float ManualFartCooldownSeconds[] = {3.0f, 3.0f, 5.0f, 8.0f, 12.0f, 18.0f};

bool IsComposureLockedForRoundPhase(const UWorld* World)
{
	const AVNHGameState* VNHGameState = World ? World->GetGameState<AVNHGameState>() : nullptr;
	if (!VNHGameState)
	{
		return false;
	}

	const EVNHRoundPhase RoundPhase = VNHGameState->GetRoundPhase();
	return RoundPhase == EVNHRoundPhase::WaitingForPlayers
		|| RoundPhase == EVNHRoundPhase::AssigningRoles;
}
constexpr float CalmFartFalloffDistance = 325.0f;
constexpr float PanicFartFalloffDistance = 1100.0f;

float GetManualFartCooldownForStreak(int32 StreakIndex)
{
	constexpr int32 MaxCooldownIndex = UE_ARRAY_COUNT(ManualFartCooldownSeconds) - 1;
	return ManualFartCooldownSeconds[FMath::Clamp(StreakIndex, 0, MaxCooldownIndex)];
}

const FArrayProperty* FindFartKnockdownArrayFieldByPrefix(const UScriptStruct* RowStruct, const TCHAR* FieldPrefix)
{
	if (!RowStruct || !FieldPrefix)
	{
		return nullptr;
	}

	const FString PrefixString(FieldPrefix);
	for (TFieldIterator<FProperty> It(RowStruct); It; ++It)
	{
		const FArrayProperty* ArrayProperty = CastField<FArrayProperty>(*It);
		if (ArrayProperty && ArrayProperty->GetName().StartsWith(PrefixString))
		{
			return ArrayProperty;
		}
	}

	return nullptr;
}

UAnimMontage* ChooseFartKnockdownMontageFromField(const uint8* RowData, const FArrayProperty* ArrayProperty)
{
	if (!RowData || !ArrayProperty)
	{
		return nullptr;
	}

	const FObjectPropertyBase* ObjectProperty = CastField<FObjectPropertyBase>(ArrayProperty->Inner);
	if (!ObjectProperty || !ObjectProperty->PropertyClass || !ObjectProperty->PropertyClass->IsChildOf(UAnimMontage::StaticClass()))
	{
		return nullptr;
	}

	FScriptArrayHelper ArrayHelper(ArrayProperty, ArrayProperty->ContainerPtrToValuePtr<void>(RowData));
	TArray<UAnimMontage*> Montages;
	for (int32 Index = 0; Index < ArrayHelper.Num(); ++Index)
	{
		if (UAnimMontage* Montage = Cast<UAnimMontage>(ObjectProperty->GetObjectPropertyValue(ArrayHelper.GetRawPtr(Index))))
		{
			Montages.Add(Montage);
		}
	}

	if (Montages.IsEmpty())
	{
		return nullptr;
	}

	return Montages[FMath::RandRange(0, Montages.Num() - 1)];
}
}

AVNHShopperCharacter::AVNHShopperCharacter()
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.TickInterval = 0.25f;
	bReplicates = true;
	AIControllerClass = AVNHShopperAIController::StaticClass();
	AutoPossessAI = EAutoPossessAI::PlacedInWorldOrSpawned;
	bUseControllerRotationYaw = false;
	JumpMaxHoldTime = 0.0f;
	JumpMaxCount = 1;

	if (UCapsuleComponent* ShopperCapsule = GetCapsuleComponent())
	{
		ShopperCapsule->SetCollisionResponseToChannel(ECC_Camera, ECR_Ignore);
	}

	if (UCharacterMovementComponent* MovementComponent = GetCharacterMovement())
	{
		MovementComponent->bOrientRotationToMovement = false;
		MovementComponent->RotationRate = FRotator(0.0f, 360.0f, 0.0f);
		MovementComponent->MaxWalkSpeed = 500.0f;
		MovementComponent->MaxAcceleration = 850.0f;
		MovementComponent->BrakingDecelerationWalking = 520.0f;
		MovementComponent->GroundFriction = 3.5f;
		MovementComponent->BrakingFrictionFactor = 1.0f;
		MovementComponent->JumpZVelocity = 650.0f;
		MovementComponent->GravityScale = 1.65f;
		MovementComponent->AirControl = 0.24f;
		MovementComponent->FallingLateralFriction = 0.15f;
		MovementComponent->NavAgentProps.bCanCrouch = true;
	}

	RoutineComponent = CreateDefaultSubobject<UVNHRoutineComponent>(TEXT("RoutineComponent"));
	AlienLocomotionComponent = CreateDefaultSubobject<UVNHAlienLocomotionComponent>(TEXT("AlienLocomotionComponent"));
	PairPressurePhysicalStateComponent = CreateDefaultSubobject<UPPPhysicalStateComponent>(TEXT("PairPressurePhysicalState"));
	PairPressureTeamMember = CreateDefaultSubobject<UPPTeamMemberComponent>(TEXT("PairPressureTeamMember"));
	PairPressureCarry = CreateDefaultSubobject<UPPCarryComponent>(TEXT("PairPressureCarry"));
	PairPressureGrabPhysicsHandle = CreateDefaultSubobject<UPhysicsHandleComponent>(TEXT("PairPressureGrabPhysicsHandle"));
	PairPressureGrabber = CreateDefaultSubobject<UPPGrabberComponent>(TEXT("PairPressureGrabber"));
	PairPressureGrabbable = CreateDefaultSubobject<UPPGrabbableComponent>(TEXT("PairPressureGrabbable"));
	PairPressureGrabbable->GrabProfile.TargetType = EPPGrabTargetType::Player;
	PairPressureGrabbable->GrabProfile.MaximumRange = 260.0f;
	PairPressureGrabbable->GrabProfile.MaximumAngleDegrees = 65.0f;
	PairPressureGrabbable->GrabProfile.LinearStiffness = 5200.0f;
	PairPressureGrabbable->GrabProfile.LinearDamping = 900.0f;
	PairPressureGrabbable->GrabProfile.BreakForce = 1250000.0f;
	PairPressureGrabbable->GrabProfile.MovementSpeedMultiplier = 0.72f;
	PairPressureGrabbable->GripPointComponentName = TEXT("GrabAnchor");
	PairPressureImpactSensor = CreateDefaultSubobject<UPPImpactSensorComponent>(TEXT("PairPressureImpactSensor"));
	PairPressureActionRouter = CreateDefaultSubobject<UPPPlayerActionRouterComponent>(TEXT("PairPressureActionRouter"));

	FollowCameraBoom = CreateDefaultSubobject<USpringArmComponent>(TEXT("FollowCameraBoom"));
	FollowCameraBoom->SetupAttachment(RootComponent);
	FollowCameraBoom->TargetArmLength = 620.0f;
	FollowCameraBoom->TargetOffset = FVector(0.0f, 0.0f, 112.0f);
	FollowCameraBoom->SetRelativeRotation(FRotator(-28.0f, 0.0f, 0.0f));
	FollowCameraBoom->bUsePawnControlRotation = true;
	FollowCameraBoom->bDoCollisionTest = true;
	FollowCameraBoom->ProbeSize = 18.0f;
	FollowCameraBoom->ProbeChannel = ECC_Camera;
	FollowCameraBoom->bEnableCameraLag = false;
	FollowCameraBoom->CameraLagSpeed = 12.0f;

	FollowCamera = CreateDefaultSubobject<UCameraComponent>(TEXT("FollowCamera"));
	FollowCamera->SetupAttachment(FollowCameraBoom, USpringArmComponent::SocketName);
	FollowCamera->bUsePawnControlRotation = false;
	FollowCamera->FieldOfView = 85.0f;
	FollowCamera->PostProcessSettings.bOverride_MotionBlurAmount = true;
	FollowCamera->PostProcessSettings.MotionBlurAmount = 0.0f;
	FollowCamera->PostProcessSettings.bOverride_MotionBlurMax = true;
	FollowCamera->PostProcessSettings.MotionBlurMax = 0.0f;
	FollowCamera->PostProcessSettings.bOverride_MotionBlurPerObjectSize = true;
	FollowCamera->PostProcessSettings.MotionBlurPerObjectSize = 0.0f;

	FirstPersonCamera = CreateDefaultSubobject<UCameraComponent>(TEXT("FirstPersonCamera"));
	FirstPersonCamera->SetupAttachment(RootComponent);
	FirstPersonCamera->SetRelativeLocation(FVector(8.0f, 0.0f, 64.0f));
	FirstPersonCamera->bUsePawnControlRotation = true;
	FirstPersonCamera->PostProcessSettings.bOverride_MotionBlurAmount = true;
	FirstPersonCamera->PostProcessSettings.MotionBlurAmount = 0.0f;
	FirstPersonCamera->PostProcessSettings.bOverride_MotionBlurMax = true;
	FirstPersonCamera->PostProcessSettings.MotionBlurMax = 0.0f;
	FirstPersonCamera->PostProcessSettings.bOverride_MotionBlurPerObjectSize = true;
	FirstPersonCamera->PostProcessSettings.MotionBlurPerObjectSize = 0.0f;
	FirstPersonCamera->SetAutoActivate(false);

	DebugBodyMesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("DebugBodyMesh"));
	DebugBodyMesh->SetupAttachment(RootComponent);
	DebugBodyMesh->SetRelativeLocation(FVector(0.0f, 0.0f, -35.0f));
	DebugBodyMesh->SetRelativeScale3D(FVector(0.55f, 0.55f, 1.55f));
	DebugBodyMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	DebugBodyMesh->SetHiddenInGame(false);

	auto CreateCosmeticMesh = [this](const TCHAR* ComponentName)
	{
		USkeletalMeshComponent* NewComponent = CreateDefaultSubobject<USkeletalMeshComponent>(ComponentName);
		NewComponent->SetupAttachment(GetMesh());
		NewComponent->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		NewComponent->SetGenerateOverlapEvents(false);
		NewComponent->VisibilityBasedAnimTickOption = EVisibilityBasedAnimTickOption::AlwaysTickPoseAndRefreshBones;
		NewComponent->bEnableUpdateRateOptimizations = false;
		return NewComponent;
	};

	HairMeshComponent = CreateCosmeticMesh(TEXT("HairMesh"));
	FaceMeshComponent = CreateCosmeticMesh(TEXT("FaceMesh"));
	HatMeshComponent = CreateCosmeticMesh(TEXT("HatMesh"));
	MustacheMeshComponent = CreateCosmeticMesh(TEXT("MustacheMesh"));
	OutfitMeshComponent = CreateCosmeticMesh(TEXT("OutfitMesh"));
	OutwearMeshComponent = CreateCosmeticMesh(TEXT("OutwearMesh"));
	PantsMeshComponent = CreateCosmeticMesh(TEXT("PantsMesh"));
	ShoesMeshComponent = CreateCosmeticMesh(TEXT("ShoesMesh"));
	AccessoryMeshComponent = CreateCosmeticMesh(TEXT("AccessoryMesh"));

	CharacterCustomization.BodyMesh = TSoftObjectPtr<USkeletalMesh>(FSoftObjectPath(DefaultBodyMeshPath));
	HumanActionAnimationTable = TSoftObjectPtr<UDataTable>(FSoftObjectPath(TEXT("/Game/Data/DT_HumanActionAnimations.DT_HumanActionAnimations")));
	AlienActionAnimationTable = TSoftObjectPtr<UDataTable>(FSoftObjectPath(TEXT("/Game/Data/DT_AlienActionAnimations.DT_AlienActionAnimations")));
	MascotAnimationTable = TSoftObjectPtr<UDataTable>(FSoftObjectPath(TEXT("/Game/PairPressure/Data/DT_MascotAnimations.DT_MascotAnimations")));

	if (USkeletalMesh* DefaultBodyMesh = LoadObject<USkeletalMesh>(nullptr, DefaultBodyMeshPath))
	{
		GetMesh()->SetSkeletalMesh(DefaultBodyMesh);
		ConfigureCharacterVisuals();
	}
	else
	{
		static ConstructorHelpers::FObjectFinder<UStaticMesh> DebugBodyMeshFinder(TEXT("/Engine/BasicShapes/Cylinder.Cylinder"));
		if (DebugBodyMeshFinder.Succeeded())
		{
			DebugBodyMesh->SetStaticMesh(DebugBodyMeshFinder.Object);
		}
	}

	static ConstructorHelpers::FObjectFinder<USoundBase> FartSoundFinder(TEXT("/Game/UI/Sounds/fartsound.fartsound"));
	if (FartSoundFinder.Succeeded())
	{
		FartSound = FartSoundFinder.Object;
	}
}

void AVNHShopperCharacter::BeginPlay()
{
	Super::BeginPlay();
	// Do this at runtime as well as on the CDO so an inherited Blueprint cannot
	// reintroduce variable-height jumps through a stale default value.
	JumpMaxHoldTime = 0.0f;
	JumpMaxCount = 1;
	if (UCharacterMovementComponent* MovementComponent = GetCharacterMovement())
	{
		MovementComponent->JumpZVelocity = 650.0f;
	}

	ConfigureCharacterVisuals();
	ApplyCharacterCustomization();
	if (ShouldUsePairPressureMascotVisuals())
	{
		// Load before the first input so diving never waits on synchronous asset IO.
		if (const FPPMascotAnimationRow* ActiveMascotRow = ResolveActiveMascotRow(TEXT("Preload mascot dive")))
		{
			PairPressureDiveAnimation = ActiveMascotRow->Dive.LoadSynchronous();
		}
		if (!PairPressureDiveAnimation)
		{
			PairPressureDiveAnimation = LoadObject<UAnimSequence>(
				nullptr,
				TEXT("/Game/CuteChubbyPenguin/Penguin/Animations/AS_Penguin_UE_falls_forward.AS_Penguin_UE_falls_forward"));
		}
		PreloadPairPressureObstacleFallAnimations();
		// Camera anchoring must follow physics every frame while the capsule is
		// disabled and the mascot body is being propelled or carried.
		PrimaryActorTick.TickInterval = 0.0f;
		if (PairPressureGrabber)
		{
			PairPressureGrabber->OnGrabStateChanged.AddUniqueDynamic(this, &AVNHShopperCharacter::HandlePairPressureGrabStateChanged);
			PairPressureGrabber->OnGrabFailed.AddUniqueDynamic(this, &AVNHShopperCharacter::HandlePairPressureGrabFailed);
			PairPressureGrabber->OnGrabReleasedPresentation.AddUniqueDynamic(this, &AVNHShopperCharacter::HandlePairPressureGrabReleasedPresentation);
			PairPressureGrabber->OnGrabThrowPresentation.AddUniqueDynamic(this, &AVNHShopperCharacter::HandlePairPressureGrabThrowPresentation);
		}
		if (PairPressureActionRouter)
		{
			PairPressureActionRouter->OnAirDiveStateChanged.AddUniqueDynamic(this, &AVNHShopperCharacter::HandlePairPressureAirDiveStateChanged);
			PairPressureActionRouter->OnAirDiveRecoveryStateChanged.AddUniqueDynamic(this, &AVNHShopperCharacter::HandlePairPressureAirDiveRecoveryStateChanged);
		}
	}
	LastMeaningfulLocation = GetActorLocation();
	LastMeaningfulControlRotation = GetControlRotation();
	UpdateComposureState();

	if (HasAuthority() && !bPossessedByAlien && RoutineComponent && RoutineComponent->GetCurrentWaypoint())
	{
		if (!GetController())
		{
			SpawnDefaultController();
		}

		FTimerHandle RoutineStartupTimerHandle;
		GetWorldTimerManager().SetTimer(RoutineStartupTimerHandle, this, &AVNHShopperCharacter::StartRoutineMovement, 2.0f, false);
	}
}

void AVNHShopperCharacter::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	UpdateRagdollCameraAnchor(DeltaSeconds);
	UpdateAdaptiveFollowCamera(DeltaSeconds);
	UpdateCourseObstacleInteractions();
	UpdatePairPressureAnimationPresentation();

	const bool bIsPairPressureMap = GetWorld() && GetWorld()->GetMapName().Contains(TEXT("PP_"));
	if (!bIsPairPressureMap && HasAuthority() && IsPlayerControlled())
	{
		UpdateComposureSystem(DeltaSeconds);
	}
}

void AVNHShopperCharacter::PossessedBy(AController* NewController)
{
	Super::PossessedBy(NewController);

	if (const AVNHPlayerState* MascotPlayerState = GetPlayerState<AVNHPlayerState>();
		MascotPlayerState && MascotPlayerState->GetClass()->ImplementsInterface(UPPMascotSelectionInterface::StaticClass()))
	{
		ApplySelectedMascotRowName_Implementation(
			IPPMascotSelectionInterface::Execute_GetSelectedMascotRowName(MascotPlayerState));
	}

	if (HasAuthority() && NewController && NewController->IsPlayerController())
	{
		bActNaturalAvailable = true;

		const FString MapName = GetWorld() ? GetWorld()->GetMapName() : FString();
		if (MapName.Contains(TEXT("Lobby")))
		{
			SetPossessedByAlien(true);
		}
	}
}

void AVNHShopperCharacter::UnPossessed()
{
	HandleJumpInputReleased();
	SetFirstPersonViewEnabled(false);
	Super::UnPossessed();
}

void AVNHShopperCharacter::SetFirstPersonViewEnabled(bool bEnabled)
{
	if (bFirstPersonViewEnabled == bEnabled)
	{
		return;
	}

	bFirstPersonViewEnabled = bEnabled;
	bUseControllerRotationYaw = bEnabled;

	if (FollowCamera)
	{
		FollowCamera->SetActive(!bEnabled);
	}

	if (FirstPersonCamera)
	{
		FirstPersonCamera->SetActive(bEnabled);
	}

	if (USkeletalMeshComponent* MeshComponent = GetMesh())
	{
		MeshComponent->SetOwnerNoSee(bEnabled);
	}

	if (DebugBodyMesh)
	{
		DebugBodyMesh->SetOwnerNoSee(bEnabled);
	}

	UE_LOG(
		LogVNH,
		Display,
		TEXT("CameraMode: Pawn=%s FirstPerson=%s FollowActive=%s FirstPersonActive=%s"),
		*GetNameSafe(this),
		bEnabled ? TEXT("true") : TEXT("false"),
		FollowCamera && FollowCamera->IsActive() ? TEXT("true") : TEXT("false"),
		FirstPersonCamera && FirstPersonCamera->IsActive() ? TEXT("true") : TEXT("false"));
}

void AVNHShopperCharacter::StabilizePairPressureRecoveryCamera(
	const FTransform& PreservedCameraBoomWorldTransform)
{
	if (!IsLocallyControlled() || !FollowCameraBoom
		|| PreservedCameraBoomWorldTransform.ContainsNaN())
	{
		return;
	}

	// Authority recovery may run from a late-frame timer after the spring arm has
	// already updated. Preserve rotation as well as position so the capsule's new
	// standing yaw cannot leak into one rendered host frame before the next boom
	// tick. Collision stays off for the short authored blend below.
	FollowCameraBoom->bDoCollisionTest = false;
	FollowCameraBoom->SetWorldTransform(
		PreservedCameraBoomWorldTransform,
		false,
		nullptr,
		ETeleportType::TeleportPhysics);
}

void AVNHShopperCharacter::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);

	DOREPLIFETIME(AVNHShopperCharacter, bPossessedByAlien);
	DOREPLIFETIME(AVNHShopperCharacter, bActNaturalAvailable);
	DOREPLIFETIME(AVNHShopperCharacter, bFrozenByPublicTest);
	DOREPLIFETIME(AVNHShopperCharacter, Composure);
	DOREPLIFETIME(AVNHShopperCharacter, ComposureState);
	DOREPLIFETIME(AVNHShopperCharacter, InactivitySeconds);
	DOREPLIFETIME(AVNHShopperCharacter, CurrentFartThreshold);
	DOREPLIFETIME(AVNHShopperCharacter, FartCooldownRemaining);
	DOREPLIFETIME(AVNHShopperCharacter, LastUniversalAction);
	DOREPLIFETIME(AVNHShopperCharacter, HeldProps);
	DOREPLIFETIME(AVNHShopperCharacter, CharacterCustomization);
}

void AVNHShopperCharacter::SetPossessedByAlien(bool bNewPossessedByAlien)
{
	if (!HasAuthority() || bPossessedByAlien == bNewPossessedByAlien)
	{
		return;
	}

	bPossessedByAlien = bNewPossessedByAlien;
	bActNaturalAvailable = bNewPossessedByAlien;

	if (RoutineComponent)
	{
		if (bPossessedByAlien)
		{
			RoutineComponent->PauseRoutineForPossession();
		}
		else
		{
			RoutineComponent->ResumeRoutineAfterPossession();
		}
	}

	OnRep_PossessedByAlien();
}

void AVNHShopperCharacter::UpdateAdaptiveFollowCamera(float DeltaSeconds)
{
	const bool bLocallyControlledCharacter = IsLocallyControlled();
	if (!bLocallyControlledCharacter && !HasAuthority())
	{
		return;
	}

	bool bInsideCourseCameraVolume = false;
	bool bInsideSpinningTube = false;
	if (!bCourseCameraActorsCached)
	{
		if (UWorld* World = GetWorld())
		{
			for (TActorIterator<AActor> ActorIterator(World); ActorIterator; ++ActorIterator)
			{
				AActor* CourseActor = *ActorIterator;
				if (!CourseActor)
				{
					continue;
				}
				const FString CourseActorName = CourseActor->GetName();
				if (CourseActorName.Contains(TEXT("SpinningTube"), ESearchCase::IgnoreCase)
					|| CourseActorName.Contains(TEXT("Pusher_V2"), ESearchCase::IgnoreCase)
					|| CourseActorName.Contains(TEXT("Drop_V1"), ESearchCase::IgnoreCase))
				{
					CourseCameraActors.Add(CourseActor);
				}
			}
			bCourseCameraActorsCached = true;
		}
	}

	for (const TWeakObjectPtr<AActor>& WeakCourseActor : CourseCameraActors)
	{
		AActor* CourseActor = WeakCourseActor.Get();
		if (!CourseActor)
		{
			continue;
		}
		const bool bSpinningTube = CourseActor->GetName().Contains(TEXT("SpinningTube"), ESearchCase::IgnoreCase);
		FVector BoundsOrigin;
		FVector BoundsExtent;
		CourseActor->GetActorBounds(true, BoundsOrigin, BoundsExtent);
		const float BoundsPadding = bSpinningTube ? 140.0f : 80.0f;
		if (FBox::BuildAABB(BoundsOrigin, BoundsExtent + FVector(BoundsPadding)).IsInsideOrOn(GetActorLocation()))
		{
			bInsideCourseCameraVolume = true;
			bInsideSpinningTube |= bSpinningTube;
		}
	}
	if (UCharacterMovementComponent* MovementComponent = GetCharacterMovement())
	{
		if (bInsideSpinningTube && !bIsCrouched && !bAutoCrouchedForCourseTube)
		{
			// The authored tube opening is lower than the standing capsule. Use the
			// engine's collision-safe crouch path at full run speed before the entrance,
			// then restore only the settings this course helper changed on exit.
			CourseTubePreviousCrouchedHalfHeight = MovementComponent->GetCrouchedHalfHeight();
			CourseTubePreviousMaxWalkSpeedCrouched = MovementComponent->MaxWalkSpeedCrouched;
			MovementComponent->SetCrouchedHalfHeight(62.0f);
			MovementComponent->MaxWalkSpeedCrouched = MovementComponent->MaxWalkSpeed;
			Crouch(false);
			bAutoCrouchedForCourseTube = true;
		}
		else if (!bInsideSpinningTube && bAutoCrouchedForCourseTube)
		{
			UnCrouch(false);
			if (!bIsCrouched)
			{
				MovementComponent->SetCrouchedHalfHeight(CourseTubePreviousCrouchedHalfHeight);
				MovementComponent->MaxWalkSpeedCrouched = CourseTubePreviousMaxWalkSpeedCrouched;
				bAutoCrouchedForCourseTube = false;
			}
		}
	}
	if (!bLocallyControlledCharacter || !FollowCameraBoom || !FollowCamera || !FollowCamera->IsActive())
	{
		return;
	}
	const bool bRagdollRecoveryCameraHandoffActive = PairPressurePhysicalStateComponent
		&& PairPressurePhysicalStateComponent->IsRagdollRecoveryBlendActive();
	if (bRagdollRecoveryCameraHandoffActive)
	{
		// Do not let a transient compression sample alter arm length, socket height,
		// or control pitch while the capsule is moving underneath the preserved
		// world-space camera frame. Normal adaptive behavior resumes next tick after
		// the physical-state blend completes.
		FollowCameraBoom->bDoCollisionTest = false;
		return;
	}
	FollowCameraBoom->bDoCollisionTest = !bInsideCourseCameraVolume;
	FollowCameraBoom->TargetArmLength = FMath::FInterpTo(
		FollowCameraBoom->TargetArmLength,
		bInsideCourseCameraVolume ? 245.0f : 620.0f,
		DeltaSeconds,
		8.0f);

	const FVector BoomOrigin = FollowCameraBoom->GetComponentLocation() + FollowCameraBoom->TargetOffset;
	const float CurrentCameraDistance = FVector::Distance(BoomOrigin, FollowCamera->GetComponentLocation());
	const float CompressionAlpha = 1.0f - FMath::Clamp(
		CurrentCameraDistance / FMath::Max(FollowCameraBoom->TargetArmLength, 1.0f),
		0.0f,
		1.0f);
	const float DesiredExtraHeight = FMath::SmoothStep(0.15f, 0.8f, CompressionAlpha) * 125.0f;
	const float OverheadAlpha = FMath::SmoothStep(0.2f, 0.85f, CompressionAlpha);
	const float NewSocketHeight = FMath::FInterpTo(
		FollowCameraBoom->SocketOffset.Z,
		DesiredExtraHeight,
		DeltaSeconds,
		7.0f);
	FollowCameraBoom->SocketOffset.Z = NewSocketHeight;

	if ((!AlienLocomotionComponent || !AlienLocomotionComponent->IsCameraOrbitDetached())
		&& GetController())
	{
		AController* ShopperController = GetController();
		FRotator AdaptiveControlRotation = ShopperController->GetControlRotation();
		AdaptiveControlRotation.Pitch = FMath::FInterpTo(
			AdaptiveControlRotation.Pitch,
			FMath::Lerp(-28.0f, -58.0f, OverheadAlpha),
			DeltaSeconds,
			6.0f);
		ShopperController->SetControlRotation(AdaptiveControlRotation);
	}
}

void AVNHShopperCharacter::UpdateCourseObstacleInteractions()
{
	if (!HasAuthority() || !GetWorld() || !GetWorld()->GetMapName().Contains(TEXT("Lobby")))
	{
		return;
	}

	UPrimitiveComponent* FloorTile = Cast<UPrimitiveComponent>(GetCharacterMovement()
		? GetCharacterMovement()->GetMovementBaseObject()
		: nullptr);
	if (!FloorTile || CollapsingCourseTiles.Contains(FloorTile))
	{
		return;
	}

	AActor* TileOwner = FloorTile->GetOwner();
	if (!TileOwner || !TileOwner->GetName().Contains(TEXT("Floor_V1"), ESearchCase::IgnoreCase))
	{
		return;
	}

	CollapsingCourseTiles.Add(FloorTile);
	TWeakObjectPtr<UPrimitiveComponent> WeakFloorTile(FloorTile);
	TWeakObjectPtr<AVNHShopperCharacter> WeakShopper(this);
	FTimerHandle CollapseTimerHandle;
	GetWorldTimerManager().SetTimer(
		CollapseTimerHandle,
		[WeakFloorTile, WeakShopper]()
		{
			if (UPrimitiveComponent* CollapsedTile = WeakFloorTile.Get())
			{
				// Visibility must stay local to the touched mesh; propagating it hides
				// attached neighbours without removing their collision.
				CollapsedTile->SetCollisionEnabled(ECollisionEnabled::NoCollision);
				CollapsedTile->SetCollisionResponseToAllChannels(ECR_Ignore);
				CollapsedTile->SetVisibility(false, false);
				if (AVNHShopperCharacter* Shopper = WeakShopper.Get())
				{
					if (UCharacterMovementComponent* MovementComponent = Shopper->GetCharacterMovement();
						MovementComponent && MovementComponent->GetMovementBaseObject() == CollapsedTile)
					{
						MovementComponent->SetBase(static_cast<UPrimitiveComponent*>(nullptr));
						MovementComponent->SetMovementMode(MOVE_Falling);
					}
				}
			}
		},
		0.5f,
		false);
}

void AVNHShopperCharacter::PrepareForPlayerPossession()
{
	if (!HasAuthority())
	{
		return;
	}

	if (RoutineComponent)
	{
		RoutineComponent->PauseRoutineForPossession();
	}

	SetActorEnableCollision(true);
	if (UCapsuleComponent* ShopperCapsule = GetCapsuleComponent())
	{
		ShopperCapsule->SetCollisionProfileName(TEXT("Pawn"));
		ShopperCapsule->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
		ShopperCapsule->UpdateOverlaps();
	}

	if (UCharacterMovementComponent* MovementComponent = GetCharacterMovement())
	{
		MovementComponent->StopMovementImmediately();
		MovementComponent->SetMovementMode(MOVE_Walking);
	}
}

bool AVNHShopperCharacter::UseActNatural()
{
	if ((GetWorld() && GetWorld()->GetMapName().Contains(TEXT("PP_")))
		|| !HasAuthority() || !IsPlayerControlled() || !bActNaturalAvailable || !RoutineComponent)
	{
		return false;
	}

	bActNaturalAvailable = false;
	RegisterMeaningfulAction(EVNHUniversalAction::None, nullptr);
	OnActNaturalUsed.Broadcast(RoutineComponent->ChooseActNaturalRecovery());
	return true;
}

FText AVNHShopperCharacter::GetComposureStateText() const
{
	switch (ComposureState)
	{
	case EVNHComposureState::Calm:
		return NSLOCTEXT("VNH", "ComposureCalm", "CALM");
	case EVNHComposureState::Stable:
		return NSLOCTEXT("VNH", "ComposureStable", "STABLE");
	case EVNHComposureState::Nervous:
		return NSLOCTEXT("VNH", "ComposureNervous", "NERVOUS");
	case EVNHComposureState::Cracking:
		return NSLOCTEXT("VNH", "ComposureCracking", "CRACKING");
	case EVNHComposureState::Panic:
	default:
		return NSLOCTEXT("VNH", "ComposurePanic", "PANIC");
	}
}

void AVNHShopperCharacter::ApplyComposureDelta(float Delta, FName Reason)
{
	if ((GetWorld() && GetWorld()->GetMapName().Contains(TEXT("PP_")))
		|| !HasAuthority() || FMath::IsNearlyZero(Delta))
	{
		return;
	}
	if (Delta < 0.0f && IsComposureLockedForRoundPhase(GetWorld()))
	{
		return;
	}

	const float PreviousComposure = Composure;
	Composure = FMath::Clamp(Composure + Delta, 0.0f, 100.0f);
	if (Delta < 0.0f)
	{
		LastSuspiciousEventTime = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.0f;
	}

	UpdateComposureState();
	UE_LOG(LogVNH, Display, TEXT("Composure: %s %.1f -> %.1f (%s)"), *GetNameSafe(this), PreviousComposure, Composure, *Reason.ToString());
}

void AVNHShopperCharacter::RegisterMeaningfulAction(EVNHUniversalAction Action, AActor* Target)
{
	if (!HasAuthority())
	{
		return;
	}

	const float Now = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.0f;
	const bool bRepeatedSpam = Action != EVNHUniversalAction::None
		&& Action == LastUniversalAction
		&& Now - LastUniversalActionTime < 2.0f;
	if (!bRepeatedSpam)
	{
		ResetInactivity();
	}

	if (Action == EVNHUniversalAction::Inspect)
	{
		if (Target && Target == LastInspectedActor.Get() && Now - LastInspectTime < 6.0f)
		{
			ApplyComposureDelta(-5.0f, TEXT("RepeatedInspect"));
		}

		LastInspectedActor = Target;
		LastInspectTime = Now;
	}

	LastUniversalAction = Action;
	LastUniversalActionTime = Now;
}

void AVNHShopperCharacter::SetHeldProp(AActor* NewHeldProp)
{
	if (HasAuthority())
	{
		if (NewHeldProp)
		{
			HeldProps.AddUnique(NewHeldProp);
		}
		else if (!HeldProps.IsEmpty())
		{
			HeldProps.Pop();
		}
	}
}

void AVNHShopperCharacter::PlayDirectionalKnockdown(const FVector& ImpactOrigin)
{
	if (!HasAuthority())
	{
		return;
	}

	const AVNHPlayerState* VNHPlayerState = GetPlayerState<AVNHPlayerState>();
	const EVNHPlayerRole PlayerRole = VNHPlayerState ? VNHPlayerState->GetRole() : EVNHPlayerRole::Unassigned;
	if (PlayerRole != EVNHPlayerRole::Human && PlayerRole != EVNHPlayerRole::Alien && PlayerRole != EVNHPlayerRole::Hunter)
	{
		return;
	}

	if (UAnimMontage* KnockdownMontage = ResolveFartKnockdownMontage(PlayerRole, GetFartKnockdownRowName(this, ImpactOrigin)))
	{
		PlayUniversalActionMontage(KnockdownMontage);
	}
}

bool AVNHShopperCharacter::WasActionRepeatedRecently(EVNHUniversalAction Action, float WithinSeconds) const
{
	const float Now = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.0f;
	return Action != EVNHUniversalAction::None
		&& LastUniversalAction == Action
		&& Now - LastUniversalActionTime <= WithinSeconds;
}

void AVNHShopperCharacter::UpdateComposureSystem(float DeltaSeconds)
{
	if (FartCooldownRemaining > 0.0f)
	{
		const float PreviousFartCooldownRemaining = FartCooldownRemaining;
		FartCooldownRemaining = FMath::Max(0.0f, FartCooldownRemaining - DeltaSeconds);
		if (FartCooldownRemaining <= ManualFartCooldownReadyTolerance)
		{
			FartCooldownRemaining = 0.0f;
		}
		if (PreviousFartCooldownRemaining > 0.0f && FartCooldownRemaining <= 0.0f)
		{
			ForceNetUpdate();
		}
	}

	if (IsComposureLockedForRoundPhase(GetWorld()))
	{
		ResetInactivity();
		return;
	}

	const FVector CurrentLocation = GetActorLocation();
	const FRotator CurrentControlRotation = GetControlRotation();
	const float MeaningfulMoveDistance = FVector::Dist2D(CurrentLocation, LastMeaningfulLocation);
	const float MeaningfulTurnDegrees = FMath::Abs(FMath::FindDeltaAngleDegrees(
		LastMeaningfulControlRotation.Yaw,
		CurrentControlRotation.Yaw))
		+ FMath::Abs(FMath::FindDeltaAngleDegrees(
			LastMeaningfulControlRotation.Pitch,
			CurrentControlRotation.Pitch));

	if (MeaningfulMoveDistance >= 45.0f || MeaningfulTurnDegrees >= 18.0f)
	{
		ResetInactivity();
	}
	else
	{
		InactivitySeconds += DeltaSeconds;
	}

	bool bHunterVeryClose = false;
	const bool bWatchedByHunter = IsWatchedByHunter(bHunterVeryClose);
	bWasWatchedByHunter = bWatchedByHunter;
	TimeSinceHunterWatch = bWatchedByHunter ? 0.0f : TimeSinceHunterWatch + DeltaSeconds;

	if (bWatchedByHunter)
	{
		ApplyComposureDelta(-4.0f * DeltaSeconds, TEXT("HunterStare"));
	}
	if (bHunterVeryClose)
	{
		ApplyComposureDelta(-2.0f * DeltaSeconds, TEXT("HunterProximity"));
	}
	if (IsNearSuspiciousObject())
	{
		ApplyComposureDelta(-3.0f * DeltaSeconds, TEXT("SuspiciousObjectProximity"));
	}

	const float Now = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.0f;
	if (!bWatchedByHunter && !bHunterVeryClose && GetVelocity().Size2D() > 15.0f && Now - LastSuspiciousEventTime > 3.0f)
	{
		ApplyComposureDelta(1.0f * DeltaSeconds, TEXT("ActiveRecovery"));
	}

	if (!bStandingStillPenaltyApplied && InactivitySeconds >= 8.0f)
	{
		bStandingStillPenaltyApplied = true;
		ApplyComposureDelta(-5.0f, TEXT("StandingStill"));
	}

	if (InactivitySeconds >= CurrentFartThreshold)
	{
		TriggerFart();
	}
}

void AVNHShopperCharacter::UpdateComposureState()
{
	EVNHComposureState NewState = EVNHComposureState::Panic;
	float NewFartThreshold = 6.5f;
	if (Composure >= 76.0f)
	{
		NewState = EVNHComposureState::Calm;
		NewFartThreshold = 13.0f;
	}
	else if (Composure >= 51.0f)
	{
		NewState = EVNHComposureState::Stable;
		NewFartThreshold = 11.0f;
	}
	else if (Composure >= 26.0f)
	{
		NewState = EVNHComposureState::Nervous;
		NewFartThreshold = 9.0f;
	}
	else if (Composure >= 1.0f)
	{
		NewState = EVNHComposureState::Cracking;
		NewFartThreshold = 7.5f;
	}

	const bool bStateChanged = ComposureState != NewState;
	ComposureState = NewState;
	CurrentFartThreshold = NewFartThreshold;
	if (bStateChanged)
	{
		ApplyComposureVisualState();
	}
}

void AVNHShopperCharacter::ResetInactivity()
{
	InactivitySeconds = 0.0f;
	bStandingStillPenaltyApplied = false;
	LastMeaningfulLocation = GetActorLocation();
	LastMeaningfulControlRotation = GetControlRotation();
}

bool AVNHShopperCharacter::TriggerFart()
{
	if ((GetWorld() && GetWorld()->GetMapName().Contains(TEXT("PP_"))) || !HasAuthority())
	{
		return false;
	}

	const FVector CloudCenter = GetActorLocation() - GetActorForwardVector() * FartCloudBackwardOffset + FVector(0.0f, 0.0f, FartCloudHeightOffset);
	MulticastTriggerFart();
	ApplyComposureDelta(-20.0f, TEXT("PublicFart"));
	ResetInactivity();

	for (TActorIterator<AVNHShopperCharacter> It(GetWorld()); It; ++It)
	{
		AVNHShopperCharacter* NearbyShopper = *It;
		if (NearbyShopper && NearbyShopper != this && NearbyShopper->IsPlayerControlled()
			&& FVector::DistSquared(NearbyShopper->GetActorLocation(), CloudCenter) <= FMath::Square(FartCloudKnockdownRadius))
		{
			NearbyShopper->ApplyComposureDelta(-3.0f, TEXT("NearbyFart"));
			const AVNHPlayerState* NearbyPlayerState = NearbyShopper->GetPlayerState<AVNHPlayerState>();
			const EVNHPlayerRole NearbyRole = NearbyPlayerState ? NearbyPlayerState->GetRole() : EVNHPlayerRole::Unassigned;
			if (NearbyRole == EVNHPlayerRole::Human || NearbyRole == EVNHPlayerRole::Alien)
			{
				if (UAnimMontage* KnockdownMontage = NearbyShopper->ResolveFartKnockdownMontage(NearbyRole, GetFartKnockdownRowName(NearbyShopper, CloudCenter)))
				{
					NearbyShopper->PlayUniversalActionMontage(KnockdownMontage);
				}
			}
		}
	}

	return true;
}

bool AVNHShopperCharacter::TriggerFartFromAction()
{
	if (!HasAuthority())
	{
		return false;
	}

	if (FartCooldownRemaining <= ManualFartCooldownReadyTolerance)
	{
		FartCooldownRemaining = 0.0f;
	}
	else
	{
		return false;
	}

	const float Now = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.0f;
	const float LastManualFartReadyTime = LastManualFartActionTime + LastManualFartCooldownSeconds;
	if (Now - LastManualFartReadyTime > ManualFartSpamWindowSeconds)
	{
		ManualFartSpamStreak = 0;
	}

	if (!TriggerFart())
	{
		return false;
	}

	FartCooldownRemaining = GetManualFartCooldownForStreak(ManualFartSpamStreak);
	ManualFartSpamStreak = FMath::Min(ManualFartSpamStreak + 1, static_cast<int32>(UE_ARRAY_COUNT(ManualFartCooldownSeconds) - 1));
	LastManualFartCooldownSeconds = FartCooldownRemaining;
	LastManualFartActionTime = Now;
	ForceNetUpdate();
	return true;
}

void AVNHShopperCharacter::PlayUniversalActionMontage(UAnimMontage* Montage)
{
	if (HasAuthority() && Montage)
	{
		MulticastPlayUniversalActionMontage(Montage);
	}
}

bool AVNHShopperCharacter::IsWatchedByHunter(bool& bOutHunterVeryClose) const
{
	bOutHunterVeryClose = false;
	if (!GetWorld())
	{
		return false;
	}

	bool bWatched = false;
	for (FConstPlayerControllerIterator It = GetWorld()->GetPlayerControllerIterator(); It; ++It)
	{
		const APlayerController* PlayerController = It->Get();
		const AVNHPlayerState* HunterPlayerState = PlayerController ? PlayerController->GetPlayerState<AVNHPlayerState>() : nullptr;
		if (!PlayerController || !HunterPlayerState || !HunterPlayerState->IsHunter() || PlayerController->GetPawn() == this)
		{
			continue;
		}

		FVector ViewLocation;
		FRotator ViewRotation;
		PlayerController->GetPlayerViewPoint(ViewLocation, ViewRotation);
		const FVector TargetLocation = GetActorLocation() + FVector(0.0f, 0.0f, 55.0f);
		const FVector ToTarget = TargetLocation - ViewLocation;
		const float Distance = ToTarget.Size();
		bOutHunterVeryClose |= Distance <= 250.0f;
		if (Distance <= KINDA_SMALL_NUMBER || Distance > 2000.0f || FVector::DotProduct(ViewRotation.Vector(), ToTarget / Distance) < 0.965f)
		{
			continue;
		}

		FCollisionQueryParams QueryParams(SCENE_QUERY_STAT(VNHHunterComposureTrace), false);
		QueryParams.AddIgnoredActor(PlayerController->GetPawn());
		FHitResult Hit;
		const bool bHit = GetWorld()->LineTraceSingleByChannel(Hit, ViewLocation, TargetLocation, ECC_Visibility, QueryParams);
		if (!bHit || Hit.GetActor() == this)
		{
			bWatched = true;
		}
	}

	return bWatched;
}

bool AVNHShopperCharacter::IsNearSuspiciousObject() const
{
	if (!GetWorld())
	{
		return false;
	}

	for (TActorIterator<AActor> It(GetWorld()); It; ++It)
	{
		const AActor* Actor = *It;
		if (Actor && !IsHoldingProp(Actor)
			&& (Actor->ActorHasTag(TEXT("VNH.Suspicious")) || Actor->GetClass()->GetName().Contains(TEXT("Suspicious")))
			&& FVector::DistSquared(Actor->GetActorLocation(), GetActorLocation()) <= FMath::Square(180.0f))
		{
			return true;
		}
	}

	return false;
}

void AVNHShopperCharacter::ApplyComposureVisualState()
{
	if (bFrozenByPublicTest)
	{
		return;
	}

	if (USkeletalMeshComponent* MeshComponent = GetMesh())
	{
		switch (ComposureState)
		{
		case EVNHComposureState::Calm:
			MeshComponent->GlobalAnimRateScale = 1.0f;
			break;
		case EVNHComposureState::Stable:
			MeshComponent->GlobalAnimRateScale = 1.03f;
			break;
		case EVNHComposureState::Nervous:
			MeshComponent->GlobalAnimRateScale = 1.08f;
			break;
		case EVNHComposureState::Cracking:
			MeshComponent->GlobalAnimRateScale = 1.14f;
			break;
		case EVNHComposureState::Panic:
			MeshComponent->GlobalAnimRateScale = 1.22f;
			break;
		}
	}
}

void AVNHShopperCharacter::MulticastTriggerFart_Implementation()
{
	const float SourceComposure = Composure;
	const float PressureAlpha = 1.0f - FMath::Clamp(SourceComposure / 100.0f, 0.0f, 1.0f);
	const float VolumeMultiplier = FMath::Lerp(CalmFartVolume, PanicFartVolume, PressureAlpha);
	const float InnerRadius = FMath::Lerp(CalmFartInnerRadius, PanicFartInnerRadius, PressureAlpha);
	const float FalloffDistance = FMath::Lerp(CalmFartFalloffDistance, PanicFartFalloffDistance, PressureAlpha);
	const FVector CloudCenter = GetActorLocation() - GetActorForwardVector() * FartCloudBackwardOffset + FVector(0.0f, 0.0f, FartCloudHeightOffset);

	USoundAttenuation* DynamicAttenuation = NewObject<USoundAttenuation>(this);
	DynamicAttenuation->Attenuation.bAttenuate = true;
	DynamicAttenuation->Attenuation.bSpatialize = true;
	DynamicAttenuation->Attenuation.DistanceAlgorithm = EAttenuationDistanceModel::Logarithmic;
	DynamicAttenuation->Attenuation.AttenuationShape = EAttenuationShape::Sphere;
	DynamicAttenuation->Attenuation.AttenuationShapeExtents = FVector(InnerRadius, 0.0f, 0.0f);
	DynamicAttenuation->Attenuation.FalloffDistance = FalloffDistance;

	if (FartSound)
	{
		UGameplayStatics::PlaySoundAtLocation(
			this,
			FartSound,
			CloudCenter,
			FRotator::ZeroRotator,
			VolumeMultiplier,
			1.0f,
			0.0f,
			DynamicAttenuation,
			nullptr,
			this);
	}
	else
	{
		constexpr int32 SampleRate = 22050;
		constexpr float DurationSeconds = 0.85f;
		const int32 SampleCount = FMath::RoundToInt(SampleRate * DurationSeconds);
		TArray<int16> Samples;
		Samples.SetNumUninitialized(SampleCount);
		FRandomStream RandomStream(GetUniqueID() + FMath::RoundToInt(GetWorld() ? GetWorld()->GetTimeSeconds() * 100.0f : 0.0f));
		for (int32 Index = 0; Index < SampleCount; ++Index)
		{
			const float Time = static_cast<float>(Index) / SampleRate;
			const float Alpha = static_cast<float>(Index) / SampleCount;
			const float Envelope = FMath::Sin(PI * FMath::Clamp(Alpha, 0.0f, 1.0f)) * FMath::Square(1.0f - Alpha);
			const float Frequency = FMath::Lerp(88.0f, 42.0f, Alpha);
			const float Body = FMath::Sin(2.0f * PI * Frequency * Time + 0.7f * FMath::Sin(2.0f * PI * 7.0f * Time));
			const float Noise = RandomStream.FRandRange(-1.0f, 1.0f) * 0.22f * (1.0f - Alpha);
			Samples[Index] = static_cast<int16>(FMath::Clamp((Body * 0.78f + Noise) * Envelope, -1.0f, 1.0f) * 24000.0f);
		}

		USoundWaveProcedural* ProceduralFart = NewObject<USoundWaveProcedural>(this);
		ProceduralFart->SetSampleRate(SampleRate);
		ProceduralFart->NumChannels = 1;
		ProceduralFart->Duration = DurationSeconds;
		ProceduralFart->SoundGroup = SOUNDGROUP_Effects;
		ProceduralFart->QueueAudio(reinterpret_cast<const uint8*>(Samples.GetData()), Samples.Num() * sizeof(int16));
		UGameplayStatics::SpawnSoundAtLocation(
			this,
			ProceduralFart,
			CloudCenter,
			FRotator::ZeroRotator,
			VolumeMultiplier,
			1.0f,
			0.0f,
			DynamicAttenuation);
	}

	const FVector CloudForward = GetActorForwardVector();
	DrawDebugSphere(GetWorld(), CloudCenter - CloudForward * 18.0f, 18.0f, 10, FColor(125, 180, 70), false, 2.0f, 0, 2.0f);
	DrawDebugSphere(GetWorld(), CloudCenter + CloudForward * 8.0f + FVector(0.0f, 0.0f, 10.0f), 24.0f, 10, FColor(155, 195, 85), false, 2.0f, 0, 2.0f);
	DrawDebugSphere(GetWorld(), CloudCenter + CloudForward * 28.0f + FVector(0.0f, 0.0f, 22.0f), 14.0f, 10, FColor(180, 205, 95), false, 2.0f, 0, 2.0f);
	UE_LOG(LogVNH, Display, TEXT("FartEvent: %s broke composure in public. SourceComposure=%.1f Volume=%.2f InnerRadius=%.0f MaxDistance=%.0f"),
		*GetNameSafe(this),
		SourceComposure,
		VolumeMultiplier,
		InnerRadius,
		InnerRadius + FalloffDistance);
}

UAnimMontage* AVNHShopperCharacter::ResolveFartKnockdownMontage(EVNHPlayerRole PlayerRole, FName RowName) const
{
	if (RowName.IsNone())
	{
		return nullptr;
	}

	UDataTable* ActionAnimationTable = nullptr;
	if (PlayerRole == EVNHPlayerRole::Human || PlayerRole == EVNHPlayerRole::Hunter)
	{
		ActionAnimationTable = HumanActionAnimationTable.LoadSynchronous();
	}
	else if (PlayerRole == EVNHPlayerRole::Alien)
	{
		ActionAnimationTable = AlienActionAnimationTable.LoadSynchronous();
	}

	if (!ActionAnimationTable)
	{
		return nullptr;
	}

	const TMap<FName, uint8*>& RowMap = ActionAnimationTable->GetRowMap();
	uint8* const* RowDataPtr = RowMap.Find(RowName);
	const uint8* RowData = RowDataPtr ? *RowDataPtr : nullptr;
	if (!RowData)
	{
		return nullptr;
	}

	const UScriptStruct* RowStruct = ActionAnimationTable->GetRowStruct();
	const TCHAR* AnimationFieldName = TEXT("HighComposureAnimations");
	if (PlayerRole != EVNHPlayerRole::Hunter && Composure <= 0.0f)
	{
		AnimationFieldName = TEXT("NoComposureAnimations");
	}
	else if (PlayerRole != EVNHPlayerRole::Hunter && Composure <= 50.0f)
	{
		AnimationFieldName = TEXT("LowComposureAnimations");
	}

	return ChooseFartKnockdownMontageFromField(RowData, FindFartKnockdownArrayFieldByPrefix(RowStruct, AnimationFieldName));
}

FName AVNHShopperCharacter::GetFartKnockdownRowName(const AVNHShopperCharacter* HitShopper, const FVector& CloudCenter) const
{
	if (!HitShopper)
	{
		return NAME_None;
	}

	const FVector PushDirection = (HitShopper->GetActorLocation() - CloudCenter).GetSafeNormal2D();
	if (PushDirection.IsNearlyZero())
	{
		return TEXT("BackKnockdown");
	}

	const FVector HitForward = HitShopper->GetActorForwardVector().GetSafeNormal2D();
	const FVector HitRight = HitShopper->GetActorRightVector().GetSafeNormal2D();
	const float ForwardDot = FVector::DotProduct(HitForward, PushDirection);
	const float RightDot = FVector::DotProduct(HitRight, PushDirection);

	if (FMath::Abs(ForwardDot) >= FMath::Abs(RightDot))
	{
		return ForwardDot >= 0.0f ? TEXT("FrontKnockdown") : TEXT("BackKnockdown");
	}

	return RightDot >= 0.0f ? TEXT("RightKnockdown") : TEXT("LeftKnockdown");
}

void AVNHShopperCharacter::MulticastPlayUniversalActionMontage_Implementation(UAnimMontage* Montage)
{
	if (!Montage)
	{
		return;
	}

	float LockDurationSeconds = Montage->GetPlayLength();
	USkeletalMeshComponent* MeshComponent = GetMesh();
	UAnimInstance* AnimInstance = MeshComponent ? MeshComponent->GetAnimInstance() : nullptr;
	if (AnimInstance)
	{
		const float PlayedDurationSeconds = AnimInstance->Montage_Play(Montage);
		if (PlayedDurationSeconds > 0.0f)
		{
			LockDurationSeconds = PlayedDurationSeconds;
		}
	}

	StartUniversalActionMovementLock(LockDurationSeconds);
}

void AVNHShopperCharacter::OnRep_Composure()
{
	ApplyComposureVisualState();
}

void AVNHShopperCharacter::OnRep_CharacterCustomization()
{
	ApplyCharacterCustomization();
}

void AVNHShopperCharacter::SetCharacterCustomization(const FVNHCharacterCustomization& NewCustomization)
{
	if (HasAuthority())
	{
		if (ShouldUsePairPressureMascotVisuals())
		{
			ApplyCharacterCustomization();
			return;
		}
		CharacterCustomization = NewCustomization;
		ApplyCharacterCustomization();
	}
}

void AVNHShopperCharacter::UpdateRagdollCameraAnchor(float DeltaSeconds)
{
	if (!IsLocallyControlled() || !FollowCameraBoom || !RootComponent)
	{
		return;
	}

	const USkeletalMeshComponent* CharacterMesh = GetMesh();
	const bool bRagdollStateActive = PairPressurePhysicalStateComponent
		&& PairPressurePhysicalStateComponent->IsRagdolled();
	const bool bRecoveryBlendActive = PairPressurePhysicalStateComponent
		&& PairPressurePhysicalStateComponent->IsRagdollRecoveryBlendActive();
	const bool bFollowPhysicsBody = CharacterMesh
		&& CharacterMesh->IsAnySimulatingPhysics()
		&& (!PairPressurePhysicalStateComponent || bRagdollStateActive);
	FVector DesiredWorldLocation = RootComponent->GetComponentLocation();
	if (bFollowPhysicsBody)
	{
		FVector PhysicsAnchor = CharacterMesh->GetComponentLocation();
		const FName CandidateBones[] = {
			FName(TEXT("hips")), FName(TEXT("pelvis")), FName(TEXT("chest"))
		};
		for (const FName Candidate : CandidateBones)
		{
			if (CharacterMesh->GetBoneIndex(Candidate) != INDEX_NONE)
			{
				PhysicsAnchor = CharacterMesh->GetBoneLocation(Candidate);
				break;
			}
		}
		if (!PhysicsAnchor.ContainsNaN())
		{
			DesiredWorldLocation = PhysicsAnchor;
		}
	}
	else if (bRecoveryBlendActive)
	{
		// Once recovery starts, stop following rotating physics bones. The capsule
		// is already placed at its grounded standing target, so the old hips focus
		// is exactly one capsule half-height below it. Ease only that vertical
		// difference using the same blend alpha as the visible mesh recovery.
		const UCapsuleComponent* Capsule = GetCapsuleComponent();
		const float CapsuleHalfHeight = Capsule ? Capsule->GetScaledCapsuleHalfHeight() : 0.0f;
		const FVector RecoveryStartWorldLocation = RootComponent->GetComponentLocation()
			- FVector::UpVector * CapsuleHalfHeight;
		DesiredWorldLocation = FMath::Lerp(
			RecoveryStartWorldLocation,
			RootComponent->GetComponentLocation(),
			PairPressurePhysicalStateComponent->GetRagdollRecoveryBlendAlpha());
	}

	const float FollowSpeed = bFollowPhysicsBody ? 15.0f : (bRecoveryBlendActive ? 10.0f : 9.0f);
	const FVector NewWorldLocation = FMath::VInterpTo(
		FollowCameraBoom->GetComponentLocation(),
		DesiredWorldLocation,
		DeltaSeconds,
		FollowSpeed);
	FollowCameraBoom->SetWorldLocation(NewWorldLocation, false, nullptr, ETeleportType::TeleportPhysics);
}

bool AVNHShopperCharacter::CanJumpInternal_Implementation() const
{
	const UCharacterMovementComponent* MovementComponent = GetCharacterMovement();
	return (!MovementComponent || !MovementComponent->IsFalling())
		&& Super::CanJumpInternal_Implementation()
		&& (!PairPressurePhysicalStateComponent || !PairPressurePhysicalStateComponent->IsRagdolled())
		&& (!PairPressureGrabber || PairPressureGrabber->CanJumpOrDive())
		&& (!PairPressureActionRouter || !PairPressureActionRouter->IsAirDiveActive());
}

void AVNHShopperCharacter::HandleJumpInputPressed()
{
	if (PairPressurePhysicalStateComponent && PairPressurePhysicalStateComponent->IsRagdolled())
	{
		bJumpInputHeld = false;
		bLandingJumpRequested = false;
		return;
	}
	bJumpInputHeld = true;
	UCharacterMovementComponent* MovementComponent = GetCharacterMovement();
	if (!MovementComponent)
	{
		Jump();
		return;
	}

	if (MovementComponent->IsMovingOnGround())
	{
		bLandingJumpRequested = false;
		if (CanJump())
		{
			Jump();
		}
	}
	else if (MovementComponent->IsFalling())
	{
		// Do not call ACharacter::Jump while airborne. Jump() mutates
		// bPressedJump even when CanJump rejects the impulse, which creates a
		// one-frame saved-move boundary and feels like an airborne hitch.
		bLandingJumpRequested = true;
	}
}

void AVNHShopperCharacter::OnRep_PlayerState()
{
	Super::OnRep_PlayerState();

	if (const AVNHPlayerState* MascotPlayerState = GetPlayerState<AVNHPlayerState>();
		MascotPlayerState && MascotPlayerState->GetClass()->ImplementsInterface(UPPMascotSelectionInterface::StaticClass()))
	{
		ApplySelectedMascotRowName_Implementation(
			IPPMascotSelectionInterface::Execute_GetSelectedMascotRowName(MascotPlayerState));
	}
}

void AVNHShopperCharacter::HandleJumpInputReleased()
{
	bJumpInputHeld = false;
	bLandingJumpRequested = false;
}

void AVNHShopperCharacter::OnJumped_Implementation()
{
	Super::OnJumped_Implementation();
	if (PairPressureActionRouter)
	{
		PairPressureActionRouter->NotifyJumpStarted();
	}
}

void AVNHShopperCharacter::Landed(const FHitResult& Hit)
{
	Super::Landed(Hit);
	if (PairPressureActionRouter)
	{
		PairPressureActionRouter->NotifyLanded();
	}
}

void AVNHShopperCharacter::OnMovementModeChanged(EMovementMode PreviousMovementMode, uint8 PreviousCustomMode)
{
	Super::OnMovementModeChanged(PreviousMovementMode, PreviousCustomMode);

	if (PreviousMovementMode != MOVE_Falling)
	{
		return;
	}

	UCharacterMovementComponent* MovementComponent = GetCharacterMovement();
	const bool bShouldJumpOnLanding = MovementComponent
		&& MovementComponent->IsMovingOnGround()
		&& bLandingJumpRequested
		&& bJumpInputHeld;
	// Never carry a buffered press through Falling -> Flying/None/Custom. A later
	// unrelated landing must not consume input from a ledge, dive, or ragdoll handoff.
	bLandingJumpRequested = false;
	if (bShouldJumpOnLanding && CanJump())
	{
		Jump();
	}
}

void AVNHShopperCharacter::UpdatePairPressureAnimationPresentation()
{
	if (!ShouldUsePairPressureMascotVisuals() || !PairPressureGrabber || !GetMesh())
	{
		return;
	}

	UAnimInstance* AnimInstance = GetMesh()->GetAnimInstance();
	if (!AnimInstance)
	{
		return;
	}

	const EPPGrabState CurrentGrabState = PairPressureGrabber->GetGrabState_Implementation();
	// This compact rig's arm chains share the torso, so procedural carry IK rolls
	// the mascot while it runs. A hanging penguin is stationary, however, and needs
	// the final small correction so both hands land on the authored collision edge.
	const bool bEnableGrabIK = CurrentGrabState == EPPGrabState::HangingFromLedge;
	FVector WorldGrabPoint = PairPressureGrabber->GetPresentationGrabPoint();
	if (WorldGrabPoint.IsNearlyZero())
	{
		WorldGrabPoint = GetActorLocation() + GetActorForwardVector() * 95.0f + FVector(0.0f, 0.0f, 54.0f);
	}
	const FVector HandSeparation = GetActorRightVector() * 17.0f;
	const FTransform MeshTransform = GetMesh()->GetComponentTransform();
	const FVector LeftEffector = MeshTransform.InverseTransformPositionNoScale(WorldGrabPoint - HandSeparation);
	const FVector RightEffector = MeshTransform.InverseTransformPositionNoScale(WorldGrabPoint + HandSeparation);

	if (FFloatProperty* AlphaProperty = FindFProperty<FFloatProperty>(AnimInstance->GetClass(), TEXT("GrabIKAlpha")))
	{
		AlphaProperty->SetPropertyValue_InContainer(AnimInstance, bEnableGrabIK ? 1.0f : 0.0f);
	}
	if (FStructProperty* LeftProperty = FindFProperty<FStructProperty>(AnimInstance->GetClass(), TEXT("LeftGrabIKEffector")))
	{
		if (LeftProperty->Struct == TBaseStructure<FVector>::Get())
		{
			*LeftProperty->ContainerPtrToValuePtr<FVector>(AnimInstance) = LeftEffector;
		}
	}
	if (FStructProperty* RightProperty = FindFProperty<FStructProperty>(AnimInstance->GetClass(), TEXT("RightGrabIKEffector")))
	{
		if (RightProperty->Struct == TBaseStructure<FVector>::Get())
		{
			*RightProperty->ContainerPtrToValuePtr<FVector>(AnimInstance) = RightEffector;
		}
	}
}

void AVNHShopperCharacter::PlayPairPressureMascotAnimation(UAnimSequence* Animation, bool bLooping)
{
	USkeletalMeshComponent* CharacterMesh = GetMesh();
	UAnimInstance* AnimInstance = CharacterMesh ? CharacterMesh->GetAnimInstance() : nullptr;
	if (!Animation || !AnimInstance || CharacterMesh->IsAnySimulatingPhysics()
		|| bPairPressureObstacleFallPresentationActive)
	{
		return;
	}
	GetWorldTimerManager().ClearTimer(PairPressurePresentationTimerHandle);
	AnimInstance->StopSlotAnimation(0.08f, TEXT("DefaultSlot"));
	AnimInstance->PlaySlotAnimationAsDynamicMontage(
		Animation,
		TEXT("DefaultSlot"),
		0.08f,
		0.12f,
		1.0f,
		bLooping ? 9999 : 1,
		-1.0f,
		0.0f);
}

void AVNHShopperCharacter::RestorePairPressureLocomotionAnimation()
{
	if (bPairPressureObstacleFallPresentationActive)
	{
		return;
	}
	if (USkeletalMeshComponent* CharacterMesh = GetMesh())
	{
		if (UAnimInstance* AnimInstance = CharacterMesh->GetAnimInstance())
		{
			AnimInstance->StopSlotAnimation(0.12f, TEXT("DefaultSlot"));
		}
	}
}

UAnimSequence* AVNHShopperCharacter::ResolvePairPressureObstacleFallAnimation(
	EPPObstacleFallDirection FallDirection)
{
	const int32 DirectionIndex = static_cast<int32>(FallDirection);
	if (PairPressureObstacleFallAnimations.IsValidIndex(DirectionIndex)
		&& PairPressureObstacleFallAnimations[DirectionIndex])
	{
		return PairPressureObstacleFallAnimations[DirectionIndex];
	}

	if (PairPressureObstacleFallAnimations.Num() != PPShopperObstacleFallDirectionCount)
	{
		PairPressureObstacleFallAnimations.SetNum(PPShopperObstacleFallDirectionCount);
	}
	const FPPMascotAnimationRow* ActiveMascotRow = ResolveActiveMascotRow(TEXT("Obstacle fall presentation"));
	const TSoftObjectPtr<UAnimSequence>* RequestedAnimation = nullptr;
	if (ActiveMascotRow)
	{
		switch (FallDirection)
		{
		case EPPObstacleFallDirection::Forward:
			RequestedAnimation = &ActiveMascotRow->ObstacleFallFront;
			break;
		case EPPObstacleFallDirection::Backward:
			RequestedAnimation = &ActiveMascotRow->ObstacleFallBack;
			break;
		case EPPObstacleFallDirection::Left:
			RequestedAnimation = &ActiveMascotRow->ObstacleFallLeft;
			break;
		case EPPObstacleFallDirection::Right:
			RequestedAnimation = &ActiveMascotRow->ObstacleFallRight;
			break;
		default:
			break;
		}
	}

	UAnimSequence* ResolvedAnimation = RequestedAnimation && !RequestedAnimation->IsNull()
		? RequestedAnimation->LoadSynchronous()
		: nullptr;
	if (!ResolvedAnimation)
	{
		// falls2/falls3 are mirrored lateral clips (and pair with wakesup2/3).
		// falls1 is the authored backward fall; the V2 set supplies the dedicated
		// forward clip used when the obstacle pushes along the facing direction.
		const TCHAR* FallbackPath = TEXT(
			"/Game/CuteChubbyPenguin/Penguin/Animations/AS_Penguin_UE_Anim_falls1.AS_Penguin_UE_Anim_falls1");
		if (FallDirection == EPPObstacleFallDirection::Forward)
		{
			FallbackPath = TEXT(
				"/Game/CuteChubbyPenguin/Penguin/Animations/AS_Penguin_UE_Anim_falls_forward.AS_Penguin_UE_Anim_falls_forward");
		}
		else if (FallDirection == EPPObstacleFallDirection::Left)
		{
			FallbackPath = TEXT(
				"/Game/CuteChubbyPenguin/Penguin/Animations/AS_Penguin_UE_Anim_falls3.AS_Penguin_UE_Anim_falls3");
		}
		else if (FallDirection == EPPObstacleFallDirection::Right)
		{
			FallbackPath = TEXT(
				"/Game/CuteChubbyPenguin/Penguin/Animations/AS_Penguin_UE_Anim_falls2.AS_Penguin_UE_Anim_falls2");
		}
		ResolvedAnimation = LoadObject<UAnimSequence>(nullptr, FallbackPath);
	}
	if (PairPressureObstacleFallAnimations.IsValidIndex(DirectionIndex))
	{
		PairPressureObstacleFallAnimations[DirectionIndex] = ResolvedAnimation;
	}
	return ResolvedAnimation;
}

void AVNHShopperCharacter::PreloadPairPressureObstacleFallAnimations()
{
	PairPressureObstacleFallAnimations.SetNum(PPShopperObstacleFallDirectionCount);
	for (int32 DirectionIndex = 0;
		DirectionIndex < PPShopperObstacleFallDirectionCount;
		++DirectionIndex)
	{
		ResolvePairPressureObstacleFallAnimation(
			static_cast<EPPObstacleFallDirection>(DirectionIndex));
	}
}

void AVNHShopperCharacter::BeginPairPressureObstacleFallPresentation(
	EPPObstacleFallDirection FallDirection,
	float PresentationElapsedSeconds)
{
	USkeletalMeshComponent* CharacterMesh = GetMesh();
	UAnimInstance* AnimInstance = CharacterMesh ? CharacterMesh->GetAnimInstance() : nullptr;
	UAnimSequence* FallAnimation = ResolvePairPressureObstacleFallAnimation(FallDirection);
	if (!CharacterMesh || !AnimInstance || !FallAnimation
		|| CharacterMesh->IsAnySimulatingPhysics())
	{
		return;
	}

	GetWorldTimerManager().ClearTimer(PairPressurePresentationTimerHandle);
	GetWorldTimerManager().ClearTimer(PairPressureDiveRecoveryPresentationTimerHandle);
	GetWorldTimerManager().ClearTimer(PairPressureObstacleFallHoldTimerHandle);
	bPairPressureActionPresentationActive = true;
	bPairPressureObstacleFallPresentationActive = true;
	bPairPressureObstacleFallRecoveryActive = false;

	const float HoldPosition = FMath::Max(
		0.0f,
		FallAnimation->GetPlayLength() - PPShopperObstacleFallPoseLeadSeconds);
	const float StartPosition = FMath::Clamp(
		FMath::Max(0.0f, PresentationElapsedSeconds) * PPShopperObstacleFallPlayRate,
		0.0f,
		HoldPosition);
	AnimInstance->StopSlotAnimation(0.03f, TEXT("DefaultSlot"));
	PairPressureObstacleFallMontage = AnimInstance->PlaySlotAnimationAsDynamicMontage(
		FallAnimation,
		TEXT("DefaultSlot"),
		0.04f,
		0.0f,
		PPShopperObstacleFallPlayRate,
		1,
		0.0f,
		StartPosition);
	if (!PairPressureObstacleFallMontage)
	{
		bPairPressureObstacleFallPresentationActive = false;
		bPairPressureObstacleFallRecoveryActive = false;
		bPairPressureActionPresentationActive = false;
		RestorePairPressureLocomotionAnimation();
		return;
	}

	const float SecondsUntilHold = (HoldPosition - StartPosition)
		/ PPShopperObstacleFallPlayRate;
	if (SecondsUntilHold <= KINDA_SMALL_NUMBER)
	{
		HoldPairPressureObstacleFallPose();
	}
	else
	{
		GetWorldTimerManager().SetTimer(
			PairPressureObstacleFallHoldTimerHandle,
			this,
			&AVNHShopperCharacter::HoldPairPressureObstacleFallPose,
			SecondsUntilHold,
			false);
	}
}

void AVNHShopperCharacter::HoldPairPressureObstacleFallPose()
{
	if (!bPairPressureObstacleFallPresentationActive
		|| bPairPressureObstacleFallRecoveryActive
		|| !PairPressureObstacleFallMontage)
	{
		return;
	}
	if (USkeletalMeshComponent* CharacterMesh = GetMesh())
	{
		if (UAnimInstance* AnimInstance = CharacterMesh->GetAnimInstance())
		{
			const float HoldPosition = FMath::Max(
				0.0f,
				PairPressureObstacleFallMontage->GetPlayLength()
					- PPShopperObstacleFallPoseLeadSeconds);
			// Clamp to an explicit grounded frame, then freeze by play rate instead of
			// pausing. The montage remains an evaluated slot contributor, so its weight
			// can subsequently fade over the synchronized recovery window.
			AnimInstance->Montage_SetPosition(PairPressureObstacleFallMontage, HoldPosition);
			AnimInstance->Montage_SetPlayRate(PairPressureObstacleFallMontage, 0.0f);
		}
	}
}

void AVNHShopperCharacter::BeginPairPressureObstacleFallRecoveryPresentation(
	float RecoveryElapsedSeconds,
	float RecoveryBlendSeconds)
{
	if (!bPairPressureObstacleFallPresentationActive
		|| bPairPressureObstacleFallRecoveryActive)
	{
		return;
	}
	bPairPressureObstacleFallRecoveryActive = true;
	GetWorldTimerManager().ClearTimer(PairPressureObstacleFallHoldTimerHandle);

	USkeletalMeshComponent* CharacterMesh = GetMesh();
	UAnimInstance* AnimInstance = CharacterMesh ? CharacterMesh->GetAnimInstance() : nullptr;
	if (!AnimInstance || !PairPressureObstacleFallMontage)
	{
		return;
	}
	const float RemainingBlendSeconds = FMath::Max(
		0.03f,
		FMath::Max(0.0f, RecoveryBlendSeconds)
			- FMath::Max(0.0f, RecoveryElapsedSeconds));
	const float HoldPosition = FMath::Max(
		0.0f,
		PairPressureObstacleFallMontage->GetPlayLength()
			- PPShopperObstacleFallPoseLeadSeconds);
	// A recovery packet can arrive before the local hold timer (especially on a
	// joining client). Force the same stable pose on every peer before beginning
	// the fade, and keep playback frozen so the montage cannot hit its zero-time
	// natural end while its slot weight blends back to locomotion.
	AnimInstance->Montage_SetPosition(PairPressureObstacleFallMontage, HoldPosition);
	AnimInstance->Montage_SetPlayRate(PairPressureObstacleFallMontage, 0.0f);
	AnimInstance->Montage_Stop(RemainingBlendSeconds, PairPressureObstacleFallMontage);
}

void AVNHShopperCharacter::EndPairPressureObstacleFallPresentation(float RecoveryBlendSeconds)
{
	GetWorldTimerManager().ClearTimer(PairPressureObstacleFallHoldTimerHandle);
	const bool bHadSynchronizedRecovery = bPairPressureObstacleFallRecoveryActive;
	bPairPressureObstacleFallPresentationActive = false;
	bPairPressureObstacleFallRecoveryActive = false;
	bPairPressureActionPresentationActive = false;
	bool bStoppedObstacleFallMontage = false;
	if (USkeletalMeshComponent* CharacterMesh = GetMesh())
	{
		if (UAnimInstance* AnimInstance = CharacterMesh->GetAnimInstance();
			AnimInstance && PairPressureObstacleFallMontage)
		{
			if (!bHadSynchronizedRecovery)
			{
				// Replicated properties may coalesce and deliver Grounded before the
				// intermediate recovery timestamp. Reconstruct the same held pose and
				// full-duration fade instead of falling back to a short client-only pop.
				const float HoldPosition = FMath::Max(
					0.0f,
					PairPressureObstacleFallMontage->GetPlayLength()
						- PPShopperObstacleFallPoseLeadSeconds);
				AnimInstance->Montage_SetPosition(PairPressureObstacleFallMontage, HoldPosition);
				AnimInstance->Montage_SetPlayRate(PairPressureObstacleFallMontage, 0.0f);
				AnimInstance->Montage_Stop(
					FMath::Max(0.03f, RecoveryBlendSeconds),
					PairPressureObstacleFallMontage);
			}
			// When synchronized recovery already started, leave its existing blend
			// untouched. A second short Stop call can truncate the remaining fade.
			bStoppedObstacleFallMontage = true;
		}
	}
	PairPressureObstacleFallMontage = nullptr;
	if (!bStoppedObstacleFallMontage)
	{
		RestorePairPressureLocomotionAnimation();
	}
}

UAnimSequence* AVNHShopperCharacter::ResolvePairPressureMascotAnimation(EPPGrabState GrabState) const
{
	if (GrabState == EPPGrabState::Releasing && PairPressureGrabber
		&& PairPressureGrabber->GetActiveGrabProfile().TargetType == EPPGrabTargetType::LedgeOrHandle)
	{
		return LoadObject<UAnimSequence>(
			nullptr,
			TEXT("/Game/CuteChubbyPenguin/Penguin/Animations/AS_Penguin_UE_climb_all.AS_Penguin_UE_climb_all"));
	}
	const FPPMascotAnimationRow* ActiveMascotRow = ResolveActiveMascotRow(TEXT("Pair Pressure presentation"));
	if (!ActiveMascotRow)
	{
		return nullptr;
	}
	if (GrabState == EPPGrabState::HangingFromLedge)
	{
		return ActiveMascotRow->Hanging.LoadSynchronous();
	}
	const TSoftObjectPtr<UAnimSequence>* RequestedAnimation = &ActiveMascotRow->Grab;
	switch (GrabState)
	{
	case EPPGrabState::Reaching:
		RequestedAnimation = ActiveMascotRow->Reach.IsNull() ? &ActiveMascotRow->Grab : &ActiveMascotRow->Reach;
		break;
	case EPPGrabState::HoldingItem:
		RequestedAnimation = ActiveMascotRow->HoldItem.IsNull() ? &ActiveMascotRow->OverheadThrow : &ActiveMascotRow->HoldItem;
		break;
	case EPPGrabState::GrabbingPlayer:
		RequestedAnimation = ActiveMascotRow->PlayerGrab.IsNull() ? &ActiveMascotRow->Punch : &ActiveMascotRow->PlayerGrab;
		break;
	case EPPGrabState::MutualGrab:
		RequestedAnimation = ActiveMascotRow->MutualGrab.IsNull() ? &ActiveMascotRow->Crouch : &ActiveMascotRow->MutualGrab;
		break;
	case EPPGrabState::PushingObject:
		RequestedAnimation = ActiveMascotRow->Push.IsNull() ? &ActiveMascotRow->Crouch : &ActiveMascotRow->Push;
		break;
	case EPPGrabState::Releasing:
		RequestedAnimation = ActiveMascotRow->GrabRelease.IsNull() ? &ActiveMascotRow->Throw : &ActiveMascotRow->GrabRelease;
		break;
	default:
		break;
	}
	return RequestedAnimation->LoadSynchronous();
}

void AVNHShopperCharacter::HandlePairPressureGrabStateChanged(EPPGrabState NewGrabState, AActor* /*NewTarget*/)
{
	if (NewGrabState == EPPGrabState::None || NewGrabState == EPPGrabState::GrabCooldown)
	{
		if (!bPairPressureActionPresentationActive && (!PairPressureActionRouter || !PairPressureActionRouter->IsAirDiveActive()))
		{
			RestorePairPressureLocomotionAnimation();
		}
		return;
	}
	if (NewGrabState == EPPGrabState::Releasing)
	{
		if (PairPressureGrabber
			&& PairPressureGrabber->GetActiveGrabProfile().TargetType == EPPGrabTargetType::LargePushable)
		{
			GetWorldTimerManager().ClearTimer(PairPressurePresentationTimerHandle);
			bPairPressureActionPresentationActive = false;
			RestorePairPressureLocomotionAnimation();
			return;
		}
		UAnimSequence* ReleaseAnimation = ResolvePairPressureMascotAnimation(NewGrabState);
		PlayPairPressureMascotAnimation(ReleaseAnimation, false);
		GetWorldTimerManager().SetTimer(PairPressurePresentationTimerHandle, this, &AVNHShopperCharacter::RestorePairPressureLocomotionAnimation, 0.22f, false);
		return;
	}
	if (NewGrabState == EPPGrabState::HangingFromLedge)
	{
		// A ledge handoff interrupts a dive. Clear both the delayed recovery and
		// any recovery montage that may already have begun on the landing frame.
		bPairPressureDiveRecoveryPresentationPlayed = true;
		GetWorldTimerManager().ClearTimer(PairPressureDiveRecoveryPresentationTimerHandle);
		if (USkeletalMeshComponent* CharacterMesh = GetMesh())
		{
			if (UAnimInstance* AnimInstance = CharacterMesh->GetAnimInstance())
			{
				AnimInstance->StopSlotAnimation(0.03f, TEXT("DefaultSlot"));
			}
		}
		PlayPairPressureMascotAnimation(ResolvePairPressureMascotAnimation(NewGrabState), true);
		return;
	}
	if (NewGrabState == EPPGrabState::Reaching)
	{
		// The authored reach clips are full-body climbing poses and tilt the mascot.
		// Keep the normal locomotion graph and let the two-hand IK provide the grab
		// gesture, including when no target is found.
		GetWorldTimerManager().ClearTimer(PairPressurePresentationTimerHandle);
		RestorePairPressureLocomotionAnimation();
		return;
	}
	// Holding, player-grab and push presentation come from hand IK over the normal
	// locomotion graph. This preserves the proper run cycle while carrying.
	GetWorldTimerManager().ClearTimer(PairPressurePresentationTimerHandle);
	RestorePairPressureLocomotionAnimation();
}

void AVNHShopperCharacter::HandlePairPressureGrabFailed()
{
	const FPPMascotAnimationRow* ActiveMascotRow = ResolveActiveMascotRow(TEXT("Failed grab presentation"));
	bPairPressureActionPresentationActive = true;
	PlayPairPressureMascotAnimation(ActiveMascotRow
		? (ActiveMascotRow->FailedGrab.IsNull() ? ActiveMascotRow->HitFront : ActiveMascotRow->FailedGrab).LoadSynchronous()
		: nullptr, false);
	GetWorldTimerManager().SetTimer(PairPressurePresentationTimerHandle, this, &AVNHShopperCharacter::FinishPairPressureActionPresentation, 0.30f, false);
}

void AVNHShopperCharacter::HandlePairPressureAirDiveStateChanged(bool bIsDiving)
{
	if (!bIsDiving)
	{
		PairPressureDivePresentationStartTimeSeconds = -1.0;
		bPairPressureDiveRecoveryPresentationPlayed = false;
		GetWorldTimerManager().ClearTimer(PairPressureDiveRecoveryPresentationTimerHandle);
		RestorePairPressureLocomotionAnimation();
		return;
	}
	PairPressureDivePresentationStartTimeSeconds = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.0;
	bPairPressureDiveRecoveryPresentationPlayed = false;
	UAnimSequence* DiveAnimation = PairPressureDiveAnimation.Get();
	USkeletalMeshComponent* CharacterMesh = GetMesh();
	UAnimInstance* AnimInstance = CharacterMesh ? CharacterMesh->GetAnimInstance() : nullptr;
	if (DiveAnimation && AnimInstance && !CharacterMesh->IsAnySimulatingPhysics())
	{
		const float DiveElapsedSeconds = PairPressureActionRouter
			? PairPressureActionRouter->GetDivePresentationElapsedSeconds()
			: 0.0f;
		const float DiveStartPosition = FMath::Clamp(
			0.06f + DiveElapsedSeconds * 1.12f,
			0.0f,
			FMath::Max(0.0f, DiveAnimation->GetPlayLength() - 0.02f));
		GetWorldTimerManager().ClearTimer(PairPressurePresentationTimerHandle);
		AnimInstance->StopSlotAnimation(0.0f, TEXT("DefaultSlot"));
		AnimInstance->PlaySlotAnimationAsDynamicMontage(
			DiveAnimation,
			TEXT("DefaultSlot"),
			0.0f,
			0.01f,
			1.12f,
			9999,
			-1.0f,
			DiveStartPosition);
	}
}

void AVNHShopperCharacter::HandlePairPressureAirDiveRecoveryStateChanged(bool bIsRecovering)
{
	GetWorldTimerManager().ClearTimer(PairPressureDiveRecoveryPresentationTimerHandle);
	if (!bIsRecovering || !GetWorld() || bPairPressureDiveRecoveryPresentationPlayed)
	{
		return;
	}
	bPairPressureDiveRecoveryPresentationPlayed = true;

	UAnimSequence* DiveAnimation = PairPressureDiveAnimation.Get();
	USkeletalMeshComponent* CharacterMesh = GetMesh();
	UAnimInstance* AnimInstance = CharacterMesh ? CharacterMesh->GetAnimInstance() : nullptr;
	if (DiveAnimation && AnimInstance && !CharacterMesh->IsAnySimulatingPhysics())
	{
		AnimInstance->StopSlotAnimation(0.03f, TEXT("DefaultSlot"));
		AnimInstance->PlaySlotAnimationAsDynamicMontage(
			DiveAnimation,
			TEXT("DefaultSlot"),
			0.03f,
			0.08f,
			0.001f,
			9999,
			-1.0f,
			FMath::Max(0.0f, DiveAnimation->GetPlayLength() - 0.02f));
	}
	const float RecoveryElapsedSeconds = PairPressureActionRouter
		? PairPressureActionRouter->GetDiveRecoveryPresentationElapsedSeconds()
		: 0.0f;
	const float GetUpDelaySeconds = PairPressureActionRouter
		? PairPressureActionRouter->GetDiveLandingGetUpDelaySeconds()
		: 0.30f;
	const float RemainingGetUpDelaySeconds = FMath::Max(0.01f, GetUpDelaySeconds - RecoveryElapsedSeconds);
	GetWorldTimerManager().SetTimer(
		PairPressureDiveRecoveryPresentationTimerHandle,
		this,
		&AVNHShopperCharacter::PlayPairPressureDiveRecoveryAnimation,
		RemainingGetUpDelaySeconds,
		false);
}

void AVNHShopperCharacter::PlayPairPressureDiveRecoveryAnimation()
{
	if (!PairPressureActionRouter || !PairPressureActionRouter->IsAirDiveRecovering())
	{
		return;
	}
	UAnimSequence* DiveRecoveryAnimation = LoadObject<UAnimSequence>(
		nullptr,
		TEXT("/Game/CuteChubbyPenguin/Penguin/Animations/AS_Penguin_UE_falls_forward_UP.AS_Penguin_UE_falls_forward_UP"));
	PlayPairPressureMascotAnimation(DiveRecoveryAnimation, false);
}

void AVNHShopperCharacter::HandlePairPressureGrabReleasedPresentation(bool bDroppedItem, bool bLedgeClimb)
{
	if (!bLedgeClimb)
	{
		// Ordinary player/dummy/item release must hand straight back to the
		// locomotion graph. In particular, the autonomous proxy predicts this
		// callback and suppresses the matching multicast, so playing the authored
		// full-body release clip here would mask its run cycle for the full clip.
		GetWorldTimerManager().ClearTimer(PairPressurePresentationTimerHandle);
		bPairPressureActionPresentationActive = false;
		RestorePairPressureLocomotionAnimation();
		return;
	}

	const FPPMascotAnimationRow* ActiveMascotRow = ResolveActiveMascotRow(TEXT("Grab release presentation"));
	bPairPressureActionPresentationActive = true;
	UAnimSequence* ReleaseAnimation = nullptr;
	if (bLedgeClimb)
	{
		ReleaseAnimation = LoadObject<UAnimSequence>(
			nullptr,
			TEXT("/Game/CuteChubbyPenguin/Penguin/Animations/AS_Penguin_UE_climb_all.AS_Penguin_UE_climb_all"));
	}
	else if (ActiveMascotRow)
	{
		const TSoftObjectPtr<UAnimSequence>& RequestedRelease = bDroppedItem
			? (ActiveMascotRow->ItemDrop.IsNull() ? ActiveMascotRow->Throw : ActiveMascotRow->ItemDrop)
			: (ActiveMascotRow->GrabRelease.IsNull() ? ActiveMascotRow->Throw : ActiveMascotRow->GrabRelease);
		ReleaseAnimation = RequestedRelease.LoadSynchronous();
	}
	PlayPairPressureMascotAnimation(ReleaseAnimation, false);
	const float ReleasePresentationSeconds = ReleaseAnimation
		? FMath::Max(0.32f, ReleaseAnimation->GetPlayLength())
		: 0.32f;
	GetWorldTimerManager().SetTimer(
		PairPressurePresentationTimerHandle,
		this,
		&AVNHShopperCharacter::FinishPairPressureActionPresentation,
		ReleasePresentationSeconds,
		false);
}

void AVNHShopperCharacter::HandlePairPressureGrabThrowPresentation(bool bChargedThrow)
{
	const FPPMascotAnimationRow* ActiveMascotRow = ResolveActiveMascotRow(TEXT("Throw presentation"));
	UAnimSequence* ThrowAnimation = ActiveMascotRow
		? (bChargedThrow ? ActiveMascotRow->OverheadThrow : ActiveMascotRow->Throw).LoadSynchronous()
		: nullptr;
	bPairPressureActionPresentationActive = true;
	PlayPairPressureMascotAnimation(ThrowAnimation, false);
	const float PresentationSeconds = ThrowAnimation ? FMath::Clamp(ThrowAnimation->GetPlayLength(), 0.35f, 1.1f) : 0.45f;
	GetWorldTimerManager().SetTimer(PairPressurePresentationTimerHandle, this, &AVNHShopperCharacter::FinishPairPressureActionPresentation, PresentationSeconds, false);
}

void AVNHShopperCharacter::FinishPairPressureActionPresentation()
{
	bPairPressureActionPresentationActive = false;
	RestorePairPressureLocomotionAnimation();
}

void AVNHShopperCharacter::ApplyPublicTest(EVNHPublicTestType TestType)
{
	if (!HasAuthority())
	{
		return;
	}

	if (RoutineComponent)
	{
		RoutineComponent->SetContext(EVNHShopperContext::Reacting, RoutineComponent->GetSnapshot().SuggestedNextActivity);
	}

	const bool bShouldAutoReactAsNpc = !IsPlayerControlled();
	if (TestType == EVNHPublicTestType::Freeze && bShouldAutoReactAsNpc)
	{
		SetFrozenByPublicTest(true);
		GetWorldTimerManager().SetTimer(FreezeTestTimerHandle, this, &AVNHShopperCharacter::ClearPublicTestFreeze, FreezeTestHoldSeconds, false);
	}
	else if (TestType == EVNHPublicTestType::LookToEntrance && bShouldAutoReactAsNpc)
	{
		ApplyLookToEntranceReaction();
	}
	else if (TestType == EVNHPublicTestType::ClearAisle && bShouldAutoReactAsNpc)
	{
		ApplyClearAisleReaction();
	}

	OnPublicTestReceived.Broadcast(TestType);
}

void AVNHShopperCharacter::OnRep_PossessedByAlien()
{
}

void AVNHShopperCharacter::OnRep_FrozenByPublicTest()
{
	ApplyFrozenVisualState();
}

void AVNHShopperCharacter::SetFrozenByPublicTest(bool bNewFrozen)
{
	if (!HasAuthority() || bFrozenByPublicTest == bNewFrozen)
	{
		return;
	}

	bFrozenByPublicTest = bNewFrozen;
	ApplyFrozenVisualState();
}

void AVNHShopperCharacter::ClearPublicTestFreeze()
{
	SetFrozenByPublicTest(false);
	ResumeRoutineMovement();
}

void AVNHShopperCharacter::ApplyFrozenVisualState()
{
	if (USkeletalMeshComponent* MeshComponent = GetMesh())
	{
		MeshComponent->bPauseAnims = bFrozenByPublicTest;
		MeshComponent->GlobalAnimRateScale = bFrozenByPublicTest ? 0.0f : 1.0f;
	}

	if (UCharacterMovementComponent* MovementComponent = GetCharacterMovement())
	{
		if (bFrozenByPublicTest)
		{
			MovementComponent->StopMovementImmediately();
			MovementComponent->DisableMovement();
		}
		else
		{
			MovementComponent->SetMovementMode(MOVE_Walking);
		}
	}

	if (bFrozenByPublicTest)
	{
		if (AController* CurrentController = GetController())
		{
			CurrentController->StopMovement();
		}
	}
}

void AVNHShopperCharacter::StartUniversalActionMovementLock(float DurationSeconds)
{
	UWorld* World = GetWorld();
	UCharacterMovementComponent* MovementComponent = GetCharacterMovement();
	if (!World || !MovementComponent || bFrozenByPublicTest)
	{
		return;
	}

	MovementComponent->StopMovementImmediately();
	MovementComponent->DisableMovement();
	if (AController* CurrentController = GetController())
	{
		CurrentController->StopMovement();
	}

	World->GetTimerManager().ClearTimer(UniversalActionMovementLockTimerHandle);
	World->GetTimerManager().SetTimer(
		UniversalActionMovementLockTimerHandle,
		this,
		&AVNHShopperCharacter::ClearUniversalActionMovementLock,
		FMath::Max(0.15f, DurationSeconds),
		false);
}

void AVNHShopperCharacter::ClearUniversalActionMovementLock()
{
	if (bFrozenByPublicTest)
	{
		return;
	}

	if (UCharacterMovementComponent* MovementComponent = GetCharacterMovement())
	{
		MovementComponent->SetMovementMode(MOVE_Walking);
	}
}

void AVNHShopperCharacter::ApplyLookToEntranceReaction()
{
	if (AController* CurrentController = GetController())
	{
		CurrentController->StopMovement();
	}

	const FVector EntranceLocation(0.0f, -760.0f, GetActorLocation().Z);
	const FVector ToEntrance = (EntranceLocation - GetActorLocation()).GetSafeNormal2D();
	if (!ToEntrance.IsNearlyZero())
	{
		SetActorRotation(FRotator(0.0f, ToEntrance.Rotation().Yaw, 0.0f));
	}

	GetWorldTimerManager().ClearTimer(PublicTestReactionTimerHandle);
	GetWorldTimerManager().SetTimer(PublicTestReactionTimerHandle, this, &AVNHShopperCharacter::ResumeRoutineMovement, 0.85f, false);
}

void AVNHShopperCharacter::ApplyClearAisleReaction()
{
	if (AController* CurrentController = GetController())
	{
		CurrentController->StopMovement();
	}

	if (UCharacterMovementComponent* MovementComponent = GetCharacterMovement())
	{
		MovementComponent->SetMovementMode(MOVE_Walking);
		MovementComponent->MaxWalkSpeed = 125.0f;
	}

	const float SideSign = GetActorLocation().Y >= 0.0f ? 1.0f : -1.0f;
	const FVector SideDirection(0.0f, SideSign, 0.0f);
	AddMovementInput(SideDirection, 1.0f);
	LaunchCharacter(SideDirection * 115.0f, false, false);
	SetActorRotation(FRotator(0.0f, SideDirection.Rotation().Yaw, 0.0f));

	GetWorldTimerManager().ClearTimer(PublicTestReactionTimerHandle);
	GetWorldTimerManager().SetTimer(PublicTestReactionTimerHandle, this, &AVNHShopperCharacter::ResumeRoutineMovement, 1.1f, false);
}

void AVNHShopperCharacter::ResumeRoutineMovement()
{
	if (!HasAuthority() || bPossessedByAlien || !RoutineComponent || RoutineComponent->IsRoutinePaused() || !RoutineComponent->GetCurrentWaypoint())
	{
		return;
	}

	AVNHShopperWaypoint* Waypoint = RoutineComponent->GetCurrentWaypoint();
	AVNHShopperAIController* ShopperAIController = Cast<AVNHShopperAIController>(GetController());
	if (Waypoint && ShopperAIController)
	{
		ShopperAIController->StartRoutineMove();
	}
}

void AVNHShopperCharacter::StartRoutineMovement()
{
	if (!HasAuthority() || bPossessedByAlien || !RoutineComponent || RoutineComponent->IsRoutinePaused())
	{
		return;
	}

	if (!GetController())
	{
		SpawnDefaultController();
	}

	if (AVNHShopperAIController* ShopperAIController = Cast<AVNHShopperAIController>(GetController()))
	{
		ShopperAIController->StartRoutineMove();
	}
}

FString AVNHShopperCharacter::BuildQuestionResponse() const
{
	if (!RoutineComponent)
	{
		return TEXT("I am just browsing.");
	}

	const FVNHShopperRoutineSnapshot Snapshot = RoutineComponent->GetSnapshot();
	const TCHAR* ContextText = TEXT("shopping");
	switch (Snapshot.Context)
	{
	case EVNHShopperContext::Browsing:
		ContextText = TEXT("looking at clothes");
		break;
	case EVNHShopperContext::Mirror:
		ContextText = TEXT("using the mirror");
		break;
	case EVNHShopperContext::Checkout:
		ContextText = TEXT("waiting to pay");
		break;
	case EVNHShopperContext::Walking:
		ContextText = TEXT("walking to another aisle");
		break;
	case EVNHShopperContext::Idle:
		ContextText = TEXT("taking a second");
		break;
	case EVNHShopperContext::Reacting:
		ContextText = TEXT("reacting to that announcement");
		break;
	default:
		break;
	}

	const FString HeldPropText = Snapshot.HeldProp.IsNone() ? TEXT("nothing") : Snapshot.HeldProp.ToString();
	const FString NextText = Snapshot.SuggestedNextActivity.IsNone() ? TEXT("keep shopping") : Snapshot.SuggestedNextActivity.ToString();
	return FString::Printf(TEXT("\"I'm %s. I have %s. I was about to %s.\""), ContextText, *HeldPropText, *NextText);
}

FString AVNHShopperCharacter::DescribeAnimationDebugState() const
{
	USkeletalMeshComponent* MeshComponent = GetMesh();
	const UAnimInstance* AnimInstance = MeshComponent ? MeshComponent->GetAnimInstance() : nullptr;
	const UCharacterMovementComponent* MovementComponent = GetCharacterMovement();
	const FVector Velocity = GetVelocity();
	const FString ShopperName = GetNameSafe(this);
	const FString MeshName = MeshComponent ? GetNameSafe(MeshComponent->GetSkeletalMeshAsset()) : TEXT("None");
	const FString AnimClassName = MeshComponent ? GetNameSafe(MeshComponent->GetAnimClass()) : TEXT("None");
	const FString AnimInstanceName = GetNameSafe(AnimInstance);
	const FString PauseText = MeshComponent && MeshComponent->bPauseAnims ? TEXT("true") : TEXT("false");

	return FString::Printf(
		TEXT("Shopper=%s Mesh=%s AnimClass=%s AnimInstance=%s Mode=%d Pause=%s Rate=%.2f SpeedXY=%.1f MaxWalk=%.1f MoveMode=%d"),
		*ShopperName,
		*MeshName,
		*AnimClassName,
		*AnimInstanceName,
		MeshComponent ? static_cast<int32>(MeshComponent->GetAnimationMode()) : -1,
		*PauseText,
		MeshComponent ? MeshComponent->GlobalAnimRateScale : 0.0f,
		Velocity.Size2D(),
		MovementComponent ? MovementComponent->MaxWalkSpeed : 0.0f,
		MovementComponent ? static_cast<int32>(MovementComponent->MovementMode) : -1);
}

void AVNHShopperCharacter::SetInteractionHighlighted(bool bHighlighted)
{
	SetInteractionHighlightStencil(bHighlighted ? 89 : 0);
}

void AVNHShopperCharacter::SetInteractionHighlightStencil(int32 StencilValue)
{
	if (USkeletalMeshComponent* MeshComponent = GetMesh())
	{
		MeshComponent->SetRenderCustomDepth(StencilValue > 0);
		MeshComponent->SetCustomDepthStencilValue(FMath::Clamp(StencilValue, 0, 255));
	}

	if (DebugBodyMesh)
	{
		DebugBodyMesh->SetRenderCustomDepth(StencilValue > 0);
		DebugBodyMesh->SetCustomDepthStencilValue(FMath::Clamp(StencilValue, 0, 255));
	}
}

bool AVNHShopperCharacter::ShouldUsePairPressureMascotVisuals() const
{
	const FString MapName = GetWorld() ? GetWorld()->GetMapName() : FString();
	return MapName.Contains(TEXT("Lobby")) || MapName.Contains(TEXT("PP_"));
}

FName AVNHShopperCharacter::GetSelectedMascotRowName_Implementation() const
{
	return ActiveMascotRowName.IsNone() ? FName(TEXT("Penguin")) : ActiveMascotRowName;
}

void AVNHShopperCharacter::ApplySelectedMascotRowName_Implementation(FName InMascotRowName)
{
	if (InMascotRowName.IsNone() || ActiveMascotRowName == InMascotRowName)
	{
		return;
	}

	const FName PreviousMascotRowName = ActiveMascotRowName;
	ActiveMascotRowName = InMascotRowName;
	if (!ResolveActiveMascotRow(TEXT("Apply mascot selection")))
	{
		ActiveMascotRowName = PreviousMascotRowName.IsNone()
			? FName(TEXT("Penguin"))
			: PreviousMascotRowName;
		return;
	}

	PairPressureObstacleFallAnimations.Reset();
	PairPressureDiveAnimation = nullptr;
	ConfigureCharacterVisuals();
	ApplyCharacterCustomization();
	PreloadPairPressureObstacleFallAnimations();
}

const FPPMascotAnimationRow* AVNHShopperCharacter::ResolveActiveMascotRow(const TCHAR* Context) const
{
	const UDataTable* LoadedMascotTable = MascotAnimationTable.IsNull()
		? nullptr
		: MascotAnimationTable.LoadSynchronous();
	if (!LoadedMascotTable)
	{
		return nullptr;
	}

	const FName MascotRowName = GetSelectedMascotRowName_Implementation();
	if (const FPPMascotAnimationRow* MascotRow = LoadedMascotTable->FindRow<FPPMascotAnimationRow>(
		MascotRowName,
		Context,
		false))
	{
		return MascotRow;
	}

	return LoadedMascotTable->FindRow<FPPMascotAnimationRow>(
		FName(TEXT("Penguin")),
		TEXT("Fallback Penguin mascot"),
		false);
}

void AVNHShopperCharacter::ConfigureCharacterVisuals()
{
	USkeletalMeshComponent* MeshComponent = GetMesh();
	if (!MeshComponent)
	{
		return;
	}

	const FPPMascotAnimationRow* ActiveMascotRow = ResolveActiveMascotRow(TEXT("Configure active mascot"));
	const bool bForcePairPressureMascot = ShouldUsePairPressureMascotVisuals();

	if (bForcePairPressureMascot || !MeshComponent->GetSkeletalMeshAsset())
	{
		USkeletalMesh* DefaultBodyMesh = ActiveMascotRow ? ActiveMascotRow->Mesh.LoadSynchronous() : nullptr;
		if (!DefaultBodyMesh)
		{
			DefaultBodyMesh = LoadObject<USkeletalMesh>(nullptr, DefaultBodyMeshPath);
		}
		if (DefaultBodyMesh)
		{
			MeshComponent->SetSkeletalMesh(DefaultBodyMesh);
		}
	}

	MeshComponent->SetRelativeLocation(FVector(0.0f, 0.0f, -88.0f));
	MeshComponent->SetRelativeRotation(FRotator(0.0f, -90.0f, 0.0f));
	MeshComponent->VisibilityBasedAnimTickOption = EVisibilityBasedAnimTickOption::AlwaysTickPoseAndRefreshBones;
	MeshComponent->bEnableUpdateRateOptimizations = false;
	MeshComponent->bPauseAnims = false;
	MeshComponent->GlobalAnimRateScale = 1.0f;

	if (bForcePairPressureMascot || !MeshComponent->GetAnimClass())
	{
		UClass* MascotLocomotionAnimClass = ActiveMascotRow ? ActiveMascotRow->AnimationBlueprint.LoadSynchronous() : nullptr;
		if (!MascotLocomotionAnimClass)
		{
			MascotLocomotionAnimClass = LoadClass<UAnimInstance>(nullptr, DefaultMascotAnimBlueprintPath);
		}
		if (MascotLocomotionAnimClass)
		{
			MeshComponent->SetAnimationMode(EAnimationMode::AnimationBlueprint);
			MeshComponent->SetAnimInstanceClass(MascotLocomotionAnimClass);
		}
	}
	if (bForcePairPressureMascot)
	{
		if (UPhysicsAsset* PairPressurePhysicsAsset = LoadObject<UPhysicsAsset>(nullptr, PairPressureMascotPhysicsAssetPath))
		{
			MeshComponent->SetPhysicsAsset(PairPressurePhysicsAsset, true);
		}
	}

	if (!MeshComponent->GetAnimClass())
	{
		UAnimationAsset* IdleAnimation = ActiveMascotRow ? ActiveMascotRow->Idle.LoadSynchronous() : nullptr;
		if (!IdleAnimation)
		{
			IdleAnimation = LoadObject<UAnimationAsset>(nullptr, DefaultMascotIdleAnimPath);
		}
		if (IdleAnimation)
		{
			MeshComponent->SetAnimationMode(EAnimationMode::AnimationSingleNode);
			MeshComponent->SetAnimation(IdleAnimation);
			MeshComponent->Play(true);
		}
	}

	if (DebugBodyMesh && MeshComponent->GetSkeletalMeshAsset())
	{
		DebugBodyMesh->SetHiddenInGame(true);
	}
}

void AVNHShopperCharacter::ApplyCharacterCustomization()
{
	ConfigureCharacterVisuals();

	USkeletalMeshComponent* BodyComponent = GetMesh();
	if (!BodyComponent)
	{
		return;
	}
	if (ShouldUsePairPressureMascotVisuals())
	{
		const TSoftObjectPtr<USkeletalMesh> EmptyCosmeticMesh;
		ApplySlotMesh(HairMeshComponent, EmptyCosmeticMesh);
		ApplySlotMesh(FaceMeshComponent, EmptyCosmeticMesh);
		ApplySlotMesh(HatMeshComponent, EmptyCosmeticMesh);
		ApplySlotMesh(MustacheMeshComponent, EmptyCosmeticMesh);
		ApplySlotMesh(OutfitMeshComponent, EmptyCosmeticMesh);
		ApplySlotMesh(OutwearMeshComponent, EmptyCosmeticMesh);
		ApplySlotMesh(PantsMeshComponent, EmptyCosmeticMesh);
		ApplySlotMesh(ShoesMeshComponent, EmptyCosmeticMesh);
		ApplySlotMesh(AccessoryMeshComponent, EmptyCosmeticMesh);
		return;
	}

	ApplySlotMesh(BodyComponent, CharacterCustomization.BodyMesh);
	ApplySlotMesh(HairMeshComponent, CharacterCustomization.HairMesh);
	ApplySlotMesh(FaceMeshComponent, CharacterCustomization.FaceMesh, CharacterCustomization.bNoFace);
	ApplySlotMesh(HatMeshComponent, CharacterCustomization.HatMesh);
	ApplySlotMesh(MustacheMeshComponent, CharacterCustomization.MustacheMesh);
	ApplySlotMesh(OutfitMeshComponent, CharacterCustomization.OutfitMesh);
	ApplySlotMesh(OutwearMeshComponent, CharacterCustomization.OutwearMesh);
	ApplySlotMesh(PantsMeshComponent, CharacterCustomization.PantsMesh);
	ApplySlotMesh(ShoesMeshComponent, CharacterCustomization.ShoesMesh);
	ApplySlotMesh(AccessoryMeshComponent, CharacterCustomization.AccessoryMesh);

	ApplyColorToMesh(BodyComponent, CharacterCustomization.BodyColor);
	ApplyColorToMesh(HairMeshComponent, CharacterCustomization.HairColor);
	ApplyColorToMesh(OutfitMeshComponent, CharacterCustomization.OutfitColor);
	ApplyColorToMesh(OutwearMeshComponent, CharacterCustomization.OutfitColor);
}

void AVNHShopperCharacter::ApplySlotMesh(USkeletalMeshComponent* SlotComponent, const TSoftObjectPtr<USkeletalMesh>& MeshAsset, bool bHideSlot)
{
	if (!SlotComponent)
	{
		return;
	}

	USkeletalMesh* LoadedMesh = MeshAsset.IsNull() ? nullptr : MeshAsset.LoadSynchronous();
	SlotComponent->SetSkeletalMesh(LoadedMesh);
	SlotComponent->SetHiddenInGame(bHideSlot || LoadedMesh == nullptr);
	SlotComponent->SetVisibility(!bHideSlot && LoadedMesh != nullptr, true);
	if (LoadedMesh && SlotComponent != GetMesh())
	{
		SlotComponent->SetLeaderPoseComponent(GetMesh());
	}
}

void AVNHShopperCharacter::ApplyColorToMesh(USkeletalMeshComponent* MeshComponent, const FLinearColor& Color)
{
	if (!MeshComponent || !MeshComponent->GetSkeletalMeshAsset())
	{
		return;
	}

	const int32 MaterialCount = MeshComponent->GetNumMaterials();
	for (int32 MaterialIndex = 0; MaterialIndex < MaterialCount; ++MaterialIndex)
	{
		UMaterialInstanceDynamic* DynamicMaterial = MeshComponent->CreateAndSetMaterialInstanceDynamic(MaterialIndex);
		if (!DynamicMaterial)
		{
			continue;
		}

		DynamicMaterial->SetVectorParameterValue(TEXT("Color"), Color);
		DynamicMaterial->SetVectorParameterValue(TEXT("BaseColor"), Color);
		DynamicMaterial->SetVectorParameterValue(TEXT("Tint"), Color);
	}
}
