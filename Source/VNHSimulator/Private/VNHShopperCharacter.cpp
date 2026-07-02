#include "VNHShopperCharacter.h"

#include "Camera/CameraComponent.h"
#include "Animation/AnimationAsset.h"
#include "Animation/AnimInstance.h"
#include "Animation/AnimMontage.h"
#include "Components/AudioComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "DrawDebugHelpers.h"
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
#include "Materials/MaterialInstanceDynamic.h"
#include "VNHAlienLocomotionComponent.h"
#include "VNHGameState.h"
#include "VNHLog.h"
#include "VNHPlayerState.h"
#include "VNHShopperAIController.h"
#include "VNHShopperWaypoint.h"
#include "Net/UnrealNetwork.h"

namespace
{
const TCHAR* DefaultBodyMeshPath = TEXT("/Game/Creative_Characters/Skeleton_Meshes/SK_Body_009.SK_Body_009");
const TCHAR* CreativeAnimClassPath = TEXT("/Game/Creative_Characters/Animations/ABP_CreativeCharacter.ABP_CreativeCharacter_C");
const TCHAR* CreativeIdleAnimPath = TEXT("/Game/TNG/Characters/Animations/ANIM_TNG_Idle_Breathing.ANIM_TNG_Idle_Breathing");
constexpr float CalmFartVolume = 1.4f;
constexpr float PanicFartVolume = 2.6f;
constexpr float CalmFartInnerRadius = 275.0f;
constexpr float PanicFartInnerRadius = 700.0f;

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
}

AVNHShopperCharacter::AVNHShopperCharacter()
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.TickInterval = 0.25f;
	bReplicates = true;
	AIControllerClass = AVNHShopperAIController::StaticClass();
	AutoPossessAI = EAutoPossessAI::PlacedInWorldOrSpawned;
	bUseControllerRotationYaw = false;

	if (UCharacterMovementComponent* MovementComponent = GetCharacterMovement())
	{
		MovementComponent->bOrientRotationToMovement = false;
		MovementComponent->RotationRate = FRotator(0.0f, 150.0f, 0.0f);
		MovementComponent->MaxWalkSpeed = 135.0f;
		MovementComponent->MaxAcceleration = 420.0f;
		MovementComponent->BrakingDecelerationWalking = 360.0f;
		MovementComponent->GroundFriction = 6.0f;
	}

	RoutineComponent = CreateDefaultSubobject<UVNHRoutineComponent>(TEXT("RoutineComponent"));
	AlienLocomotionComponent = CreateDefaultSubobject<UVNHAlienLocomotionComponent>(TEXT("AlienLocomotionComponent"));

	FollowCameraBoom = CreateDefaultSubobject<USpringArmComponent>(TEXT("FollowCameraBoom"));
	FollowCameraBoom->SetupAttachment(RootComponent);
	FollowCameraBoom->TargetArmLength = 420.0f;
	FollowCameraBoom->SetRelativeRotation(FRotator(-18.0f, 0.0f, 0.0f));
	FollowCameraBoom->bUsePawnControlRotation = true;
	FollowCameraBoom->bEnableCameraLag = true;
	FollowCameraBoom->CameraLagSpeed = 12.0f;

	FollowCamera = CreateDefaultSubobject<UCameraComponent>(TEXT("FollowCamera"));
	FollowCamera->SetupAttachment(FollowCameraBoom, USpringArmComponent::SocketName);
	FollowCamera->bUsePawnControlRotation = false;

	FirstPersonCamera = CreateDefaultSubobject<UCameraComponent>(TEXT("FirstPersonCamera"));
	FirstPersonCamera->SetupAttachment(RootComponent);
	FirstPersonCamera->SetRelativeLocation(FVector(8.0f, 0.0f, 64.0f));
	FirstPersonCamera->bUsePawnControlRotation = true;
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
	CharacterCustomization.HairMesh = TSoftObjectPtr<USkeletalMesh>(FSoftObjectPath(TEXT("/Game/TNG/Characters/Cosmetics/Hair/SK_TNG_Hair_Male_003.SK_TNG_Hair_Male_003")));
	CharacterCustomization.FaceMesh = TSoftObjectPtr<USkeletalMesh>(FSoftObjectPath(TEXT("/Game/TNG/Characters/Cosmetics/Faces/SK_TNG_Face_Happy_001.SK_TNG_Face_Happy_001")));
	CharacterCustomization.HatMesh = TSoftObjectPtr<USkeletalMesh>(FSoftObjectPath(TEXT("/Game/TNG/Characters/Cosmetics/Hats/SK_TNG_Hat_003.SK_TNG_Hat_003")));
	CharacterCustomization.MustacheMesh = TSoftObjectPtr<USkeletalMesh>(FSoftObjectPath(TEXT("/Game/TNG/Characters/Cosmetics/Mustaches/SK_TNG_Mustache_001.SK_TNG_Mustache_001")));
	CharacterCustomization.OutfitMesh = TSoftObjectPtr<USkeletalMesh>(FSoftObjectPath(TEXT("/Game/TNG/Characters/Cosmetics/Outfits/SK_TNG_Outfit_001.SK_TNG_Outfit_001")));

	if (USkeletalMesh* DefaultBodyMesh = LoadObject<USkeletalMesh>(nullptr, DefaultBodyMeshPath))
	{
		GetMesh()->SetSkeletalMesh(DefaultBodyMesh);
		ConfigureCreativeCharacterVisuals();
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

	ConfigureCreativeCharacterVisuals();
	ApplyCharacterCustomization();
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

	if (HasAuthority() && IsPlayerControlled())
	{
		UpdateComposureSystem(DeltaSeconds);
	}
}

void AVNHShopperCharacter::PossessedBy(AController* NewController)
{
	Super::PossessedBy(NewController);

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
	DOREPLIFETIME(AVNHShopperCharacter, HeldProp);
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

bool AVNHShopperCharacter::UseActNatural()
{
	if (!HasAuthority() || !IsPlayerControlled() || !bActNaturalAvailable || !RoutineComponent)
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
	if (!HasAuthority() || FMath::IsNearlyZero(Delta))
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
		HeldProp = NewHeldProp;
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

	FartCooldownRemaining = FMath::Max(0.0f, FartCooldownRemaining - DeltaSeconds);

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

	if (InactivitySeconds >= CurrentFartThreshold && FartCooldownRemaining <= 0.0f)
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
	if (!HasAuthority() || FartCooldownRemaining > 0.0f)
	{
		return false;
	}

	MulticastTriggerFart();
	ApplyComposureDelta(-20.0f, TEXT("PublicFart"));
	FartCooldownRemaining = 30.0f;
	ResetInactivity();

	for (TActorIterator<AVNHShopperCharacter> It(GetWorld()); It; ++It)
	{
		AVNHShopperCharacter* NearbyShopper = *It;
		if (NearbyShopper && NearbyShopper != this && NearbyShopper->IsPlayerControlled()
			&& FVector::DistSquared(NearbyShopper->GetActorLocation(), GetActorLocation()) <= FMath::Square(700.0f))
		{
			NearbyShopper->ApplyComposureDelta(-3.0f, TEXT("NearbyFart"));
		}
	}

	return true;
}

bool AVNHShopperCharacter::TriggerFartFromAction()
{
	return TriggerFart();
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
		if (Actor && Actor != HeldProp
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
			GetActorLocation(),
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
			GetActorLocation(),
			FRotator::ZeroRotator,
			VolumeMultiplier,
			1.0f,
			0.0f,
			DynamicAttenuation);
	}

	const FVector CloudCenter = GetActorLocation() + FVector(0.0f, 0.0f, 35.0f);
	DrawDebugSphere(GetWorld(), CloudCenter + FVector(0.0f, -18.0f, 0.0f), 18.0f, 10, FColor(125, 180, 70), false, 2.0f, 0, 2.0f);
	DrawDebugSphere(GetWorld(), CloudCenter + FVector(0.0f, 8.0f, 10.0f), 24.0f, 10, FColor(155, 195, 85), false, 2.0f, 0, 2.0f);
	DrawDebugSphere(GetWorld(), CloudCenter + FVector(0.0f, 28.0f, 22.0f), 14.0f, 10, FColor(180, 205, 95), false, 2.0f, 0, 2.0f);
	UE_LOG(LogVNH, Display, TEXT("FartEvent: %s broke composure in public. SourceComposure=%.1f Volume=%.2f InnerRadius=%.0f MaxDistance=%.0f"),
		*GetNameSafe(this),
		SourceComposure,
		VolumeMultiplier,
		InnerRadius,
		InnerRadius + FalloffDistance);
}

void AVNHShopperCharacter::MulticastPlayUniversalActionMontage_Implementation(UAnimMontage* Montage)
{
	if (!Montage)
	{
		return;
	}

	USkeletalMeshComponent* MeshComponent = GetMesh();
	UAnimInstance* AnimInstance = MeshComponent ? MeshComponent->GetAnimInstance() : nullptr;
	if (AnimInstance)
	{
		AnimInstance->Montage_Play(Montage);
	}
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
		CharacterCustomization = NewCustomization;
		ApplyCharacterCustomization();
	}
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

void AVNHShopperCharacter::ConfigureCreativeCharacterVisuals()
{
	USkeletalMeshComponent* MeshComponent = GetMesh();
	if (!MeshComponent)
	{
		return;
	}

	if (!MeshComponent->GetSkeletalMeshAsset())
	{
		if (USkeletalMesh* DefaultBodyMesh = LoadObject<USkeletalMesh>(nullptr, DefaultBodyMeshPath))
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

	if (UClass* CreativeAnimClass = LoadClass<UAnimInstance>(nullptr, CreativeAnimClassPath))
	{
		MeshComponent->SetAnimationMode(EAnimationMode::AnimationBlueprint);
		MeshComponent->SetAnimInstanceClass(CreativeAnimClass);
	}
	else if (!MeshComponent->GetAnimClass())
	{
		if (UAnimationAsset* IdleAnimation = LoadObject<UAnimationAsset>(nullptr, CreativeIdleAnimPath))
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
	ConfigureCreativeCharacterVisuals();

	USkeletalMeshComponent* BodyComponent = GetMesh();
	if (!BodyComponent)
	{
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
