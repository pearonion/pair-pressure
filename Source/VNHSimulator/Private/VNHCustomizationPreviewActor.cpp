#include "VNHCustomizationPreviewActor.h"

#include "Animation/AnimInstance.h"
#include "Animation/AnimationAsset.h"
#include "Components/DirectionalLightComponent.h"
#include "Components/PointLightComponent.h"
#include "Components/SceneCaptureComponent2D.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Materials/MaterialInstanceDynamic.h"

namespace
{
const TCHAR* DefaultBodyMeshPath = TEXT("/Game/Creative_Characters/Skeleton_Meshes/SK_Body_009.SK_Body_009");
const TCHAR* CreativeAnimClassPath = TEXT("/Game/Creative_Characters/Animations/ABP_CreativeCharacter.ABP_CreativeCharacter_C");
const TCHAR* CreativeIdleAnimPath = TEXT("/Game/TNG/Characters/Animations/ANIM_TNG_Idle_Breathing.ANIM_TNG_Idle_Breathing");
constexpr int32 PreviewRenderTargetWidth = 768;
constexpr int32 PreviewRenderTargetHeight = 1184;
}

AVNHCustomizationPreviewActor::AVNHCustomizationPreviewActor()
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.TickInterval = 0.0f;
	bReplicates = false;
	SetCanBeDamaged(false);

	PreviewRoot = CreateDefaultSubobject<USceneComponent>(TEXT("PreviewRoot"));
	SetRootComponent(PreviewRoot);

	BodyMeshComponent = CreateDefaultSubobject<USkeletalMeshComponent>(TEXT("BodyMesh"));
	BodyMeshComponent->SetupAttachment(PreviewRoot);
	BodyMeshComponent->SetRelativeLocation(FVector(0.0f, 0.0f, -54.0f));
	BodyMeshComponent->SetRelativeRotation(FRotator(0.0f, -90.0f, 0.0f));
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
	SceneCaptureComponent->bCaptureEveryFrame = false;
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

	if (SceneCaptureComponent)
	{
		SceneCaptureComponent->CaptureScene();
	}
}

void AVNHCustomizationPreviewActor::AddYawInput(float DeltaYaw)
{
	PreviewYaw = FMath::UnwindDegrees(PreviewYaw + DeltaYaw);
	PreviewRoot->SetRelativeRotation(FRotator(0.0f, PreviewYaw, 0.0f));
	if (SceneCaptureComponent)
	{
		SceneCaptureComponent->CaptureScene();
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

	if (UClass* CreativeAnimClass = LoadClass<UAnimInstance>(nullptr, CreativeAnimClassPath))
	{
		MeshComponent->SetAnimationMode(EAnimationMode::AnimationBlueprint);
		MeshComponent->SetAnimInstanceClass(CreativeAnimClass);
	}
	else if (UAnimationAsset* IdleAnimation = LoadObject<UAnimationAsset>(nullptr, CreativeIdleAnimPath))
	{
		MeshComponent->SetAnimationMode(EAnimationMode::AnimationSingleNode);
		MeshComponent->SetAnimation(IdleAnimation);
		MeshComponent->Play(true);
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
