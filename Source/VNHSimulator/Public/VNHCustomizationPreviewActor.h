#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "TimerManager.h"
#include "VNHGameplayTypes.h"
#include "VNHCustomizationPreviewActor.generated.h"

class USceneCaptureComponent2D;
class USceneComponent;
class UAnimationAsset;
class USkeletalMesh;
class USkeletalMeshComponent;
class UTextureRenderTarget2D;

UCLASS(NotPlaceable, Transient)
class VNHSIMULATOR_API AVNHCustomizationPreviewActor : public AActor
{
	GENERATED_BODY()

public:
	AVNHCustomizationPreviewActor();

	virtual void Tick(float DeltaSeconds) override;

	UTextureRenderTarget2D* GetOrCreateRenderTarget();
	void ApplyCustomization(const FVNHCharacterCustomization& Customization);
	void AddYawInput(float DeltaYaw);
	void PlaySlotChangeAnimation(EVNHCustomizationSlot CustomizationSlot, bool bRemovingItem);
	void SetUseFemalePreviewAnimations(bool bInUseFemalePreviewAnimations);
	void SetAutoRotate(bool bEnabled) { bAutoRotate = bEnabled; }

protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

private:
	UPROPERTY(VisibleAnywhere, Category = "VNH|Preview")
	TObjectPtr<USceneComponent> PreviewRoot;

	UPROPERTY(VisibleAnywhere, Category = "VNH|Preview")
	TObjectPtr<USceneComponent> CharacterRoot;

	UPROPERTY(VisibleAnywhere, Category = "VNH|Preview")
	TObjectPtr<USkeletalMeshComponent> BodyMeshComponent;

	UPROPERTY(VisibleAnywhere, Category = "VNH|Preview")
	TObjectPtr<USkeletalMeshComponent> HairMeshComponent;

	UPROPERTY(VisibleAnywhere, Category = "VNH|Preview")
	TObjectPtr<USkeletalMeshComponent> FaceMeshComponent;

	UPROPERTY(VisibleAnywhere, Category = "VNH|Preview")
	TObjectPtr<USkeletalMeshComponent> HatMeshComponent;

	UPROPERTY(VisibleAnywhere, Category = "VNH|Preview")
	TObjectPtr<USkeletalMeshComponent> MustacheMeshComponent;

	UPROPERTY(VisibleAnywhere, Category = "VNH|Preview")
	TObjectPtr<USkeletalMeshComponent> OutfitMeshComponent;

	UPROPERTY(VisibleAnywhere, Category = "VNH|Preview")
	TObjectPtr<USkeletalMeshComponent> OutwearMeshComponent;

	UPROPERTY(VisibleAnywhere, Category = "VNH|Preview")
	TObjectPtr<USkeletalMeshComponent> PantsMeshComponent;

	UPROPERTY(VisibleAnywhere, Category = "VNH|Preview")
	TObjectPtr<USkeletalMeshComponent> ShoesMeshComponent;

	UPROPERTY(VisibleAnywhere, Category = "VNH|Preview")
	TObjectPtr<USkeletalMeshComponent> AccessoryMeshComponent;

	UPROPERTY(VisibleAnywhere, Category = "VNH|Preview")
	TObjectPtr<USceneCaptureComponent2D> SceneCaptureComponent;

	UPROPERTY(Transient)
	TObjectPtr<UTextureRenderTarget2D> RenderTarget;

	bool bAutoRotate = false;
	bool bUseFemalePreviewAnimations = false;
	float PreviewYaw = 0.0f;
	FTimerHandle ReturnToIdleTimerHandle;
	float ProceduralChangeElapsed = 0.0f;
	float ProceduralChangeDuration = 0.0f;
	bool bProceduralChangeRemovingItem = false;
	EVNHCustomizationSlot ProceduralChangeSlot = EVNHCustomizationSlot::Outfit;

	void EnsureRenderTarget();
	USkeletalMeshComponent* CreateCosmeticComponent(const TCHAR* ComponentName);
	void ConfigureMeshComponent(USkeletalMeshComponent* MeshComponent);
	void ApplySlotMesh(USkeletalMeshComponent* SlotComponent, const TSoftObjectPtr<USkeletalMesh>& MeshAsset, bool bHideSlot = false);
	void ApplyColorToMesh(USkeletalMeshComponent* MeshComponent, const FLinearColor& Color);
	UAnimationAsset* LoadCompatibleAnimation(const TArray<FString>& CandidatePaths) const;
	UAnimationAsset* LoadPreferredAnimation(const TCHAR* MalePath, const TCHAR* FemalePath) const;
	UAnimationAsset* LoadPreferredAnimation(const TCHAR* MalePath, const TCHAR* FemalePath, const TCHAR* MaleFallbackPath, const TCHAR* FemaleFallbackPath) const;
	void PlayAnimationAsset(UAnimationAsset* AnimationAsset, bool bLooping);
	void PlayIdleAnimation();
	void StartProceduralChangeAnimation(EVNHCustomizationSlot CustomizationSlot, bool bRemovingItem);
	void UpdateProceduralChangeAnimation(float DeltaSeconds);
	void ResetBodyPreviewPose();
};
