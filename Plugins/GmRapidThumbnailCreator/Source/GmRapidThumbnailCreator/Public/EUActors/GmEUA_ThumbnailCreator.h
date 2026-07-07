// Copyright Dev.GaeMyo 2024. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

#include "EditorUtilityActor.h"

#include "GmEUA_ThumbnailCreator.generated.h"

class UNiagaraComponent;
class USpringArmComponent;
class UPointLightComponent;
class UGmEUW_ThumbnailCreator;
class USceneCaptureComponent2D;
class USkeletalMeshComponent;
class UStaticMeshComponent;
class UDirectionalLightComponent;
class UTextureRenderTarget2D;
class UMaterialInstanceDynamic;
class UMaterial;
class UTexture;

// DECLARE_DYNAMIC_DELEGATE_OneParam(FGmOnPropertyChanged, class UGmEUW_ThumbnailCreator*, InThumbnailCreatorWidget);

UENUM(BlueprintType)
enum class EGmExportType : uint8
{
	EExportTypeEXR = 0 UMETA(DisplayName = EXR),
	EExportTypePNG = 1 UMETA(DisplayName = PNG),
	EExportTypeJPG = 2 UMETA(DisplayName = JPG)
};

UENUM(BlueprintType)
enum class EGmDisplayObjectType : uint8
{
	EActor = 0 UMETA(DisplayName = "Actor"),
	ESM = 1 UMETA(DisplayName = "Static Mesh"),
	ESKM = 2 UMETA(DisplayName = "Skeletal Mesh")
};

USTRUCT(BlueprintType)
struct FGmThumbnailLightSettings
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Init Settings|Light Settings")
	bool bEnableCenterLight{false};
	
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Init Settings|Light Settings")
	bool bEnableNearByLight{false};
	
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Init Settings|Light Settings")
	bool bEnableDistantLight{false};
	
};

UCLASS(Abstract, Blueprintable, meta = (ShowWorldContextPin))
class GMRAPIDTHUMBNAILCREATOR_API AGmEUA_ThumbnailCreator : public AEditorUtilityActor
{
	GENERATED_BODY()

public:

	AGmEUA_ThumbnailCreator(const FObjectInitializer& ObjectInitializer);

	UFUNCTION(CallInEditor, BlueprintCallable, Category = "Gm Rapid Thumbnail Creator")
	virtual void GiveLife();

protected:

	// AActor interface
	virtual void OnConstruction(const FTransform& Transform) override;
	//~End of AActor interface

public:

	// Properties
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Components")
	TObjectPtr<USceneCaptureComponent2D> SceneCapture2DComponent;

	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Components")
	TObjectPtr<UNiagaraComponent> NiagaraComponent;

	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Components")
	TObjectPtr<USpringArmComponent> SpringArmComp;

	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Components")
	TObjectPtr<UStaticMeshComponent> MeshesLoc;

	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Components")
	TObjectPtr<UStaticMeshComponent> MeshesRot;
	
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Components")
	TObjectPtr<USkeletalMeshComponent> SkeletalMeshComp;

	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Components")
	TObjectPtr<UStaticMeshComponent> StaticMeshComp;

	// Center
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Components")
	TObjectPtr<USpringArmComponent> Center_SpringArm;

	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Components")
	TObjectPtr<UDirectionalLightComponent> Center_DirectionalLight;

	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Components")
	TObjectPtr<UPointLightComponent> Center_PointLight;

	// NerBy

	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Components")
	TObjectPtr<USpringArmComponent> NearBy_SpringArm;
	
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Components")
	TObjectPtr<UDirectionalLightComponent> NearBy_DirectionalLight;

	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Components")
	TObjectPtr<UPointLightComponent> NearBy_PointLight;

	// Distant
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Components")
	TObjectPtr<USpringArmComponent> Distant_SpringArm;

	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Components")
	TObjectPtr<UDirectionalLightComponent> Distant_DirectionalLight;

	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Components")
	TObjectPtr<UPointLightComponent> Distant_PointLight;

	// Variables
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Init Settings", meta = (DisplayName = "Display Object Type"))
	EGmDisplayObjectType DisplayObjType{EGmDisplayObjectType::ESM};

	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Light Settings", meta = (DisplayName = "Light Settings"))
	FGmThumbnailLightSettings ThumbnailLightSettings;

	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Skeletal Mesh Settings", meta = (DisplayName = "Skeletal Mesh Asset"))
	TObjectPtr<USkeletalMesh> SkeletalMeshAsset;
	
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Skeletal Mesh Settings")
	TObjectPtr<USkeletalMeshComponent> ActorSKM;

	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Animation")
	TObjectPtr<UAnimSequenceBase> AnimationAsset;

	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Animation", meta = (ClampMin = -2, ClampMax = 2))
	double AnimPlayRate{1.f};

	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Animation Settings")
	double AnimPosition{0.f};
	
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Actor")
	TSubclassOf<AActor> TargetActor;

	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Location Offset")
	FVector2D ViewLocationOffset;

	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Resolution", meta = (ClampMin = 16, ClampMax = 8192))
	int32 TextureResWidth{2048};

	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Resolution", meta = (ClampMin = 16, ClampMax = 8192))
	int32 TextureResHeight{2048};

	FVector2D CenterLightDistanceMinMax{FVector2D(-100, 100)};
	FVector2D NearByLightDistanceMinMax{FVector2D(25, 100)};
	FVector2D DistantLightDistanceMinMax{FVector2D(100, 300)};

	// Outline options
	UPROPERTY(BlueprintReadOnly, EditAnywhere, Category = "Outline")
	bool bEnableInsideOutline{false};
	UPROPERTY(BlueprintReadOnly, EditAnywhere, Category = "Outline")
	bool bEnableOutsideOutline{false};

	// Niagara options
	UPROPERTY(BlueprintReadOnly, EditAnywhere, Category = "Niagara Settings")
	bool bEnableNiagara{false};

	UPROPERTY(BlueprintReadOnly, EditAnywhere, Category = "FX Offset")
	FVector2D FXLocationOffset;
	UPROPERTY(BlueprintReadOnly, EditAnywhere, Category = "FX Offset")
	float FXDistance{100.f};

	// Background
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Init Settings")
	bool bEnableBackground{false};
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Background", meta = (EditCondition = bUseTexture))
	TObjectPtr<UTexture> BackgroundTexture;
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Background")
	FLinearColor BackgroundColor{FLinearColor(0.272569f, 0.272569f, 0.272569f, 1.f)};
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Background", meta = (EditCondition = bEnableBackground))
	float ObjectTransparency{0.0f};
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Background", meta = (EditCondition = bEnableBackground))
	bool bUseTexture{false};
	
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Outline|Outside")
	FLinearColor Outside_OutlineColor{FLinearColor(1, 1, 1, 1)};
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Outline|Outside")
	float Outside_GlowIntensity{4.5f};
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Outline|Outside")
	float Outside_Radius{15.f};
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Outline|Outside")
	float Outside_Steps{4.5f};
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Outline|Outside")
	float Outside_RadialSteps{10.f};
	
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Outline|Inside")
	FLinearColor Inside_OutlineColor{FLinearColor(1, 1, 1, 1)};
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Outline|Inside")
	float Inside_GlowIntensity{1.f};
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Outline|Inside")
	float Inside_Width{2.f};
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Outline|Inside")
	float Inside_FallOff{-100.f};

	// Created post process material ref
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "PPMRef")
	TObjectPtr<UMaterialInstanceDynamic> PPMID_Outline_Outside;
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "PPMRef")
	TObjectPtr<UMaterialInstanceDynamic> PPMID_Outline_Inside;
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "PPMRef")
	TObjectPtr<UMaterialInstanceDynamic> PPMID_BG;
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "PPMRef")
	TObjectPtr<UMaterialInstanceDynamic> PPMID_Alpha;
	
	//~End of Outline options

	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Gm Rapid Thumbnail Creator")
	TObjectPtr<UTextureRenderTarget2D> RenderTarget2D_EXR;
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Gm Rapid Thumbnail Creator")
	TObjectPtr<UTextureRenderTarget2D> RenderTarget2D_PNG;

	/**
	 * Export path
	 */
	UPROPERTY(BlueprintReadOnly, EditAnywhere, Category = "Export Settings")
	FString ExportPath{FString("/Game/GmRapidThumbnailCreator/")};
	
	/**
	 * Output file name
	 */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Export Settings", meta = (MultiLine = true))
	FString OutputFileName{FString("Texture")};

	/**
	 * Export Type
	 */
	UPROPERTY(BlueprintReadOnly, EditAnywhere, Category = "Export Settings", meta = (DisplayName = "Export Type", EditCondition = bExportLocalDesktop))
	EGmExportType ExportType{EGmExportType::EExportTypeEXR};

	/**
	 * Export to local desktop
	 */
	UPROPERTY(BlueprintReadOnly, EditAnywhere, Category = "Export Settings")
	bool bExportLocalDesktop{false};

	/** Skip the post-save modal when thumbnails are generated by an editor script. */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Export Settings")
	bool bSuppressSaveConfirmation{false};

	// Outline materials
	UPROPERTY(EditDefaultsOnly, Category = "Init Settings|Materials")
	TObjectPtr<UMaterial> PPM_InsideOutline; 

	UPROPERTY(EditDefaultsOnly, Category = "Init Settings|Materials")
	TObjectPtr<UMaterial> PPM_OutsideOutline;

	UPROPERTY(EditDefaultsOnly, Category = "Init Settings|Materials")
	TObjectPtr<UMaterial> PPM_Background;

	UPROPERTY(EditDefaultsOnly, Category = "Init Settings|Materials")
	TObjectPtr<UMaterial> PPM_Alpha;

	UPROPERTY(BlueprintReadOnly, EditAnywhere, Category = "Gm Rapid Thumbnail Creator")
	TObjectPtr<AActor> SpawnedActor{nullptr};

#if WITH_EDITOR
	
	UFUNCTION(CallInEditor, BlueprintCallable, Category = "Gm Rapid Thumbnail Creator")
	void UpdateCameraCaptureRender();
	
	UFUNCTION(CallInEditor, Category = "Gm Rapid Thumbnail Creator")
	bool RefreshChildActor(USkeletalMeshComponent*& OutNewSKM);
	//
	UFUNCTION(BlueprintCallable, CallInEditor, Category = "Gm Rapid Thumbnail Creator")
	void UpdateEditorUtilityWidget();
	UFUNCTION(CallInEditor, Category = "Gm Rapid Thumbnail Creator")
	void UpdateAllLights() const;
	UFUNCTION(CallInEditor, Category = "Gm Rapid Thumbnail Creator")
	bool AddRemoveAllLights(const int32 InLightIndex);
	UFUNCTION(CallInEditor, BlueprintImplementableEvent, Category = "Gm Rapid Thumbnail Creator")
	void UpdateAllMeshes();
	UFUNCTION(BlueprintCallable, CallInEditor, Category = "Gm Rapid Thumbnail Creator")
	void ChangeRenderSize() const;
	UFUNCTION(CallInEditor, BlueprintImplementableEvent, BlueprintCallable, Category = "Gm Rapid Thumbnail Creator")
	void UpdateAnim();
	UFUNCTION(CallInEditor, Category = "Gm Rapid Thumbnail Creator")
	void CreatePPMats();
	
	UFUNCTION(CallInEditor, Category = "Gm Rapid Thumbnail Creator")
	void UpdatePPMBlends() const;
	// Show static or skeletal mesh
	UFUNCTION(CallInEditor, Category = "Gm Rapid Thumbnail Creator")
	void ShowMesh() const;
	UFUNCTION(CallInEditor, BlueprintCallable, Category = "Gm Rapid Thumbnail Creator")
	void UpdateBackground() const;
	UFUNCTION(CallInEditor, BlueprintCallable, Category = "Gm Rapid Thumbnail Creator")
	void UpdateAlpha() const;
	UFUNCTION(CallInEditor, BlueprintCallable, Category = "Gm Rapid Thumbnail Creator")
	void UpdateOutline_Inside() const;
	UFUNCTION(CallInEditor, BlueprintCallable, Category = "Gm Rapid Thumbnail Creator")
	void UpdateOutline_Outside() const;
	UFUNCTION(CallInEditor, Category = "Gm Rapid Thumbnail Creator")
	bool RefreshAllPointLight(const int32 InLightIndex);
	UFUNCTION(CallInEditor, Category = "Gm Rapid Thumbnail Creator")
	bool RefreshAllDirectionalLight(const int32 InLightIndex);
	
	UFUNCTION(CallInEditor, Category = "Gm Rapid Thumbnail Creator")
	void UpdateDistance();

	UFUNCTION(CallInEditor, BlueprintCallable, BlueprintImplementableEvent, Category = "Gm Rapid Thumbnail Creator")
	void UpdateAnimationPlayRate();
	
	UFUNCTION(CallInEditor, BlueprintCallable, Category = "Gm Rapid Thumbnail Creator")
	void SaveStaticTexture();
	
#endif

	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Ref")
	TObjectPtr<UGmEUW_ThumbnailCreator> EditorUtilityWidgetRef{nullptr};

	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Gm Rapid Thumbnail Creator")
	bool bPlaying{false};

	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Gm Rapid Thumbnail Creator")
	bool bHasAnimAsset{false};

	UPROPERTY(BlueprintReadWrite, EditAnywhere, meta = (ClampMin = 0.001f), Category = "Gm Rapid Thumbnail Creator")
	float SliderUpdateRate{0.005f};

private:
	
	friend UGmEUW_ThumbnailCreator;
	
	void GmExportTexture(UTexture2D* InTexture, const FString& InFilename, const FString& InExtension) const;

	// Utils
	ECameraProjectionMode::Type GmGetSceneCapture2DProjectionType() const;
	void GmSetSceneCapture2DProjectionType(ECameraProjectionMode::Type InNewProjectionType) const;
	
};
