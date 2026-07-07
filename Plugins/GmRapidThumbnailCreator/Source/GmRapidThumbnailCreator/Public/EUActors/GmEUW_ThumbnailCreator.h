// Copyright Dev.GaeMyo 2024. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

#include "EditorUtilityWidget.h"

#include "GmEUW_ThumbnailCreator.generated.h"

class UButton;
class UExpandableArea;
enum class EGmDisplayObjectType : uint8;
class UTextBlock;
class USlider;
class UImage;
class UUGmDetailsView;
class AGmEUA_ThumbnailCreator;

UCLASS(Abstract, meta = (ShowWorldContextPin), Config = Editor)
class GMRAPIDTHUMBNAILCREATOR_API UGmEUW_ThumbnailCreator : public UEditorUtilityWidget
{
	GENERATED_BODY()

public:
	
	// UGmEUW_ThumbnailCreator(const FObjectInitializer& ObjectInitializer);

	virtual bool IsEditorUtility() const override { return true; }

	// UUserWidget Interface
	virtual void NativeOnInitialized() override;
	virtual void NativeConstruct() override;
	virtual void NativePreConstruct() override;
	virtual void NativeDestruct() override;
#if WITH_EDITOR
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
#endif
	//~End of UUserWidget Interface

	// Properties
	UPROPERTY(meta = (BindWidgetOptional))
	TObjectPtr<UImage> Img_MainRenderTarget;

	// Details view
	UPROPERTY(meta = (BindWidgetOptional))
	TObjectPtr<UUGmDetailsView> DV_InitSettings;

	UPROPERTY(meta = (BindWidgetOptional))
	TObjectPtr<UUGmDetailsView> DV_NiagaraOptions;

	UPROPERTY(meta = (BindWidgetOptional))
	TObjectPtr<UUGmDetailsView> DV_LocOffset;
	
	UPROPERTY(meta = (BindWidgetOptional))
	TObjectPtr<UUGmDetailsView> DV_Rotation;

	UPROPERTY(meta = (BindWidgetOptional))
	TObjectPtr<UUGmDetailsView> DV_SKMAsset;

	UPROPERTY(meta = (BindWidgetOptional))
	TObjectPtr<UUGmDetailsView> DV_Actor;

	UPROPERTY(meta = (BindWidgetOptional))
	TObjectPtr<UUGmDetailsView> DV_SKM;

	UPROPERTY(meta = (BindWidgetOptional))
	TObjectPtr<UUGmDetailsView> DV_SM;

	UPROPERTY(meta = (BindWidgetOptional))
	TObjectPtr<UUGmDetailsView> DV_SceneCapture2D;

	// Light info
	UPROPERTY(meta = (BindWidgetOptional))
	TObjectPtr<UUGmDetailsView> DV_CenterLightInfo;
	
	UPROPERTY(meta = (BindWidgetOptional))
	TObjectPtr<UUGmDetailsView> DV_NearByLightInfo;

	UPROPERTY(meta = (BindWidgetOptional))
	TObjectPtr<UUGmDetailsView> DV_NearByLightTransform;

	UPROPERTY(meta = (BindWidgetOptional))
	TObjectPtr<UUGmDetailsView> DV_DistantLightInfo;

	// Distance
	UPROPERTY(meta = (BindWidgetOptional))
	TObjectPtr<UUGmDetailsView> DV_CenterLightDistance;

	UPROPERTY(meta = (BindWidgetOptional))
	TObjectPtr<UUGmDetailsView> DV_NearByLightDistance;

	UPROPERTY(meta = (BindWidgetOptional))
	TObjectPtr<UUGmDetailsView> DV_DistantLightDistance;
	//~End of Distance

	// Distant light transform
	UPROPERTY(meta = (BindWidgetOptional))
	TObjectPtr<UUGmDetailsView> DV_DistantLight_Transform;

	UPROPERTY(meta = (BindWidgetOptional))
	TObjectPtr<UUGmDetailsView> DV_TextureRes;

	UPROPERTY(meta = (BindWidgetOptional))
	TObjectPtr<UUGmDetailsView> DV_Animation;

	// DV Outline
	UPROPERTY(meta = (BindWidgetOptional))
	TObjectPtr<UUGmDetailsView> DV_OutlineInside;
	UPROPERTY(meta = (BindWidgetOptional))
	TObjectPtr<UUGmDetailsView> DV_OutlineOutside;

	// Niagara
	UPROPERTY(meta = (BindWidgetOptional))
	TObjectPtr<UUGmDetailsView> DV_Niagara;
	UPROPERTY(meta = (BindWidgetOptional))
	TObjectPtr<UUGmDetailsView> DV_FXOffset;

	// Background
	UPROPERTY(meta = (BindWidgetOptional))
	TObjectPtr<UUGmDetailsView> DV_Background;

	// Export
	UPROPERTY(meta = (BindWidgetOptional))
	TObjectPtr<UUGmDetailsView> DV_ExportSettings;

	UPROPERTY(meta = (BindWidgetOptional), BlueprintReadOnly, Category = "Gm Rapid Thumbnail Creator")
	TObjectPtr<USlider> AnimationSlider;

	UPROPERTY(meta = (BindWidgetOptional))
	TObjectPtr<UTextBlock> TB_AnimState;

	// Expandable area
	UPROPERTY(meta = (BindWidgetOptional))
	TObjectPtr<UExpandableArea> ExpandableArea_Niagara;
	UPROPERTY(meta = (BindWidgetOptional))
	TObjectPtr<UExpandableArea> ExpandableArea_CenterLight;
	UPROPERTY(meta = (BindWidgetOptional))
	TObjectPtr<UExpandableArea> ExpandableArea_NearByLight;
	UPROPERTY(meta = (BindWidgetOptional))
	TObjectPtr<UExpandableArea> ExpandableArea_DistantLight;

	UPROPERTY(meta = (BindWidgetOptional))
	TObjectPtr<UButton> Btn_Save;
	
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Gm Rapid Thumbnail Creator")
	TObjectPtr<USkeletalMeshComponent> ActorSKM;

	UPROPERTY(BlueprintReadOnly, Category = "Gm Editor Utility Widget")
	float CurrentSliderValue{0.f};

	UFUNCTION(BlueprintCallable, Category = "Gm Editor Utility Widget")
	void ChangeAnimPositionSliderValue(float InValue);

	UFUNCTION(BlueprintCallable, Category = "Gm Rapid Thumbnail Creator")
	void SetAnimPositionSliderMaxValue(const float InValue) const;
	UFUNCTION(BlueprintCallable, Category = "Gm Rapid Thumbnail Creator")
	void ResizeRenderPreview(const FVector2D InWidthAndHeight) const;
	UFUNCTION(BlueprintCallable, CallInEditor, Category = "Gm Rapid Thumbnail Creator")
	void ChangeAnimStateText(const bool InIsPlaying) const;
	UFUNCTION(BlueprintCallable, CallInEditor, Category = "Gm Rapid Thumbnail Creator")
	void ShowMesh(const EGmDisplayObjectType InDisplayObjType) const;
	UFUNCTION(BlueprintCallable, CallInEditor, Category = "Gm Rapid Thumbnail Creator")
	void ShowOutlineOptions(const bool InIsInside, const bool InIsShow) const;
	UFUNCTION(BlueprintCallable, CallInEditor, Category = "Gm Rapid Thumbnail Creator")
	void ShowBackgroundOptions(const bool InIsShow);

	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Gm Rapid Thumbnail Creator")
	TObjectPtr<AGmEUA_ThumbnailCreator> SpawnedThumbnailCreatorActor;

	UFUNCTION(CallInEditor, BlueprintCallable, Category = "Gm Rapid Thumbnail Creator")
	void ExecuteUtility();

	UFUNCTION(BlueprintCallable, Category = "Gm Rapid Thumbnail Creator")
	void SetCenterLightObj();
	UFUNCTION(BlueprintCallable, Category = "Gm Rapid Thumbnail Creator")
	void SetNearByLightObj();
	UFUNCTION(BlueprintCallable, Category = "Gm Rapid Thumbnail Creator")
	void SetDistantLightObj();

private:

	// Variables
	UPROPERTY(EditDefaultsOnly, Category = "Init Settings")
	TSubclassOf<AGmEUA_ThumbnailCreator> SpawnedActorClassRef;

	UFUNCTION()
	void DV_SKMAssetFunc(FName InP);
	UFUNCTION()
	void DV_SMFunc(FName InP);
	UFUNCTION()
	void DV_SimpleUpdateEUW(FName InP);
	UFUNCTION()
	void DV_InitSettingsFunc(FName InP);
	UFUNCTION()
	void DV_SceneCapture2DFunc(FName InP);
	UFUNCTION()
	void DV_SimpleBindUpdateAnim(FName InP);
	bool IsPerspective() const;
	UFUNCTION()
	void Btn_SaveFunc();
	UFUNCTION()
	void DV_ActorFunc(FName InP);
	UFUNCTION()
	void DV_NiagaraOptionsFunc(FName InP);
};
