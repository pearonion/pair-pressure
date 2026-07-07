// Copyright Dev.GaeMyo 2024. All Rights Reserved.

#include "EUActors/GmEUA_ThumbnailCreator.h"

#include <UObject/ConstructorHelpers.h>

#include "DesktopPlatformModule.h"
#include "GroomComponent.h"
#include "Engine/Texture2D.h"
#include "IDesktopPlatform.h"
#include "ImageUtils.h"
#include "NiagaraComponent.h"
#include "Components/DirectionalLightComponent.h"
#include "Components/PointLightComponent.h"
#include "Components/SceneCaptureComponent2D.h"
#include "Engine/TextureRenderTarget2D.h"
#include "EUActors/GmEUW_ThumbnailCreator.h"
#include "GameFramework/SpringArmComponent.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Subsystems/EditorAssetSubsystem.h"
#include "Containers/UnrealString.h"
#include "Kismet/KismetMaterialLibrary.h"
#include "Misc/FileHelper.h"
#include "Misc/App.h"
#include "Misc/Paths.h"
#include "Serialization/BufferArchive.h"

#if WITH_EDITOR
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetToolsModule.h"
#include "IAssetTools.h"
#include "IContentBrowserSingleton.h"
#include "PackageTools.h"
#endif

#define LOCTEXT_NAMESPACE "GmThumbnailCreator"

AGmEUA_ThumbnailCreator::AGmEUA_ThumbnailCreator(const FObjectInitializer& ObjectInitializer)
	:Super(ObjectInitializer)
{
	struct FConstructorStatics
	{
		ConstructorHelpers::FObjectFinder<UTextureRenderTarget2D> TRT2D_EXR;
		ConstructorHelpers::FObjectFinder<UTextureRenderTarget2D> TRT2D_PNG;
		ConstructorHelpers::FObjectFinder<UMaterial> OutlineMat_Inside;
		ConstructorHelpers::FObjectFinder<UMaterial> OutlineMat_Outside;
		ConstructorHelpers::FObjectFinder<UMaterial> PPM_BG;
		ConstructorHelpers::FObjectFinder<UMaterial> PPM_AlphaPath;

		FConstructorStatics()
			:
		TRT2D_EXR(TEXT("/Script/Engine.TextureRenderTarget2D'/GmRapidThumbnailCreator/Materials/RT/RT_ThumbnailCreatorEXR'")),
		TRT2D_PNG(TEXT("/Script/Engine.TextureRenderTarget2D'/GmRapidThumbnailCreator/Materials/RT/RT_ThumbnailCreatorPNG'")),
		OutlineMat_Inside(TEXT("/Script/Engine.Material'/GmRapidThumbnailCreator/Materials/PPM_OutlineInside'")),
		OutlineMat_Outside(TEXT("/Script/Engine.Material'/GmRapidThumbnailCreator/Materials/PPM_OutlineOutside'")),
		PPM_BG(TEXT("/Script/Engine.Material'/GmRapidThumbnailCreator/Materials/PPM_Background'")),
		PPM_AlphaPath(TEXT("/Script/Engine.Material'/GmRapidThumbnailCreator/Materials/PPM_Alpha'"))
		{}
	};

	static FConstructorStatics GmS_Helper;

	if (!PPM_InsideOutline)
	{
		PPM_InsideOutline = GmS_Helper.OutlineMat_Inside.Object;
	}
	if (!PPM_OutsideOutline)
	{
		PPM_OutsideOutline = GmS_Helper.OutlineMat_Outside.Object;
	}
	if (!PPM_Background)
	{
		PPM_Background = GmS_Helper.PPM_BG.Object;
	}
	if (!PPM_Alpha)
	{
		PPM_Alpha = GmS_Helper.PPM_AlphaPath.Object;
	}
	if (!RenderTarget2D_EXR)
	{
		RenderTarget2D_EXR = GmS_Helper.TRT2D_EXR.Object;
	}
	if (!RenderTarget2D_PNG)
	{
		RenderTarget2D_PNG = GmS_Helper.TRT2D_PNG.Object;
	}

	bAllowTickBeforeBeginPlay = true;
	SetTickableWhenPaused(true);
	
	SceneCapture2DComponent = CreateDefaultSubobject<USceneCaptureComponent2D>(TEXT("SceneCaptureComponent2D"));
	SetRootComponent(SceneCapture2DComponent);
	SceneCapture2DComponent->FOVAngle = 40.f;
	SceneCapture2DComponent->OrthoWidth = 256.f;
	SceneCapture2DComponent->TextureTarget = GmS_Helper.TRT2D_EXR.Object;
	SceneCapture2DComponent->CaptureSource = SCS_FinalColorLDR;
	SceneCapture2DComponent->bAlwaysPersistRenderingState = true;
	SceneCapture2DComponent->bUseRayTracingIfEnabled = true;
	// SceneCapture2DComponent->PostProcessSettings.blur
	
	SpringArmComp = CreateDefaultSubobject<USpringArmComponent>(TEXT("SpringArmComp"));
	SpringArmComp->SetupAttachment(SceneCapture2DComponent);
	SpringArmComp->TargetArmLength = 0;
	SpringArmComp->bDoCollisionTest = false;

	MeshesLoc = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("MeshesLoc"));
	MeshesLoc->SetupAttachment(SpringArmComp);
	MeshesLoc->SetVisibility(false);
	MeshesLoc->SetHiddenInGame(true);
	MeshesLoc->bVisibleInRayTracing = false;
	MeshesLoc->bVisibleInReflectionCaptures = false;
	MeshesLoc->bVisibleInRealTimeSkyCaptures = false;
	MeshesLoc->SetHiddenInSceneCapture(true);

	// Niagara component
	NiagaraComponent = CreateDefaultSubobject<UNiagaraComponent>(TEXT("NiagaraComponent"));
	NiagaraComponent->SetupAttachment(MeshesLoc);

	Distant_SpringArm = CreateDefaultSubobject<USpringArmComponent>(TEXT("Distant_Spring"));
	Distant_SpringArm->SetupAttachment(MeshesLoc);
	Distant_SpringArm->TargetArmLength = 150.f;
	Distant_SpringArm->bDoCollisionTest = false;
	Distant_SpringArm->SetRelativeRotation(FRotator(0, -45, 0));
	
	NearBy_SpringArm = CreateDefaultSubobject<USpringArmComponent>(TEXT("NearBy_SpringArm"));
	NearBy_SpringArm->SetupAttachment(MeshesLoc);
	NearBy_SpringArm->TargetArmLength = 75.f;
	NearBy_SpringArm->bDoCollisionTest = false;
	NearBy_SpringArm->SetRelativeRotation(FRotator(0, 45, 0));
	
	MeshesRot = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("MeshesRot"));
	MeshesRot->SetupAttachment(MeshesLoc);
	MeshesRot->SetVisibility(false);
	MeshesRot->SetHiddenInGame(true);
	MeshesRot->bVisibleInReflectionCaptures = false;
	MeshesRot->bVisibleInRealTimeSkyCaptures = false;
	MeshesRot->bVisibleInRayTracing = false;
	MeshesRot->SetHiddenInSceneCapture(true);

	Distant_PointLight = CreateDefaultSubobject<UPointLightComponent>(TEXT("Distant_PointLight"));
	Distant_PointLight->SetupAttachment(Distant_SpringArm);
	Distant_PointLight->SetVisibility(false);
	Distant_PointLight->SetIntensity(10000.f);
	Distant_DirectionalLight = CreateDefaultSubobject<UDirectionalLightComponent>(TEXT("Distant_DirectionalLight"));
	Distant_DirectionalLight->SetupAttachment(Distant_SpringArm);
	Distant_DirectionalLight->SetVisibility(false);
	Distant_DirectionalLight->SetIntensity(15.f);
	Distant_DirectionalLight->ForwardShadingPriority = 3;

	NearBy_PointLight = CreateDefaultSubobject<UPointLightComponent>(TEXT("NearBy_PointLight"));
	NearBy_PointLight->SetupAttachment(NearBy_SpringArm);
	NearBy_PointLight->SetVisibility(false);
	NearBy_PointLight->SetIntensity(1000.f);
	NearBy_DirectionalLight = CreateDefaultSubobject<UDirectionalLightComponent>(TEXT("NearBy_DirectionalLight"));
	NearBy_DirectionalLight->SetupAttachment(NearBy_SpringArm);
	NearBy_DirectionalLight->SetVisibility(false);
	NearBy_DirectionalLight->SetIntensity(15.f);
	NearBy_DirectionalLight->ForwardShadingPriority = 2;

	SkeletalMeshComp = CreateDefaultSubobject<USkeletalMeshComponent>(TEXT("SkeletalMeshComp"));
	SkeletalMeshComp->SetupAttachment(MeshesRot);
	SkeletalMeshComp->SetAnimationMode(EAnimationMode::AnimationSingleNode);
	SkeletalMeshComp->SetRenderCustomDepth(true);
	
	Center_SpringArm = CreateDefaultSubobject<USpringArmComponent>(TEXT("Center_SpringArm"));
	Center_SpringArm->SetupAttachment(MeshesRot);
	Center_SpringArm->TargetArmLength = 51.f;
	Center_SpringArm->bDoCollisionTest = false;
	Center_SpringArm->SetRelativeRotation(FRotator(0, -90, 0));
	
	Center_DirectionalLight = CreateDefaultSubobject<UDirectionalLightComponent>(TEXT("Center_DirectionalLight"));
	Center_DirectionalLight->SetupAttachment(Center_SpringArm);
	Center_DirectionalLight->SetVisibility(false);
	Center_DirectionalLight->SetIntensity(15.f);
	Center_DirectionalLight->ForwardShadingPriority = 1;
	Center_PointLight = CreateDefaultSubobject<UPointLightComponent>(TEXT("Center_PointLight"));
	Center_PointLight->SetupAttachment(Center_SpringArm);
	Center_PointLight->SetVisibility(false);
	Center_PointLight->SetIntensity(1000.f);
	
	StaticMeshComp = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("StaticMeshComp"));
	StaticMeshComp->SetupAttachment(MeshesRot);
	StaticMeshComp->SetRenderCustomDepth(true);
	StaticMeshComp->bIgnoreInstanceForTextureStreaming = true;
}

void AGmEUA_ThumbnailCreator::GiveLife()
{
	Run();

	ChangeRenderSize();
	UpdateAllMeshes();
	UpdateEditorUtilityWidget();
}

#if WITH_EDITOR
void AGmEUA_ThumbnailCreator::SaveStaticTexture()
{
	const bool DoesDirExist{GEditor->GetEditorSubsystem<UEditorAssetSubsystem>()->DoesDirectoryExist(ExportPath)};
	
	FString ResultPath{FString((DoesDirExist ? ExportPath :
		FString("/Game/GmRapidThumbnailCreator/")) + OutputFileName)};

	if (ExportType != EGmExportType::EExportTypeEXR)
	{
		SceneCapture2DComponent->TextureTarget = RenderTarget2D_PNG;
		SceneCapture2DComponent->CaptureScene();
	}
	
	UTexture2D* ResultTexture{nullptr};

	// Render target create static texture editor only
	FString Name, PackageName;
	
	IAssetTools& AssetTools{FModuleManager::Get().LoadModuleChecked<FAssetToolsModule>("AssetTools").Get()};

	//Use asset name only if directories are specified, otherwise full path
	if (!ResultPath.Contains(L"/"))
	{
		AssetTools.CreateUniqueAssetName(
			FPackageName::GetLongPackagePath(
				UPackageTools::SanitizePackageName(SceneCapture2DComponent->TextureTarget->GetOutermost()->GetName()))
				+ L"/", ResultPath, PackageName, Name);
	}
	else
	{
		ResultPath.RemoveFromStart(L"/");
		ResultPath.RemoveFromStart(L"Content/");
		ResultPath.StartsWith(L"Game/") ? ResultPath.InsertAt(0, L"/") :
		ResultPath.InsertAt(0, L"/Game/");
		AssetTools.CreateUniqueAssetName(ResultPath, L"", PackageName,
			Name);
	}
	
	// create a static 2d texture
	UObject* NewObj{SceneCapture2DComponent->TextureTarget->ConstructTexture2D(CreatePackage(*PackageName),
		Name, SceneCapture2DComponent->TextureTarget->GetMaskedFlags() | RF_Public | RF_Standalone,
		CTF_Default | CTF_AllowMips, nullptr)};

	if (UTexture2D* NewTex{Cast<UTexture2D>(NewObj)})
	{
		// package needs saving
		NewObj->MarkPackageDirty();

		// Notify the asset registry
		FAssetRegistryModule::AssetCreated(NewObj);

		// Update Compression and Mip settings
		NewTex->CompressionSettings = ExportType == EGmExportType::EExportTypeEXR
			? TC_HDR_F32
			: TC_EditorIcon;
		NewTex->MipGenSettings = TMGS_NoMipmaps;
		NewTex->SRGB = ExportType != EGmExportType::EExportTypeEXR;
		NewTex->PostEditChange();

		ResultTexture = NewTex;
	}//~ End of Render target create static texture editor only

	if (ResultTexture)
	{
		if (bExportLocalDesktop)
		{
			if (ExportType == EGmExportType::EExportTypeEXR) // exr
			{
				TArray<FString> AssetsToExport_EXR;
				AssetsToExport_EXR.Add(*GetPathNameSafe(ResultTexture));
				
				FModuleManager::GetModuleChecked<FAssetToolsModule>("AssetTools").Get().ExportAssetsWithDialog(
					AssetsToExport_EXR, true);
			}
			else // png or jpg
			{
				//@TODO This function is not used because the jpg extension cannot be used.
				// UAssetToolsImpl::Get().ExportAssetsWithDialog(AssetObjsToExport, true);

				TArray<FString> OutputFileNames;
				const void* ParentWindowPtr{FSlateApplication::Get()
					.GetActiveTopLevelWindow()->GetNativeWindow()->GetOSWindowHandle()};

				/*FString ResultExtension{FString("")}, */ FString SupportedFileType{FString("")};
				switch (ExportType)
				{
				case EGmExportType::EExportTypeEXR:
					// ResultExtension = FString(".exr");
					SupportedFileType = FString("Gm Image Files(EXR))|*.exr");
					break;
				case EGmExportType::EExportTypeJPG:
					// ResultExtension = FString(".jpg");
					SupportedFileType = FString("Gm Image Files(JPG))|*.jpg");
					break;
				case EGmExportType::EExportTypePNG:
					// ResultExtension = FString(".png");
					SupportedFileType = FString("Gm Image Files(PNG))|*.png");
					break;
				}

				if (IDesktopPlatform* DesktopPlatform{FDesktopPlatformModule::Get()})
				{
					DesktopPlatform->SaveFileDialog(ParentWindowPtr, L"Save File Path", L"/",
						Name,
						SupportedFileType,
						EFileDialogFlags::None, OutputFileNames);
				}
				if (!OutputFileNames.IsEmpty())
				{
					GmExportTexture(ResultTexture, OutputFileNames[0], FPaths::GetExtension(OutputFileNames[0]));
				}
			}
		}//~End of export

		if (!bSuppressSaveConfirmation)
		{
			const FText Title{FText::FromString(DoesDirExist ? FString("Texture successfully saved.") :
				FString("Save directory folder does not exist."))};

			if (const EAppReturnType::Type Result{FMessageDialog::Open(EAppMsgType::YesNo, EAppReturnType::No,
				FText::FromString(PackageName + FString("\nOpen saved texture?")), Title)};
				Result == EAppReturnType::Yes)
			{
				GEditor->GetEditorSubsystem<UAssetEditorSubsystem>()->OpenEditorForAsset(ResultTexture);
			}
		}
	}
	SceneCapture2DComponent->TextureTarget = RenderTarget2D_EXR;
}
#endif

void AGmEUA_ThumbnailCreator::GmExportTexture(UTexture2D* InTexture, const FString& InFilename,
	const FString& InExtension) const
{
	const TextureCompressionSettings Old_CompressionSettings{InTexture->CompressionSettings};
	const TextureMipGenSettings Old_MipGenSettings{InTexture->MipGenSettings};
	const uint8 Old_SRGB{InTexture->SRGB};
	const int32 Width{/*InTexture->GetSizeX()*/TextureResWidth}, Height{/*InTexture->GetSizeY()*/TextureResHeight};
	
	InTexture->CompressionSettings = TC_VectorDisplacementmap;//TC_EditorIcon;
	InTexture->MipGenSettings = TMGS_NoMipmaps;
	InTexture->SRGB = false;
	InTexture->UpdateResource();

	const uint8* TextureData{static_cast<uint8*>(InTexture->GetPlatformData()->Mips[0].BulkData.Lock(LOCK_READ_ONLY))};
	InTexture->GetPlatformData()->Mips[0].BulkData.Unlock();
	InTexture->UpdateResource();
	
	if (!TextureData)
		return;
	
	TArray<FColor> Colors;
	for (int32 y{0}; y < Height; y++)
	{
		for (int32 x{0}; x < Width; x++)
		{
			FColor Color;
			Color.B = TextureData[(y * Width + x) * 4 + 0];
			Color.G = TextureData[(y * Width + x) * 4 + 1];
			Color.R = TextureData[(y * Width + x) * 4 + 2];
			Color.A = TextureData[(y * Width + x) * 4 + 3];
			Colors.Add(Color);
		}
	}
	
	// TArray<FColor> DstData;
	// FImageUtils::ImageResize(Width,Height,Colors,TextureResWidth,
	// 	TextureResHeight,DstData,true);
	//
	// Width = TextureResWidth;
	// Height = TextureResHeight;
	// Colors = DstData;

	// InFilename : Saved full path
	if (InExtension.Equals("png", ESearchCase::IgnoreCase))
	{
		TArray64<uint8> TextureDatas;
		FImageUtils::PNGCompressImageArray(Width, Height, Colors, TextureDatas);
		FFileHelper::SaveArrayToFile(TextureDatas, *InFilename);
	}
	else if(InExtension.Equals("jpg", ESearchCase::IgnoreCase))
	{
		TArray<uint8> TextureDatas;
		FImageUtils::ThumbnailCompressImageArray(Width, Height, Colors, TextureDatas);
		FFileHelper::SaveArrayToFile(TextureDatas, *InFilename);
	}
	// Note == is case insensitive
	else if (InExtension.Equals("hdr", ESearchCase::IgnoreCase))
	{
		FString TotalFileName{*GetPathNameSafe(InTexture)};
		if (FArchive* Ar{IFileManager::Get().CreateFileWriter(*TotalFileName)})
		{
			bool IsSuccess{false};
			FBufferArchive Bf;
			if (SceneCapture2DComponent->TextureTarget->RenderTargetFormat == RTF_RGBA16f)
			{
				IsSuccess = FImageUtils::ExportRenderTarget2DAsHDR(SceneCapture2DComponent->TextureTarget, Bf);
			}

			if (IsSuccess)
			{
				Ar->Serialize(/*const_cast<uint8*>*/(Bf.GetData()), Bf.Num());
			}

			delete Ar;
		}
	}
	else
	{
		return;
	}
	
	InTexture->MipGenSettings = Old_MipGenSettings;
	InTexture->CompressionSettings = Old_CompressionSettings;
	InTexture->SRGB = Old_SRGB;
	InTexture->UpdateResource();
}

ECameraProjectionMode::Type AGmEUA_ThumbnailCreator::GmGetSceneCapture2DProjectionType() const
{
	return SceneCapture2DComponent->ProjectionType;
}

void AGmEUA_ThumbnailCreator::GmSetSceneCapture2DProjectionType(const ECameraProjectionMode::Type InNewProjectionType) const
{
	SceneCapture2DComponent->ProjectionType = InNewProjectionType;
}

void AGmEUA_ThumbnailCreator::OnConstruction(const FTransform& Transform)
{
	Super::OnConstruction(Transform);

	UpdateCameraCaptureRender();

	StaticMeshComp->bDisallowNanite = true;

	if (!PPMID_Outline_Outside)
	{
		CreatePPMats();
	}

	// Set niagara component location offset
	const FTransform RootRelativeTransform{SceneCapture2DComponent->GetRelativeTransform()};
	
	NiagaraComponent->SetRelativeLocation(MeshesLoc->GetRelativeLocation() +
		(RootRelativeTransform.GetUnitAxis(EAxis::X) * FXDistance) + 
		(RootRelativeTransform.GetUnitAxis(EAxis::Y) * FXLocationOffset.X) +
		(RootRelativeTransform.GetUnitAxis(EAxis::Z) * FXLocationOffset.Y));
}

void AGmEUA_ThumbnailCreator::UpdateCameraCaptureRender()
{
	SceneCapture2DComponent->ShowOnlyActorComponents(DisplayObjType == EGmDisplayObjectType::EActor
	? (SpawnedActor ? SpawnedActor : this) : this, true);
}

bool AGmEUA_ThumbnailCreator::RefreshChildActor(USkeletalMeshComponent*& OutNewSKM)
{
	bool IsSuccess{false};
	
	if (SpawnedActor)
	{
		SpawnedActor->Destroy();
	}

	if (!TargetActor)
	{
		return false;
	}
	SpawnedActor = GetWorld()->SpawnActor<AActor>(TargetActor, FActorSpawnParameters());
	SpawnedActor->AttachToComponent(MeshesRot, FAttachmentTransformRules(EAttachmentRule::KeepRelative,
		true));

	TArray<USkeletalMeshComponent*> Arr_SKMs;
	SpawnedActor->GetComponents<USkeletalMeshComponent>(Arr_SKMs, true);
	for (USkeletalMeshComponent* Elem : Arr_SKMs)
	{
		Elem->Play(true);
		Elem->SetUpdateAnimationInEditor(true);
		Elem->SetRenderCustomDepth(true);
		
		if (Elem->LeaderPoseComponent.IsValid())
		{
			ActorSKM = Elem;
			OutNewSKM = ActorSKM;
			IsSuccess = true;
		}
		else if (!IsSuccess && ((Elem->GetAttachParent() && Elem->GetAttachParent()->GetClass()
			!= USkeletalMeshComponent::StaticClass()) || !Elem->GetAttachParent()))
		{
			ActorSKM = Elem;
			OutNewSKM = ActorSKM;
			IsSuccess = true;
		}
	}
	
	TArray<UStaticMeshComponent*> Arr_SMs;
	SpawnedActor->GetComponents<UStaticMeshComponent>(Arr_SMs);
	for (UStaticMeshComponent* Elem : Arr_SMs)
	{
		Elem->SetRenderCustomDepth(true);
		Elem->bDisallowNanite = true;
	}
	TArray<UGroomComponent*> Arr_Grooms;
	SpawnedActor->GetComponents<UGroomComponent>(Arr_Grooms);
	for (UGroomComponent* Elem : Arr_Grooms)
	{
		Elem->SetRenderCustomDepth(true);
	}
	
	return IsSuccess;
}

void AGmEUA_ThumbnailCreator::UpdateEditorUtilityWidget()
{
	ShowMesh();
	SkeletalMeshComp->SetSkeletalMeshAsset(SkeletalMeshAsset);

	MeshesLoc->SetRelativeLocation(FVector(0, ViewLocationOffset.X, ViewLocationOffset.Y));

	Center_SpringArm->TargetArmLength = FMath::Clamp(Center_SpringArm->TargetArmLength, CenterLightDistanceMinMax.X, CenterLightDistanceMinMax.Y);
	NearBy_SpringArm->TargetArmLength = FMath::Clamp(NearBy_SpringArm->TargetArmLength, NearByLightDistanceMinMax.X, NearByLightDistanceMinMax.Y);
	Distant_SpringArm->TargetArmLength = FMath::Clamp(Distant_SpringArm->TargetArmLength, DistantLightDistanceMinMax.X, DistantLightDistanceMinMax.Y);

	if (EditorUtilityWidgetRef)
	{
		UpdateAnim();
		EditorUtilityWidgetRef->ShowOutlineOptions(false, bEnableOutsideOutline);
		EditorUtilityWidgetRef->ShowOutlineOptions(true, bEnableInsideOutline);
		EditorUtilityWidgetRef->ShowBackgroundOptions(bEnableBackground);
	}

	UpdateOutline_Outside();
	UpdateBackground();
	UpdateAlpha();
	UpdateOutline_Inside();
	UpdatePPMBlends();
}

void AGmEUA_ThumbnailCreator::UpdateAllLights() const
{
	if (SceneCapture2DComponent->ProjectionType == ECameraProjectionMode::Orthographic)
	{
		Center_DirectionalLight->SetVisibility(Center_PointLight->IsVisible());
		NearBy_DirectionalLight->SetVisibility(NearBy_PointLight->IsVisible());
		Distant_DirectionalLight->SetVisibility(Distant_PointLight->IsVisible());
		Center_PointLight->SetVisibility(false);
		Distant_PointLight->SetVisibility(false);
		NearBy_PointLight->SetVisibility(false);
		return;
	}
	Center_PointLight->SetVisibility(Center_DirectionalLight->IsVisible());
	NearBy_PointLight->SetVisibility(NearBy_DirectionalLight->IsVisible());
	Distant_PointLight->SetVisibility(Distant_DirectionalLight->IsVisible());
	Center_DirectionalLight->SetVisibility(false);
	Distant_DirectionalLight->SetVisibility(false);
	NearBy_DirectionalLight->SetVisibility(false);
}

bool AGmEUA_ThumbnailCreator::AddRemoveAllLights(const int32 InLightIndex)
{
	if (SceneCapture2DComponent->ProjectionType == ECameraProjectionMode::Perspective)
	{
		return RefreshAllPointLight(InLightIndex);
	}
	return RefreshAllDirectionalLight(InLightIndex);
}

void AGmEUA_ThumbnailCreator::ChangeRenderSize() const
{
	// Resize
	if (RenderTarget2D_EXR)
	{
		// Resize function silently fails if either dimension isn't positive, so check for that here so we can warn the caller
		if (TextureResWidth > 0 && TextureResHeight > 0)
		{
			RenderTarget2D_EXR->ResizeTarget((uint32)TextureResWidth, (uint32)TextureResHeight);
		}
		else
		{
			FMessageLog("Blueprint").Warning(LOCTEXT("ResizeRenderTarget2D_InvalidDimensions",
				"ResizeRenderTarget2D: Dimensions must be positive."));
		}
	}
	if (RenderTarget2D_PNG)
	{
		// Resize function silently fails if either dimension isn't positive, so check for that here so we can warn the caller
		if (TextureResWidth > 0 && TextureResHeight > 0)
		{
			RenderTarget2D_PNG->ResizeTarget((uint32)TextureResWidth, (uint32)TextureResHeight);
		}
		else
		{
			FMessageLog("Blueprint").Warning(LOCTEXT("ResizeRenderTarget2D_InvalidDimensions",
				"ResizeRenderTarget2D: Dimensions must be positive."));
		}
	}

	// Update resized widget preview
	if (EditorUtilityWidgetRef)
	{
		EditorUtilityWidgetRef->ResizeRenderPreview(FVector2D(TextureResWidth, TextureResHeight));
	}

	if (PPMID_Outline_Outside)
	{
		PPMID_Outline_Outside->SetScalarParameterValue(FName("Width"), TextureResWidth);
		PPMID_Outline_Outside->SetScalarParameterValue(FName("Height"), TextureResHeight);
	}
}

void AGmEUA_ThumbnailCreator::CreatePPMats()
{
	// Don't use transient material
	const UWorld* World{GetWorld()};
	UObject* MIDOuter{World ? this : nullptr};

	if (PPM_OutsideOutline)
	{
		PPMID_Outline_Outside = UKismetMaterialLibrary::CreateDynamicMaterialInstance(MIDOuter, PPM_OutsideOutline, FName("PPMID_Outline_Outside"));
		PPMID_Outline_Outside->SetScalarParameterValue(FName("InitWidth"), TextureResWidth);
		PPMID_Outline_Outside->SetScalarParameterValue(FName("InitHeight"), TextureResHeight);
	}

	if (PPM_InsideOutline)
	{
		PPMID_Outline_Inside = UKismetMaterialLibrary::CreateDynamicMaterialInstance(MIDOuter, PPM_InsideOutline, FName("PPMID_Outline_Inside"));
	}
	if (PPM_Background)
	{
		PPMID_BG = UKismetMaterialLibrary::CreateDynamicMaterialInstance(MIDOuter, PPM_Background, FName("PPMID_BG"));
	}
	if (PPM_Alpha)
	{
		PPMID_Alpha = UKismetMaterialLibrary::CreateDynamicMaterialInstance(MIDOuter, PPM_Alpha, FName("PPMID_Alpha"));
	}
}

void AGmEUA_ThumbnailCreator::UpdatePPMBlends() const
{
	FWeightedBlendables NewWeightedBlendables;
	NewWeightedBlendables.Array.Add(FWeightedBlendable(bEnableBackground, PPMID_BG));
	NewWeightedBlendables.Array.Add(FWeightedBlendable(bEnableInsideOutline, PPMID_Outline_Inside));
	NewWeightedBlendables.Array.Add(FWeightedBlendable(bEnableOutsideOutline, PPMID_Outline_Outside));
	NewWeightedBlendables.Array.Add(FWeightedBlendable(1.0f, PPMID_Alpha));
	SceneCapture2DComponent->PostProcessSettings.WeightedBlendables = NewWeightedBlendables;

	// UE_LOG(LogTemp, Error, L"Current PPSettings is %i.", SceneCapture2DComponent->PostProcessSettings.WeightedBlendables.Array.Num());
}

void AGmEUA_ThumbnailCreator::UpdateBackground() const
{
	if (PPMID_BG)
	{
		PPMID_BG->SetTextureParameterValue(FName("BackgroundTexture"), BackgroundTexture);
		PPMID_BG->SetVectorParameterValue(FName("BackgroundColor"), BackgroundColor);
		PPMID_BG->SetScalarParameterValue(FName("UseTexture?"), (float)bUseTexture);
		return;
	}
	// UE_LOG(LogTemp, Error, L"PPMID_BG is not valid.");
}

void AGmEUA_ThumbnailCreator::UpdateAlpha() const
{
	if (PPMID_Alpha)
	{
		PPMID_Alpha->SetScalarParameterValue(FName("UseBackground?"), bEnableBackground);
		PPMID_Alpha->SetScalarParameterValue(FName("ObjectTransparency"), ObjectTransparency);
	}
}

void AGmEUA_ThumbnailCreator::ShowMesh() const
{
	switch (DisplayObjType)
	{
	case EGmDisplayObjectType::EActor:
		SkeletalMeshComp->SetVisibility(false, false);
		StaticMeshComp->SetVisibility(false, false);
		if (SpawnedActor)
		{
			SpawnedActor->GetRootComponent()->SetVisibility(true, false);
		}
		break;
	case EGmDisplayObjectType::ESM:
		SkeletalMeshComp->SetVisibility(false, false);
		StaticMeshComp->SetVisibility(true, false);
		if (SpawnedActor)
		{
			SpawnedActor->GetRootComponent()->SetVisibility(false, false);
		}
		break;
	case EGmDisplayObjectType::ESKM:
		SkeletalMeshComp->SetVisibility(true, false);
		StaticMeshComp->SetVisibility(false, false);
		if (SpawnedActor)
		{
			SpawnedActor->GetRootComponent()->SetVisibility(false, false);
		}
		break;
	}
	if (EditorUtilityWidgetRef)
	{
		EditorUtilityWidgetRef->ShowMesh(DisplayObjType);
	}
}

void AGmEUA_ThumbnailCreator::UpdateOutline_Inside() const
{
	if (!PPMID_Outline_Inside)
	{
		// UE_LOG(LogTemp, Error, L"Outline material is not valid.");
		return;
	}
	PPMID_Outline_Inside->SetVectorParameterValue(FName("HighlightColor"), Inside_OutlineColor);
	PPMID_Outline_Inside->SetScalarParameterValue(FName("OutlineGlowIntensity"), Inside_GlowIntensity);
	PPMID_Outline_Inside->SetScalarParameterValue(FName("LineRenderWidth"), Inside_Width);
	PPMID_Outline_Inside->SetScalarParameterValue(FName("EdgeAngleFallOff"), Inside_FallOff);
}

void AGmEUA_ThumbnailCreator::UpdateOutline_Outside() const
{
	if (!PPMID_Outline_Outside)
	{
		// UE_LOG(LogTemp, Error, L"Outline material is not valid.");
		return;
	}
	PPMID_Outline_Outside->SetVectorParameterValue(FName("HighlightColor"), Outside_OutlineColor);
	PPMID_Outline_Outside->SetScalarParameterValue(FName("OutlineGlowIntensity"), Outside_GlowIntensity);
	PPMID_Outline_Outside->SetScalarParameterValue(FName("Radius"), Outside_Radius);
	PPMID_Outline_Outside->SetScalarParameterValue(FName("Steps"), Outside_Steps);
	PPMID_Outline_Outside->SetScalarParameterValue(FName("RadialSteps"), Outside_RadialSteps);
}

void AGmEUA_ThumbnailCreator::UpdateDistance()
{
	UpdateEditorUtilityWidget();

	FVector NewBoxExtent{FVector::ZeroVector};

	switch (DisplayObjType)
	{
	case EGmDisplayObjectType::ESM:
		NewBoxExtent = StaticMeshComp->Bounds.BoxExtent;
		break;
	case EGmDisplayObjectType::ESKM:
		NewBoxExtent = SkeletalMeshComp->Bounds.BoxExtent;
		break;
	case EGmDisplayObjectType::EActor:
		FVector Origin{FVector::ZeroVector};
		SpawnedActor->GetActorBounds(true, Origin, NewBoxExtent, false);
		break;
	}
	SpringArmComp->TargetArmLength = (NewBoxExtent.X + NewBoxExtent.Y + NewBoxExtent.Z) * -2.f;
}

bool AGmEUA_ThumbnailCreator::RefreshAllPointLight(const int32 InLightIndex)
{
	UPointLightComponent* Target{nullptr};
	switch (InLightIndex)
	{
	case 0:
		Target = Center_PointLight;
		break;
	case 1:
		Target = NearBy_PointLight;
		break;
	case 2:
		Target = Distant_PointLight;
		break;
	default: ;
	}
	// const bool IsVisible{Target->IsVisible()};
	Target->ToggleVisibility();
	// Target->SetVisibility(!IsVisible, false);
	UpdateEditorUtilityWidget();
	return Target->IsVisible();
}

bool AGmEUA_ThumbnailCreator::RefreshAllDirectionalLight(const int32 InLightIndex)
{
	UDirectionalLightComponent* Target{nullptr};
	switch (InLightIndex)
	{
	case 0:
		Target = Center_DirectionalLight;
		break;
	case 1:
		Target = NearBy_DirectionalLight;
		break;
	case 2:
		Target = Distant_DirectionalLight;
		break;
	default: ;
	}
	// const bool IsVisible{Target->IsVisible()};
	Target->ToggleVisibility();
	// Target->SetVisibility(!IsVisible, false);
	UpdateEditorUtilityWidget();
	return Target->IsVisible();
}

#undef LOCTEXT_NAMESPACE
