// Copyright Dev.GaeMyo 2024. All Rights Reserved.

#include "EUActors/GmEUW_ThumbnailCreator.h"

#include <EngineUtils.h>
#include <Components/Button.h>
#include <Components/ExpandableArea.h>
#include <Components/Image.h>
#include <Components/Slider.h>
#include <Components/TextBlock.h>

#include "EUActors/GmEUA_ThumbnailCreator.h"
#include "Widgets/Misc/UGmDetailsView.h"

// UGmEUW_ThumbnailCreator::UGmEUW_ThumbnailCreator(const FObjectInitializer& ObjectInitializer)
	// :Super(ObjectInitializer)
// {}

void UGmEUW_ThumbnailCreator::NativeOnInitialized()
{
	Super::NativeOnInitialized();
}

void UGmEUW_ThumbnailCreator::NativeConstruct()
{
	Super::NativeConstruct();
	
	Btn_Save->OnClicked.AddUniqueDynamic(this, &ThisClass::Btn_SaveFunc);

	DV_InitSettings->GmGetPropertyChangedDel().AddUniqueDynamic(this, &ThisClass::DV_InitSettingsFunc);
	DV_SceneCapture2D->GmGetPropertyChangedDel().AddUniqueDynamic(this, &ThisClass::DV_SceneCapture2DFunc);
	DV_SKMAsset->GmGetPropertyChangedDel().AddUniqueDynamic(this, &ThisClass::DV_SKMAssetFunc);
	DV_SM->GmGetPropertyChangedDel().AddUniqueDynamic(this, &ThisClass::DV_SMFunc);
	
	DV_ExportSettings->GmGetPropertyChangedDel().AddUniqueDynamic(this, &ThisClass::DV_SimpleUpdateEUW);
	DV_OutlineInside->GmGetPropertyChangedDel().AddUniqueDynamic(this, &ThisClass::DV_SimpleUpdateEUW);
	DV_OutlineOutside->GmGetPropertyChangedDel().AddUniqueDynamic(this, &ThisClass::DV_SimpleUpdateEUW);
	DV_Background->GmGetPropertyChangedDel().AddUniqueDynamic(this, &ThisClass::DV_SimpleUpdateEUW);
	DV_LocOffset->GmGetPropertyChangedDel().AddUniqueDynamic(this, &ThisClass::DV_SimpleUpdateEUW);

	DV_Actor->GmGetPropertyChangedDel().AddUniqueDynamic(this, &ThisClass::DV_ActorFunc);

	// Bind simple update anim
	DV_DistantLight_Transform->GmGetPropertyChangedDel().AddUniqueDynamic(this, &ThisClass::DV_SimpleBindUpdateAnim);
	DV_NearByLightTransform->GmGetPropertyChangedDel().AddUniqueDynamic(this, &ThisClass::DV_SimpleBindUpdateAnim);
	
	DV_DistantLightInfo->GmGetPropertyChangedDel().AddUniqueDynamic(this, &ThisClass::DV_SimpleBindUpdateAnim);
	DV_NearByLightInfo->GmGetPropertyChangedDel().AddUniqueDynamic(this, &ThisClass::DV_SimpleBindUpdateAnim);
	DV_CenterLightInfo->GmGetPropertyChangedDel().AddUniqueDynamic(this, &ThisClass::DV_SimpleBindUpdateAnim);
	DV_Rotation->GmGetPropertyChangedDel().AddUniqueDynamic(this, &ThisClass::DV_SimpleBindUpdateAnim);

	DV_DistantLightDistance->GmGetPropertyChangedDel().AddUniqueDynamic(this, &ThisClass::DV_SimpleBindUpdateAnim);
	DV_NearByLightDistance->GmGetPropertyChangedDel().AddUniqueDynamic(this, &ThisClass::DV_SimpleBindUpdateAnim);
	DV_CenterLightDistance->GmGetPropertyChangedDel().AddUniqueDynamic(this, &ThisClass::DV_SimpleBindUpdateAnim);

	// Niagara
	DV_NiagaraOptions->GmGetPropertyChangedDel().AddUniqueDynamic(this, &ThisClass::DV_NiagaraOptionsFunc);
	
	ExecuteUtility();
}

void UGmEUW_ThumbnailCreator::NativePreConstruct()
{
	Super::NativePreConstruct();
}

void UGmEUW_ThumbnailCreator::NativeDestruct()
{
	if (SpawnedThumbnailCreatorActor)
	{
		if (AActor* SpawnedCompActor{SpawnedThumbnailCreatorActor->SpawnedActor})
		{
			SpawnedCompActor->Destroy();
		}
		SpawnedThumbnailCreatorActor->Destroy();
	}
	
	Super::NativeDestruct();
}

#if WITH_EDITOR
void UGmEUW_ThumbnailCreator::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);
}
#endif

void UGmEUW_ThumbnailCreator::ChangeAnimPositionSliderValue(const float InValue)
{
	if (const float CurrentCond{AnimationSlider->GetValue() + InValue}; CurrentCond >= AnimationSlider->GetMaxValue())
	{
		AnimationSlider->SetValue(0);
		CurrentSliderValue = 0;
	}
	else if(CurrentCond <= 0)
	{
		AnimationSlider->SetValue(AnimationSlider->GetMaxValue());
		CurrentSliderValue = AnimationSlider->GetMaxValue();
	}
	else
	{
		AnimationSlider->SetValue(CurrentCond);
		CurrentSliderValue = CurrentCond;
	}
}

void UGmEUW_ThumbnailCreator::SetAnimPositionSliderMaxValue(const float InValue) const
{
	AnimationSlider->SetValue(0);
	AnimationSlider->SetMaxValue(InValue);
}

void UGmEUW_ThumbnailCreator::ResizeRenderPreview(const FVector2D InWidthAndHeight) const
{
	FSlateBrush NewBrush;
	NewBrush.SetImageSize(InWidthAndHeight);
	
#if ENGINE_MAJOR_VERSION == 5
#if ENGINE_MINOR_VERSION <= 1
	NewBrush.SetResourceObject(Img_MainRenderTarget->Brush.GetResourceObject());
#else
	NewBrush.SetResourceObject(Img_MainRenderTarget->GetBrush().GetResourceObject());
#endif
#endif
	
	Img_MainRenderTarget->SetBrush(NewBrush);
}

void UGmEUW_ThumbnailCreator::ChangeAnimStateText(const bool InIsPlaying) const
{
	TB_AnimState->SetText(InIsPlaying ? FText::FromString(TEXT("Pause animation")) : FText::FromString(TEXT("Play animation")));
}

void UGmEUW_ThumbnailCreator::ShowMesh(const EGmDisplayObjectType InDisplayObjType) const
{
	switch (InDisplayObjType)
	{
	case EGmDisplayObjectType::EActor:
		DV_SM->SetVisibility(ESlateVisibility::Collapsed);
		DV_SKM->SetVisibility(ESlateVisibility::Collapsed);
		DV_SKMAsset->SetVisibility(ESlateVisibility::Collapsed);
		DV_Animation->SetVisibility(ESlateVisibility::Collapsed);
		DV_Actor->SetVisibility(ESlateVisibility::Visible);
		ActorSKM ? DV_Animation->SetVisibility(ESlateVisibility::Visible) :
		DV_Animation->SetVisibility(ESlateVisibility::Collapsed);
		break;
	case EGmDisplayObjectType::ESM:
		DV_SM->SetVisibility(ESlateVisibility::Visible);
		DV_SKM->SetVisibility(ESlateVisibility::Collapsed);
		DV_SKMAsset->SetVisibility(ESlateVisibility::Collapsed);
		DV_Animation->SetVisibility(ESlateVisibility::Collapsed);
		DV_Actor->SetVisibility(ESlateVisibility::Collapsed);
		break;
	case EGmDisplayObjectType::ESKM:
		DV_SM->SetVisibility(ESlateVisibility::Collapsed);
		DV_Actor->SetVisibility(ESlateVisibility::Collapsed);
		DV_SKM->SetVisibility(ESlateVisibility::Visible);
		DV_SKMAsset->SetVisibility(ESlateVisibility::Visible);
		if (SpawnedThumbnailCreatorActor)
		{
			SpawnedThumbnailCreatorActor->SkeletalMeshComp->GetSkinnedAsset() ?
				DV_Animation->SetVisibility(ESlateVisibility::Visible) :
			DV_Animation->SetVisibility(ESlateVisibility::Collapsed);
		}
		break;
	}
}

void UGmEUW_ThumbnailCreator::ShowOutlineOptions(const bool InIsInside, const bool InIsShow) const
{
	(InIsInside ? DV_OutlineInside : DV_OutlineOutside)->SetVisibility(
		InIsShow ? ESlateVisibility::Visible : ESlateVisibility::Collapsed);
}

void UGmEUW_ThumbnailCreator::ShowBackgroundOptions(const bool InIsShow)
{
	DV_Background->SetVisibility(/*@TODO Add background options.*/InIsShow ? ESlateVisibility::Visible : ESlateVisibility::Collapsed);
}

void UGmEUW_ThumbnailCreator::ExecuteUtility()
{
	if (!SpawnedActorClassRef)
	{
		// UE_LOG(LogTemp, Error, L"SpawnedActorClassRef is not valid.");
		return;
	}
	// Spawn new Thumbnail creator actor
	if (UWorld* World{GetWorld()})
	{
		for (TActorIterator<AGmEUA_ThumbnailCreator> It(World, AGmEUA_ThumbnailCreator::StaticClass()); It; ++It)
		{
			(*It)->Destroy();
		}

		SpawnedThumbnailCreatorActor = Cast<AGmEUA_ThumbnailCreator>(World->SpawnActor(SpawnedActorClassRef));

		if (SpawnedThumbnailCreatorActor)
		{
			SpawnedThumbnailCreatorActor->EditorUtilityWidgetRef = this;

			// ~~~~~~~~~~~~~~All Set Objs
			DV_Actor->SetObject(SpawnedThumbnailCreatorActor);

			// Outlines
			DV_OutlineInside->SetObject(SpawnedThumbnailCreatorActor);
			DV_OutlineOutside->SetObject(SpawnedThumbnailCreatorActor);
			
			DV_InitSettings->SetObject(SpawnedThumbnailCreatorActor);
			DV_LocOffset->SetObject(SpawnedThumbnailCreatorActor);

			DV_SKMAsset->SetObject(SpawnedThumbnailCreatorActor);
			DV_TextureRes->SetObject(SpawnedThumbnailCreatorActor);

			DV_Background->SetObject(SpawnedThumbnailCreatorActor);
			DV_Animation->SetObject(SpawnedThumbnailCreatorActor);

			// Camera
			DV_SceneCapture2D->SetObject((UObject*)SpawnedThumbnailCreatorActor->SceneCapture2DComponent);
			// Skeletal mesh
			DV_SKM->SetObject(SpawnedThumbnailCreatorActor->SkeletalMeshComp);
			// Static mesh
			DV_SM->SetObject(SpawnedThumbnailCreatorActor->StaticMeshComp);
			// Rotation
			DV_Rotation->SetObject(SpawnedThumbnailCreatorActor->MeshesRot);

			// Export
			DV_ExportSettings->SetObject(SpawnedThumbnailCreatorActor);

			// Niagara
			DV_NiagaraOptions->SetObject(SpawnedThumbnailCreatorActor);
			DV_Niagara->SetObject((UObject*)SpawnedThumbnailCreatorActor->NiagaraComponent);
			DV_FXOffset->SetObject(SpawnedThumbnailCreatorActor);
			
			// Preset, 

			SpawnedThumbnailCreatorActor->GiveLife();
			SpawnedThumbnailCreatorActor->UpdateAllLights();
			SetCenterLightObj();
			SetDistantLightObj();
			SetNearByLightObj();
		}
	}
}

void UGmEUW_ThumbnailCreator::SetCenterLightObj()
{
	if (SpawnedThumbnailCreatorActor)
	{
		DV_CenterLightDistance->SetObject((UObject*)SpawnedThumbnailCreatorActor->Center_SpringArm);
		DV_CenterLightInfo->SetObject(IsPerspective() ? (UObject*)SpawnedThumbnailCreatorActor->Center_PointLight :
			(UObject*)SpawnedThumbnailCreatorActor->Center_DirectionalLight);
	}
}

void UGmEUW_ThumbnailCreator::SetNearByLightObj()
{
	if (SpawnedThumbnailCreatorActor)
	{
		DV_NearByLightDistance->SetObject((UObject*)SpawnedThumbnailCreatorActor->NearBy_SpringArm);
		DV_NearByLightTransform->SetObject((UObject*)SpawnedThumbnailCreatorActor->NearBy_SpringArm);
		DV_NearByLightInfo->SetObject(IsPerspective() ? (UObject*)SpawnedThumbnailCreatorActor->NearBy_PointLight :
			(UObject*)SpawnedThumbnailCreatorActor->NearBy_DirectionalLight);
	}
}

void UGmEUW_ThumbnailCreator::SetDistantLightObj()
{
	if (SpawnedThumbnailCreatorActor)
	{
		DV_DistantLightDistance->SetObject((UObject*)SpawnedThumbnailCreatorActor->Distant_SpringArm);
		DV_DistantLight_Transform->SetObject((UObject*)SpawnedThumbnailCreatorActor->Distant_SpringArm);
		DV_DistantLightInfo->SetObject(IsPerspective() ? (UObject*)SpawnedThumbnailCreatorActor->Distant_PointLight :
			(UObject*)SpawnedThumbnailCreatorActor->Distant_DirectionalLight);
	}
}

void UGmEUW_ThumbnailCreator::DV_SKMAssetFunc(FName)
{
	if (SpawnedThumbnailCreatorActor)
	{
		SpawnedThumbnailCreatorActor->UpdateAllMeshes();
		SpawnedThumbnailCreatorActor->UpdateEditorUtilityWidget();
	}
}

void UGmEUW_ThumbnailCreator::DV_SMFunc(const FName InP)
{
	if (SpawnedThumbnailCreatorActor)
	{
		if (InP == FName("StaticMesh"))
		{
			SpawnedThumbnailCreatorActor->UpdateAllMeshes();
		}
		else
		{
			SpawnedThumbnailCreatorActor->UpdateDistance();
		}
		SpawnedThumbnailCreatorActor->UpdateEditorUtilityWidget();
	}
}

void UGmEUW_ThumbnailCreator::DV_SimpleUpdateEUW(FName)
{
	if (SpawnedThumbnailCreatorActor)
	{
		SpawnedThumbnailCreatorActor->UpdateEditorUtilityWidget();
	}
}

void UGmEUW_ThumbnailCreator::DV_InitSettingsFunc(const FName InP)
{
	if (SpawnedThumbnailCreatorActor)
	{
		ESlateVisibility NewVisibility{ESlateVisibility::Collapsed};
		
		if (InP == FName("DisplayObjType"))
		{
			if (SpawnedThumbnailCreatorActor->DisplayObjType != EGmDisplayObjectType::ESM)
			{
				SpawnedThumbnailCreatorActor->UpdateAllMeshes();
			}
		}
		else if (InP == FName("bEnableCenterLight"))
		{
			if (SpawnedThumbnailCreatorActor->AddRemoveAllLights(0))
			{
				NewVisibility = ESlateVisibility::Visible;
			}
			ExpandableArea_CenterLight->SetVisibility(NewVisibility);
		}
		else if (InP == FName("bEnableNearByLight"))
		{
			if (SpawnedThumbnailCreatorActor->AddRemoveAllLights(1))
			{
				NewVisibility = ESlateVisibility::Visible;
			}
			ExpandableArea_NearByLight->SetVisibility(NewVisibility);
		}
		else if (InP == FName("bEnableDistantLight"))
		{
			if (SpawnedThumbnailCreatorActor->AddRemoveAllLights(2))
			{
				NewVisibility = ESlateVisibility::Visible;
			}
			ExpandableArea_DistantLight->SetVisibility(NewVisibility);
		}
		else if (InP == FName("bEnableBackground"))
		{
			if (SpawnedThumbnailCreatorActor->GmGetSceneCapture2DProjectionType() ==
				ECameraProjectionMode::Perspective)
			{
				SpawnedThumbnailCreatorActor->GmSetSceneCapture2DProjectionType(ECameraProjectionMode::Orthographic);
				DV_SceneCapture2DFunc(FName("ProjectionType"));
			}
		}
		SpawnedThumbnailCreatorActor->UpdateEditorUtilityWidget();
	}
}

void UGmEUW_ThumbnailCreator::DV_SceneCapture2DFunc(const FName InP)
{
	if (SpawnedThumbnailCreatorActor)
	{
		SpawnedThumbnailCreatorActor->UpdateAnim();
		if (InP == FName("ProjectionType"))
		{
			SpawnedThumbnailCreatorActor->UpdateAllLights();
			SetNearByLightObj();
			SetCenterLightObj();
			SetDistantLightObj();
			const ESlateVisibility NewVisibility{IsPerspective() ? ESlateVisibility::Visible :
				ESlateVisibility::Collapsed};
			
			DV_CenterLightDistance->SetVisibility(NewVisibility);
			DV_NearByLightDistance->SetVisibility(NewVisibility);
			DV_DistantLightDistance->SetVisibility(NewVisibility);
		}
	}
}

void UGmEUW_ThumbnailCreator::DV_SimpleBindUpdateAnim(FName)
{
	if (SpawnedThumbnailCreatorActor)
	{
		SpawnedThumbnailCreatorActor->UpdateAnim();
	}
}

bool UGmEUW_ThumbnailCreator::IsPerspective() const
{
	if (SpawnedThumbnailCreatorActor)
	{
		return SpawnedThumbnailCreatorActor->GmGetSceneCapture2DProjectionType() ==
			ECameraProjectionMode::Perspective;
	}
	return false;
}

void UGmEUW_ThumbnailCreator::Btn_SaveFunc()
{
	if (SpawnedThumbnailCreatorActor)
	{
		SpawnedThumbnailCreatorActor->SaveStaticTexture();
	}
}

void UGmEUW_ThumbnailCreator::DV_ActorFunc(FName)
{
	if (SpawnedThumbnailCreatorActor)
	{
		if (USkeletalMeshComponent* NewTarget{nullptr};SpawnedThumbnailCreatorActor->RefreshChildActor(NewTarget))
		{
			if (NewTarget)
			{
				ActorSKM = NewTarget;
			}
			SpawnedThumbnailCreatorActor->UpdateCameraCaptureRender();
			SpawnedThumbnailCreatorActor->UpdateAllMeshes();
		}
	}
}

void UGmEUW_ThumbnailCreator::DV_NiagaraOptionsFunc(const FName InP)
{
	if (SpawnedThumbnailCreatorActor)
	{
		if (InP == FName("bEnableNiagara"))
		{
			ExpandableArea_Niagara->SetVisibility(SpawnedThumbnailCreatorActor->bEnableNiagara ?
				ESlateVisibility::Visible : ESlateVisibility::Collapsed);
		}
	}
}

// void UGmEUW_ThumbnailCreator::DV_LightInfoInitSettings()
// {
// 	TArray<FName> LICategories, LIProperties;
// 	LICategories.SetNum(3);
// 	LICategories[0] = FName("LightFunction");
// 	LICategories[1] = FName("LightShafts");
// 	LICategories[2] = FName("DistanceFieldShadows");
// 	LIProperties.SetNum(19);
// 	LIProperties[0] = FName("IntensityUnits");
// 	LIProperties[1] = FName("Intensity");
// 	LIProperties[2] = FName("LightColor");
// 	LIProperties[3] = FName("bUseTemperature");
// 	LIProperties[4] = FName("Temperature");
// 	LIProperties[5] = FName("AttenuationRadius");
// 	LIProperties[6] = FName("MaxDrawDistance");
// 	LIProperties[7] = FName("MaxDistanceFadeRange");
// 	LIProperties[8] = FName("SpecularScale");
// 	LIProperties[9] = FName("ShadowResolutionScale");
// 	LIProperties[10] = FName("ShadowBias");
// 	LIProperties[11] = FName("ShadowSlopeBias");
// 	LIProperties[12] = FName("ShadowSharpen");
// 	LIProperties[13] = FName("ContactShadowLength");
// 	LIProperties[14] = FName("ContactShadowLengthInWS");
// 	LIProperties[15] = FName("CastTranslucentShadows");
// 	LIProperties[16] = FName("bCastShadowsFromCinematicObjectsOnly");
// 	LIProperties[17] = FName("bAffectDynamicIndirectLighting");
// 	LIProperties[18] = FName("LightingChannels");
//
// 	auto SetLI{[this, LICategories, LIProperties](UUGmDetailsView* InDV)->void
// 	{
// 		if(InDV)
// 			return;
// 		InDV->CategoriesToShow = LICategories; InDV->PropertiesToShow = LIProperties;
// 	}};
// 	SetLI(DV_NearByLightInfo);
// 	SetLI(DV_CenterLightInfo);
// 	SetLI(DV_DistantLightInfo);
// }
