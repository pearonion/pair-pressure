#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "PPMascotPreviewActor.generated.h"

class UAnimSequence;
class UPointLightComponent;
class USceneCaptureComponent2D;
class USceneComponent;
class USkeletalMesh;
class USkeletalMeshComponent;
class UTextureRenderTarget2D;

UCLASS(NotBlueprintable, Transient)
class VNHSIMULATOR_API APPMascotPreviewActor : public AActor
{
	GENERATED_BODY()

public:
	APPMascotPreviewActor();

	void ConfigurePreview(
		USkeletalMesh* InMesh,
		UAnimSequence* InAnimation,
		UTextureRenderTarget2D* InRenderTarget,
		bool bLoopAnimation,
		bool bCaptureContinuously,
		float InVerticalFramingOffset = 0.0f);
	void SetPreviewYaw(float InYawDegrees);
	void AddPreviewYaw(float DeltaYawDegrees);
	float GetPreviewYaw() const { return PreviewYawDegrees; }
	void CaptureOnce();

private:
	UPROPERTY(VisibleAnywhere)
	TObjectPtr<USceneComponent> PreviewRoot;

	UPROPERTY(VisibleAnywhere)
	TObjectPtr<USkeletalMeshComponent> PreviewMesh;

	UPROPERTY(VisibleAnywhere)
	TObjectPtr<USceneCaptureComponent2D> SceneCapture;

	UPROPERTY(VisibleAnywhere)
	TObjectPtr<UPointLightComponent> KeyLight;

	UPROPERTY(VisibleAnywhere)
	TObjectPtr<UPointLightComponent> FillLight;

	UPROPERTY(VisibleAnywhere)
	TObjectPtr<UPointLightComponent> RimLight;

	float PreviewYawDegrees = 0.0f;
};
