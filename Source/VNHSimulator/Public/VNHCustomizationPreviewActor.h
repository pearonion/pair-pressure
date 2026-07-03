#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "VNHGameplayTypes.h"
#include "VNHCustomizationPreviewActor.generated.h"

class USceneCaptureComponent2D;
class USceneComponent;
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
	void SetAutoRotate(bool bEnabled) { bAutoRotate = bEnabled; }

protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

private:
	UPROPERTY(VisibleAnywhere, Category = "VNH|Preview")
	TObjectPtr<USceneComponent> PreviewRoot;

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
	float PreviewYaw = 0.0f;

	void EnsureRenderTarget();
	USkeletalMeshComponent* CreateCosmeticComponent(const TCHAR* ComponentName);
	void ConfigureMeshComponent(USkeletalMeshComponent* MeshComponent);
	void ApplySlotMesh(USkeletalMeshComponent* SlotComponent, const TSoftObjectPtr<USkeletalMesh>& MeshAsset, bool bHideSlot = false);
	void ApplyColorToMesh(USkeletalMeshComponent* MeshComponent, const FLinearColor& Color);
};
