#include "VNHCustomizationPreviewActor.h"

#include "Animation/AnimInstance.h"
#include "Animation/AnimationAsset.h"
#include "Components/DirectionalLightComponent.h"
#include "Components/PointLightComponent.h"
#include "Components/SceneCaptureComponent2D.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/World.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "TimerManager.h"
#include "VNHLog.h"

namespace
{
const TCHAR* DefaultBodyMeshPath = TEXT("/Game/Creative_Characters/Skeleton_Meshes/SK_Body_009.SK_Body_009");
const TCHAR* CreativeAnimClassPath = TEXT("/Game/Creative_Characters/Animations/ABP_CreativeCharacter.ABP_CreativeCharacter_C");
const TCHAR* CreativeIdleAnimPath = TEXT("/Game/TNG/Characters/Animations/TryingOnClothesAnims/Female/ANIM_TNG_Idle_Breathing.ANIM_TNG_Idle_Breathing");
const TCHAR* MaleIdleAnimPath = TEXT("/Game/TNG/Characters/Animations/TryingOnClothesAnims/Male/A_Male_Idle.A_Male_Idle");
const TCHAR* FemaleIdleAnimPath = TEXT("/Game/TNG/Characters/Animations/TryingOnClothesAnims/Female/A_Female_Idle.A_Female_Idle");
const TCHAR* MaleHatPutOnAnimPath = TEXT("/Game/TNG/Characters/Animations/TryingOnClothesAnims/Male/A_Male_Hat_PutOn.A_Male_Hat_PutOn");
const TCHAR* MaleHatTakeOffAnimPath = TEXT("/Game/TNG/Characters/Animations/TryingOnClothesAnims/Male/A_Male_Hat_TakeOff.A_Male_Hat_TakeOff");
const TCHAR* MaleGlassesPutOnAnimPath = TEXT("/Game/TNG/Characters/Animations/TryingOnClothesAnims/Male/A_Male_Glasses_PutOn.A_Male_Glasses_PutOn");
const TCHAR* MaleGlassesTakeOffAnimPath = TEXT("/Game/TNG/Characters/Animations/TryingOnClothesAnims/Male/A_Male_Glasses_TakeOff.A_Male_Glasses_TakeOff");
const TCHAR* MaleMaskPutOnAnimPath = TEXT("/Game/TNG/Characters/Animations/TryingOnClothesAnims/Male/A_Male_Mask_PutOn.A_Male_Mask_PutOn");
const TCHAR* MaleMaskTakeOffAnimPath = TEXT("/Game/TNG/Characters/Animations/TryingOnClothesAnims/Male/A_Male_Mask_TakeOff.A_Male_Mask_TakeOff");
const TCHAR* MalePantsPutOnAnimPath = TEXT("/Game/TNG/Characters/Animations/TryingOnClothesAnims/Male/A_Male_Pants_PutOn.A_Male_Pants_PutOn");
const TCHAR* MaleShoesPutOnAnimPath = TEXT("/Game/TNG/Characters/Animations/TryingOnClothesAnims/Male/A_Male_Shoes_01.A_Male_Shoes_01");
const TCHAR* MaleTorsoPutOnAnimPath = TEXT("/Game/TNG/Characters/Animations/TryingOnClothesAnims/Male/A_Male_Torso_PutOn.A_Male_Torso_PutOn");
const TCHAR* MaleTorsoTakeOffAnimPath = TEXT("/Game/TNG/Characters/Animations/TryingOnClothesAnims/Male/A_Male_Torso_TakeOff.A_Male_Torso_TakeOff");
const TCHAR* FemaleHatPutOnAnimPath = TEXT("/Game/TNG/Characters/Animations/TryingOnClothesAnims/Female/A_Female_Hat_PutOn.A_Female_Hat_PutOn");
const TCHAR* FemaleHatTakeOffAnimPath = TEXT("/Game/TNG/Characters/Animations/TryingOnClothesAnims/Female/A_Female_Hat_TakeOff.A_Female_Hat_TakeOff");
const TCHAR* FemaleGlassesPutOnAnimPath = TEXT("/Game/TNG/Characters/Animations/TryingOnClothesAnims/Female/A_Female_Glasses_PutOn.A_Female_Glasses_PutOn");
const TCHAR* FemaleGlassesTakeOffAnimPath = TEXT("/Game/TNG/Characters/Animations/TryingOnClothesAnims/Female/A_Female_Glasses_TakeOff.A_Female_Glasses_TakeOff");
const TCHAR* FemaleMaskPutOnAnimPath = TEXT("/Game/TNG/Characters/Animations/TryingOnClothesAnims/Female/A_Female_Mask_PutOn.A_Female_Mask_PutOn");
const TCHAR* FemaleMaskTakeOffAnimPath = TEXT("/Game/TNG/Characters/Animations/TryingOnClothesAnims/Female/A_Female_Mask_TakeOff.A_Female_Mask_TakeOff");
const TCHAR* FemalePantsPutOnAnimPath = TEXT("/Game/TNG/Characters/Animations/TryingOnClothesAnims/Female/A_Female_Pants_PutOn.A_Female_Pants_PutOn");
const TCHAR* FemalePantsTakeOffAnimPath = TEXT("/Game/TNG/Characters/Animations/TryingOnClothesAnims/Female/A_Female_Pants_TakeOff.A_Female_Pants_TakeOff");
const TCHAR* FemaleShoesPutOnAnimPath = TEXT("/Game/TNG/Characters/Animations/TryingOnClothesAnims/Female/A_Female_Shoes_PutOn.A_Female_Shoes_PutOn");
const TCHAR* FemaleTorsoPutOnAnimPath = TEXT("/Game/TNG/Characters/Animations/TryingOnClothesAnims/Female/A_Female_Torso_PutOn.A_Female_Torso_PutOn");
const TCHAR* FemaleTorsoTakeOffAnimPath = TEXT("/Game/TNG/Characters/Animations/TryingOnClothesAnims/Female/A_Female_Torso_TakeOff.A_Female_Torso_TakeOff");
constexpr int32 PreviewRenderTargetWidth = 768;
constexpr int32 PreviewRenderTargetHeight = 1184;
const FVector BodyPreviewBaseLocation(0.0f, 0.0f, -54.0f);
const FRotator BodyPreviewBaseRotation(0.0f, -90.0f, 0.0f);
const FVector BodyPreviewBaseScale(1.0f, 1.0f, 1.0f);
}

AVNHCustomizationPreviewActor::AVNHCustomizationPreviewActor()
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.TickInterval = 0.0f;
	bReplicates = false;
	SetCanBeDamaged(false);

	PreviewRoot = CreateDefaultSubobject<USceneComponent>(TEXT("PreviewRoot"));
	SetRootComponent(PreviewRoot);

	CharacterRoot = CreateDefaultSubobject<USceneComponent>(TEXT("CharacterRoot"));
	CharacterRoot->SetupAttachment(PreviewRoot);

	BodyMeshComponent = CreateDefaultSubobject<USkeletalMeshComponent>(TEXT("BodyMesh"));
	BodyMeshComponent->SetupAttachment(CharacterRoot);
	BodyMeshComponent->SetRelativeLocation(BodyPreviewBaseLocation);
	BodyMeshComponent->SetRelativeRotation(BodyPreviewBaseRotation);
	ConfigureMeshComponent(BodyMeshComponent);

	HairMeshComponent = CreateCosmeticComponent(TEXT("HairMesh"));
	FaceMeshComponent = CreateCosmeticComponent(TEXT("FaceMesh"));
	HatMeshComponent = CreateCosmeticComponent(TEXT("HatMesh"));
	MustacheMeshComponent = CreateCosmeticComponent(TEXT("MustacheMesh"));
	OutfitMeshComponent = CreateCosmeticComponent(TEXT("OutfitMesh"));
	OutwearMeshComponent = CreateCosmeticComponent(TEXT("OutwearMesh"));
	PantsMeshComponent = CreateCosmeticComponent(TEXT("PantsMesh"));
	ShoesMeshComponent = CreateCosmeticComponent(TEXT("ShoesMesh"));
	AccessoryMeshComponent = CreateCosmeticComponent(TEXT("AccessoryMesh"));

	SceneCaptureComponent = CreateDefaultSubobject<USceneCaptureComponent2D>(TEXT("SceneCapture"));
	SceneCaptureComponent->SetupAttachment(PreviewRoot);
	SceneCaptureComponent->SetRelativeLocation(FVector(335.0f, 0.0f, 58.0f));
	SceneCaptureComponent->SetRelativeRotation(FRotator(0.0f, 180.0f, 0.0f));
	SceneCaptureComponent->FOVAngle = 34.0f;
	SceneCaptureComponent->CaptureSource = ESceneCaptureSource::SCS_FinalColorLDR;
	SceneCaptureComponent->bCaptureEveryFrame = true;
	SceneCaptureComponent->bCaptureOnMovement = false;
	SceneCaptureComponent->bAlwaysPersistRenderingState = true;
	SceneCaptureComponent->PostProcessSettings.bOverride_AutoExposureBias = true;
	SceneCaptureComponent->PostProcessSettings.AutoExposureBias = 0.0f;
	SceneCaptureComponent->ShowFlags.SetEyeAdaptation(false);
	SceneCaptureComponent->ShowFlags.SetBloom(false);
	SceneCaptureComponent->ShowFlags.SetMotionBlur(false);

	UPointLightComponent* KeyLight = CreateDefaultSubobject<UPointLightComponent>(TEXT("PreviewKeyLight"));
	KeyLight->SetupAttachment(PreviewRoot);
	KeyLight->SetRelativeLocation(FVector(240.0f, -220.0f, 240.0f));
	KeyLight->SetIntensity(185000.0f);
	KeyLight->SetAttenuationRadius(1400.0f);

	UPointLightComponent* FillLight = CreateDefaultSubobject<UPointLightComponent>(TEXT("PreviewFillLight"));
	FillLight->SetupAttachment(PreviewRoot);
	FillLight->SetRelativeLocation(FVector(120.0f, 260.0f, 130.0f));
	FillLight->SetIntensity(85000.0f);
	FillLight->SetAttenuationRadius(1200.0f);

	UDirectionalLightComponent* RimLight = CreateDefaultSubobject<UDirectionalLightComponent>(TEXT("PreviewRimLight"));
	RimLight->SetupAttachment(PreviewRoot);
	RimLight->SetRelativeRotation(FRotator(-28.0f, -145.0f, 0.0f));
	RimLight->SetIntensity(2.0f);
}

void AVNHCustomizationPreviewActor::BeginPlay()
{
	Super::BeginPlay();

	EnsureRenderTarget();
}

void AVNHCustomizationPreviewActor::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (SceneCaptureComponent)
	{
		SceneCaptureComponent->TextureTarget = nullptr;
	}
	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearTimer(ReturnToIdleTimerHandle);
	}
	RenderTarget = nullptr;
	Super::EndPlay(EndPlayReason);
}

void AVNHCustomizationPreviewActor::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	if (bAutoRotate)
	{
		AddYawInput(DeltaSeconds * 10.0f);
	}

	UpdateProceduralChangeAnimation(DeltaSeconds);
}

void AVNHCustomizationPreviewActor::ApplyCustomization(const FVNHCharacterCustomization& Customization)
{
	EnsureRenderTarget();

	ApplySlotMesh(BodyMeshComponent, Customization.BodyMesh.IsNull()
		? TSoftObjectPtr<USkeletalMesh>(FSoftObjectPath(DefaultBodyMeshPath))
		: Customization.BodyMesh);
	ApplySlotMesh(HairMeshComponent, Customization.HairMesh);
	ApplySlotMesh(FaceMeshComponent, Customization.FaceMesh, Customization.bNoFace);
	ApplySlotMesh(HatMeshComponent, Customization.HatMesh);
	ApplySlotMesh(MustacheMeshComponent, Customization.MustacheMesh);
	ApplySlotMesh(OutfitMeshComponent, Customization.OutfitMesh);
	ApplySlotMesh(OutwearMeshComponent, Customization.OutwearMesh);
	ApplySlotMesh(PantsMeshComponent, Customization.PantsMesh);
	ApplySlotMesh(ShoesMeshComponent, Customization.ShoesMesh);
	ApplySlotMesh(AccessoryMeshComponent, Customization.AccessoryMesh);

	ApplyColorToMesh(BodyMeshComponent, Customization.BodyColor);
	ApplyColorToMesh(HairMeshComponent, Customization.HairColor);
	ApplyColorToMesh(OutfitMeshComponent, Customization.OutfitColor);
	ApplyColorToMesh(OutwearMeshComponent, Customization.OutfitColor);
	PlayIdleAnimation();

	if (SceneCaptureComponent)
	{
		SceneCaptureComponent->CaptureScene();
	}
}

void AVNHCustomizationPreviewActor::AddYawInput(float DeltaYaw)
{
	PreviewYaw = FMath::UnwindDegrees(PreviewYaw + DeltaYaw);
	if (CharacterRoot)
	{
		CharacterRoot->SetRelativeRotation(FRotator(0.0f, PreviewYaw, 0.0f));
	}
	if (SceneCaptureComponent)
	{
		SceneCaptureComponent->CaptureScene();
	}
}

void AVNHCustomizationPreviewActor::SetUseFemalePreviewAnimations(bool bInUseFemalePreviewAnimations)
{
	if (bUseFemalePreviewAnimations == bInUseFemalePreviewAnimations)
	{
		return;
	}

	bUseFemalePreviewAnimations = bInUseFemalePreviewAnimations;
	PlayIdleAnimation();
}

void AVNHCustomizationPreviewActor::PlaySlotChangeAnimation(EVNHCustomizationSlot CustomizationSlot, bool bRemovingItem)
{
	UAnimationAsset* ChangeAnimation = nullptr;
	switch (CustomizationSlot)
	{
	case EVNHCustomizationSlot::Hat:
		ChangeAnimation = bRemovingItem
			? LoadPreferredAnimation(MaleHatTakeOffAnimPath, FemaleHatTakeOffAnimPath)
			: LoadPreferredAnimation(MaleHatPutOnAnimPath, FemaleHatPutOnAnimPath);
		break;
	case EVNHCustomizationSlot::Face:
	case EVNHCustomizationSlot::Mustache:
		ChangeAnimation = bRemovingItem
			? LoadPreferredAnimation(MaleMaskTakeOffAnimPath, FemaleMaskTakeOffAnimPath)
			: LoadPreferredAnimation(MaleMaskPutOnAnimPath, FemaleMaskPutOnAnimPath);
		break;
	case EVNHCustomizationSlot::Outfit:
	case EVNHCustomizationSlot::Outwear:
	case EVNHCustomizationSlot::Body:
		ChangeAnimation = bRemovingItem
			? LoadPreferredAnimation(MaleTorsoTakeOffAnimPath, FemaleTorsoTakeOffAnimPath)
			: LoadPreferredAnimation(MaleTorsoPutOnAnimPath, FemaleTorsoPutOnAnimPath);
		break;
	case EVNHCustomizationSlot::Pants:
		ChangeAnimation = bRemovingItem
			? LoadPreferredAnimation(nullptr, FemalePantsTakeOffAnimPath)
			: LoadPreferredAnimation(MalePantsPutOnAnimPath, FemalePantsPutOnAnimPath);
		break;
	case EVNHCustomizationSlot::Shoes:
		ChangeAnimation = bRemovingItem ? nullptr : LoadPreferredAnimation(MaleShoesPutOnAnimPath, FemaleShoesPutOnAnimPath);
		break;
	case EVNHCustomizationSlot::Accessory:
	case EVNHCustomizationSlot::Hair:
		ChangeAnimation = bRemovingItem
			? LoadPreferredAnimation(MaleGlassesTakeOffAnimPath, FemaleGlassesTakeOffAnimPath)
			: LoadPreferredAnimation(MaleGlassesPutOnAnimPath, FemaleGlassesPutOnAnimPath);
		break;
	default:
		break;
	}

	if (!ChangeAnimation)
	{
		StartProceduralChangeAnimation(CustomizationSlot, bRemovingItem);
		return;
	}

	ResetBodyPreviewPose();
	PlayAnimationAsset(ChangeAnimation, false);
	if (UWorld* World = GetWorld())
	{
		const float ReturnDelay = FMath::Max(ChangeAnimation->GetPlayLength(), 0.5f);
		World->GetTimerManager().ClearTimer(ReturnToIdleTimerHandle);
		World->GetTimerManager().SetTimer(ReturnToIdleTimerHandle, this, &AVNHCustomizationPreviewActor::PlayIdleAnimation, ReturnDelay, false);
	}
}

UTextureRenderTarget2D* AVNHCustomizationPreviewActor::GetOrCreateRenderTarget()
{
	EnsureRenderTarget();
	return RenderTarget;
}

void AVNHCustomizationPreviewActor::EnsureRenderTarget()
{
	if (RenderTarget)
	{
		if (SceneCaptureComponent && SceneCaptureComponent->TextureTarget != RenderTarget)
		{
			SceneCaptureComponent->TextureTarget = RenderTarget;
		}
		return;
	}

	RenderTarget = NewObject<UTextureRenderTarget2D>(this, TEXT("CustomizerPreviewRT"));
	if (RenderTarget)
	{
		RenderTarget->ClearColor = FLinearColor(0.005f, 0.018f, 0.024f, 1.0f);
		RenderTarget->InitAutoFormat(PreviewRenderTargetWidth, PreviewRenderTargetHeight);
		RenderTarget->UpdateResourceImmediate(true);
		if (SceneCaptureComponent)
		{
			SceneCaptureComponent->TextureTarget = RenderTarget;
			SceneCaptureComponent->CaptureScene();
		}
	}
}

USkeletalMeshComponent* AVNHCustomizationPreviewActor::CreateCosmeticComponent(const TCHAR* ComponentName)
{
	USkeletalMeshComponent* NewComponent = CreateDefaultSubobject<USkeletalMeshComponent>(ComponentName);
	NewComponent->SetupAttachment(BodyMeshComponent);
	ConfigureMeshComponent(NewComponent);
	return NewComponent;
}

void AVNHCustomizationPreviewActor::ConfigureMeshComponent(USkeletalMeshComponent* MeshComponent)
{
	if (!MeshComponent)
	{
		return;
	}

	MeshComponent->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	MeshComponent->SetGenerateOverlapEvents(false);
	MeshComponent->VisibilityBasedAnimTickOption = EVisibilityBasedAnimTickOption::AlwaysTickPoseAndRefreshBones;
	MeshComponent->bEnableUpdateRateOptimizations = false;
	MeshComponent->bPauseAnims = false;
	MeshComponent->GlobalAnimRateScale = 1.0f;
	MeshComponent->SetCastShadow(true);

	if (MeshComponent != BodyMeshComponent)
	{
		MeshComponent->SetLeaderPoseComponent(BodyMeshComponent);
	}
}

void AVNHCustomizationPreviewActor::ApplySlotMesh(USkeletalMeshComponent* SlotComponent, const TSoftObjectPtr<USkeletalMesh>& MeshAsset, bool bHideSlot)
{
	if (!SlotComponent)
	{
		return;
	}

	USkeletalMesh* LoadedMesh = MeshAsset.IsNull() ? nullptr : MeshAsset.LoadSynchronous();
	SlotComponent->SetSkeletalMesh(LoadedMesh);
	SlotComponent->SetHiddenInGame(bHideSlot || LoadedMesh == nullptr);
	SlotComponent->SetVisibility(!bHideSlot && LoadedMesh != nullptr, true);
	if (LoadedMesh && SlotComponent != BodyMeshComponent)
	{
		SlotComponent->SetLeaderPoseComponent(BodyMeshComponent);
	}
}

void AVNHCustomizationPreviewActor::ApplyColorToMesh(USkeletalMeshComponent* MeshComponent, const FLinearColor& Color)
{
	if (!MeshComponent || !MeshComponent->GetSkeletalMeshAsset())
	{
		return;
	}

	for (int32 MaterialIndex = 0; MaterialIndex < MeshComponent->GetNumMaterials(); ++MaterialIndex)
	{
		if (UMaterialInstanceDynamic* DynamicMaterial = MeshComponent->CreateAndSetMaterialInstanceDynamic(MaterialIndex))
		{
			DynamicMaterial->SetVectorParameterValue(TEXT("Color"), Color);
			DynamicMaterial->SetVectorParameterValue(TEXT("BaseColor"), Color);
			DynamicMaterial->SetVectorParameterValue(TEXT("Tint"), Color);
		}
	}
}

UAnimationAsset* AVNHCustomizationPreviewActor::LoadCompatibleAnimation(const TArray<FString>& CandidatePaths) const
{
	const USkeletalMesh* BodyMesh = BodyMeshComponent ? BodyMeshComponent->GetSkeletalMeshAsset() : nullptr;
	if (!BodyMesh || !BodyMesh->GetSkeleton())
	{
		UE_LOG(LogVNH, Warning, TEXT("CustomizerPreviewAnim: cannot load preview animation because body mesh or body skeleton is missing."));
		return nullptr;
	}

	for (const FString& CandidatePath : CandidatePaths)
	{
		if (CandidatePath.IsEmpty())
		{
			continue;
		}

		UAnimationAsset* AnimationAsset = LoadObject<UAnimationAsset>(nullptr, *CandidatePath);
		if (AnimationAsset && AnimationAsset->GetSkeleton() == BodyMesh->GetSkeleton())
		{
			UE_LOG(
				LogVNH,
				Display,
				TEXT("CustomizerPreviewAnim: using %s on body %s skeleton %s."),
				*AnimationAsset->GetPathName(),
				*BodyMesh->GetPathName(),
				*BodyMesh->GetSkeleton()->GetPathName());
			return AnimationAsset;
		}
		if (AnimationAsset)
		{
			UE_LOG(
				LogVNH,
				Warning,
				TEXT("CustomizerPreviewAnim: rejected %s skeleton %s for body %s skeleton %s."),
				*AnimationAsset->GetPathName(),
				AnimationAsset->GetSkeleton() ? *AnimationAsset->GetSkeleton()->GetPathName() : TEXT("None"),
				*BodyMesh->GetPathName(),
				*BodyMesh->GetSkeleton()->GetPathName());
		}
		else
		{
			UE_LOG(LogVNH, Warning, TEXT("CustomizerPreviewAnim: failed to load candidate animation %s."), *CandidatePath);
		}
	}

	return nullptr;
}

UAnimationAsset* AVNHCustomizationPreviewActor::LoadPreferredAnimation(const TCHAR* MalePath, const TCHAR* FemalePath) const
{
	return bUseFemalePreviewAnimations
		? LoadCompatibleAnimation({FemalePath ? FString(FemalePath) : FString(), MalePath ? FString(MalePath) : FString()})
		: LoadCompatibleAnimation({MalePath ? FString(MalePath) : FString(), FemalePath ? FString(FemalePath) : FString()});
}

UAnimationAsset* AVNHCustomizationPreviewActor::LoadPreferredAnimation(
	const TCHAR* MalePath,
	const TCHAR* FemalePath,
	const TCHAR* MaleFallbackPath,
	const TCHAR* FemaleFallbackPath) const
{
	return bUseFemalePreviewAnimations
		? LoadCompatibleAnimation({
			FemalePath ? FString(FemalePath) : FString(),
			FemaleFallbackPath ? FString(FemaleFallbackPath) : FString(),
			MalePath ? FString(MalePath) : FString(),
			MaleFallbackPath ? FString(MaleFallbackPath) : FString()})
		: LoadCompatibleAnimation({
			MalePath ? FString(MalePath) : FString(),
			MaleFallbackPath ? FString(MaleFallbackPath) : FString(),
			FemalePath ? FString(FemalePath) : FString(),
			FemaleFallbackPath ? FString(FemaleFallbackPath) : FString()});
}

void AVNHCustomizationPreviewActor::PlayAnimationAsset(UAnimationAsset* AnimationAsset, bool bLooping)
{
	if (!BodyMeshComponent || !AnimationAsset)
	{
		return;
	}

	BodyMeshComponent->SetAnimationMode(EAnimationMode::AnimationSingleNode);
	BodyMeshComponent->SetAnimation(AnimationAsset);
	BodyMeshComponent->Play(bLooping);
	if (SceneCaptureComponent)
	{
		SceneCaptureComponent->CaptureScene();
	}
}

void AVNHCustomizationPreviewActor::PlayIdleAnimation()
{
	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearTimer(ReturnToIdleTimerHandle);
	}

	if (ProceduralChangeDuration <= 0.0f)
	{
		ResetBodyPreviewPose();
	}

	if (UAnimationAsset* TryingOnIdle = LoadPreferredAnimation(MaleIdleAnimPath, FemaleIdleAnimPath, CreativeIdleAnimPath, CreativeIdleAnimPath))
	{
		PlayAnimationAsset(TryingOnIdle, true);
		return;
	}

	if (UClass* CreativeAnimClass = LoadClass<UAnimInstance>(nullptr, CreativeAnimClassPath))
	{
		BodyMeshComponent->SetAnimationMode(EAnimationMode::AnimationBlueprint);
		BodyMeshComponent->SetAnimInstanceClass(CreativeAnimClass);
	}
	else if (UAnimationAsset* FallbackIdle = LoadObject<UAnimationAsset>(nullptr, CreativeIdleAnimPath))
	{
		PlayAnimationAsset(FallbackIdle, true);
	}
}

void AVNHCustomizationPreviewActor::StartProceduralChangeAnimation(EVNHCustomizationSlot CustomizationSlot, bool bRemovingItem)
{
	ProceduralChangeSlot = CustomizationSlot;
	bProceduralChangeRemovingItem = bRemovingItem;
	ProceduralChangeElapsed = 0.0f;
	ProceduralChangeDuration = 0.72f;
	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearTimer(ReturnToIdleTimerHandle);
	}
	if (SceneCaptureComponent)
	{
		SceneCaptureComponent->CaptureScene();
	}
}

void AVNHCustomizationPreviewActor::UpdateProceduralChangeAnimation(float DeltaSeconds)
{
	if (ProceduralChangeDuration <= 0.0f || !BodyMeshComponent)
	{
		return;
	}

	ProceduralChangeElapsed += DeltaSeconds;
	const float Alpha = FMath::Clamp(ProceduralChangeElapsed / ProceduralChangeDuration, 0.0f, 1.0f);
	const float Wave = FMath::Sin(Alpha * UE_PI);
	const float Snap = FMath::Sin(Alpha * UE_PI * 2.0f);

	FVector LocationOffset = FVector::ZeroVector;
	FRotator RotationOffset = FRotator::ZeroRotator;
	FVector Scale = BodyPreviewBaseScale;

	switch (ProceduralChangeSlot)
	{
	case EVNHCustomizationSlot::Hat:
	case EVNHCustomizationSlot::Hair:
	case EVNHCustomizationSlot::Face:
	case EVNHCustomizationSlot::Mustache:
	case EVNHCustomizationSlot::Accessory:
		LocationOffset.Z = Wave * 5.0f;
		RotationOffset.Pitch = (bProceduralChangeRemovingItem ? -1.0f : 1.0f) * Wave * 4.0f;
		RotationOffset.Roll = Snap * 2.5f;
		break;
	case EVNHCustomizationSlot::Pants:
	case EVNHCustomizationSlot::Shoes:
		LocationOffset.Z = Wave * 8.0f;
		RotationOffset.Roll = Snap * 3.0f;
		break;
	case EVNHCustomizationSlot::Body:
	case EVNHCustomizationSlot::Outfit:
	case EVNHCustomizationSlot::Outwear:
	default:
		LocationOffset.Z = Wave * 7.0f;
		RotationOffset.Pitch = (bProceduralChangeRemovingItem ? 1.0f : -1.0f) * Wave * 5.5f;
		RotationOffset.Roll = Snap * 3.5f;
		Scale = BodyPreviewBaseScale * (1.0f + Wave * 0.025f);
		break;
	}

	BodyMeshComponent->SetRelativeLocation(BodyPreviewBaseLocation + LocationOffset);
	BodyMeshComponent->SetRelativeRotation(BodyPreviewBaseRotation + RotationOffset);
	BodyMeshComponent->SetRelativeScale3D(Scale);

	if (Alpha >= 1.0f)
	{
		ProceduralChangeDuration = 0.0f;
		ProceduralChangeElapsed = 0.0f;
		ResetBodyPreviewPose();
		PlayIdleAnimation();
	}
}

void AVNHCustomizationPreviewActor::ResetBodyPreviewPose()
{
	if (!BodyMeshComponent)
	{
		return;
	}

	BodyMeshComponent->SetRelativeLocation(BodyPreviewBaseLocation);
	BodyMeshComponent->SetRelativeRotation(BodyPreviewBaseRotation);
	BodyMeshComponent->SetRelativeScale3D(BodyPreviewBaseScale);
}
