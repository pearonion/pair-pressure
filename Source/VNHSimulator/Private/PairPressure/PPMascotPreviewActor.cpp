#include "PairPressure/PPMascotPreviewActor.h"

#include "Animation/AnimSequence.h"
#include "Components/PointLightComponent.h"
#include "Components/SceneCaptureComponent2D.h"
#include "Components/SceneComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/TextureRenderTarget2D.h"

APPMascotPreviewActor::APPMascotPreviewActor()
{
	PrimaryActorTick.bCanEverTick = false;
	SetActorEnableCollision(false);

	PreviewRoot = CreateDefaultSubobject<USceneComponent>(TEXT("PreviewRoot"));
	SetRootComponent(PreviewRoot);

	PreviewMesh = CreateDefaultSubobject<USkeletalMeshComponent>(TEXT("PreviewMesh"));
	PreviewMesh->SetupAttachment(PreviewRoot);
	PreviewMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	PreviewMesh->SetCastShadow(false);
	PreviewMesh->bCastDynamicShadow = false;
	PreviewMesh->SetReceivesDecals(false);
	PreviewMesh->SetLightingChannels(false, false, true);
	PreviewMesh->SetRelativeLocation(FVector::ZeroVector);
	PreviewMesh->SetRelativeRotation(FRotator(0.0f, -90.0f, 0.0f));

	SceneCapture = CreateDefaultSubobject<USceneCaptureComponent2D>(TEXT("SceneCapture"));
	SceneCapture->SetupAttachment(PreviewRoot);
	SceneCapture->SetRelativeLocation(FVector(300.0f, 0.0f, 50.0f));
	SceneCapture->SetRelativeRotation(FRotator(0.0f, 180.0f, 0.0f));
	SceneCapture->FOVAngle = 36.0f;
	SceneCapture->bCaptureEveryFrame = false;
	SceneCapture->bCaptureOnMovement = false;
	SceneCapture->CaptureSource = ESceneCaptureSource::SCS_SceneColorHDR;
	SceneCapture->PrimitiveRenderMode = ESceneCapturePrimitiveRenderMode::PRM_UseShowOnlyList;
	SceneCapture->ShowOnlyComponent(PreviewMesh);
	SceneCapture->ShowFlags.SetMotionBlur(false);
	SceneCapture->ShowFlags.SetAtmosphere(false);
	SceneCapture->ShowFlags.SetCloud(false);
	SceneCapture->ShowFlags.SetFog(false);
	SceneCapture->ShowFlags.SetVolumetricFog(false);
	SceneCapture->ShowFlags.SetSkyLighting(false);
	SceneCapture->ShowFlags.SetDynamicShadows(false);
	SceneCapture->ShowFlags.SetAmbientOcclusion(false);
	SceneCapture->ShowFlags.SetReflectionEnvironment(false);
	SceneCapture->ShowFlags.SetEyeAdaptation(false);

	KeyLight = CreateDefaultSubobject<UPointLightComponent>(TEXT("KeyLight"));
	KeyLight->SetupAttachment(PreviewRoot);
	KeyLight->SetRelativeLocation(FVector(170.0f, -130.0f, 165.0f));
	KeyLight->SetIntensity(9000.0f);
	KeyLight->SetAttenuationRadius(700.0f);
	KeyLight->SetSourceRadius(80.0f);
	KeyLight->SetLightColor(FLinearColor::White);
	KeyLight->SetCastShadows(false);
	KeyLight->SetLightingChannels(false, false, true);

	FillLight = CreateDefaultSubobject<UPointLightComponent>(TEXT("FillLight"));
	FillLight->SetupAttachment(PreviewRoot);
	FillLight->SetRelativeLocation(FVector(105.0f, 155.0f, 90.0f));
	FillLight->SetIntensity(5500.0f);
	FillLight->SetAttenuationRadius(650.0f);
	FillLight->SetSourceRadius(70.0f);
	FillLight->SetLightColor(FLinearColor::White);
	FillLight->SetCastShadows(false);
	FillLight->SetLightingChannels(false, false, true);

	RimLight = CreateDefaultSubobject<UPointLightComponent>(TEXT("RimLight"));
	RimLight->SetupAttachment(PreviewRoot);
	RimLight->SetRelativeLocation(FVector(-80.0f, 0.0f, 135.0f));
	RimLight->SetIntensity(3500.0f);
	RimLight->SetAttenuationRadius(600.0f);
	RimLight->SetSourceRadius(60.0f);
	RimLight->SetLightColor(FLinearColor::White);
	RimLight->SetCastShadows(false);
	RimLight->SetLightingChannels(false, false, true);
}

void APPMascotPreviewActor::ConfigurePreview(
	USkeletalMesh* InMesh,
	UAnimSequence* InAnimation,
	UTextureRenderTarget2D* InRenderTarget,
	bool bLoopAnimation,
	bool bCaptureContinuously,
	float InVerticalFramingOffset)
{
	if (!PreviewMesh || !SceneCapture)
	{
		return;
	}

	PreviewMesh->SetSkeletalMesh(InMesh);
	PreviewMesh->bForceMipStreaming = true;
	PreviewMesh->PrestreamTextures(30.0f, true);
	PreviewMesh->PrestreamMeshLODs(30.0f);
	PreviewMesh->SetAnimationMode(EAnimationMode::AnimationSingleNode);
	PreviewMesh->SetAnimation(InAnimation);
	if (InAnimation)
	{
		PreviewMesh->Play(bLoopAnimation);
	}
	else
	{
		PreviewMesh->Stop();
	}

	PreviewMesh->UpdateBounds();
	const FBoxSphereBounds PreviewBounds = PreviewMesh->Bounds;
	const FVector LocalBoundsCenter = GetActorTransform().InverseTransformPosition(PreviewBounds.Origin);
	const float HalfFovRadians = FMath::DegreesToRadians(SceneCapture->FOVAngle * 0.5f);
	const float CameraDistance = FMath::Max(
		220.0f,
		(PreviewBounds.SphereRadius * 1.15f) / FMath::Max(FMath::Tan(HalfFovRadians), 0.01f));
	SceneCapture->SetRelativeLocation(
		FVector(
			LocalBoundsCenter.X + CameraDistance,
			LocalBoundsCenter.Y,
			LocalBoundsCenter.Z + InVerticalFramingOffset));
	SceneCapture->SetRelativeRotation(FRotator(0.0f, 180.0f, 0.0f));

	SceneCapture->TextureTarget = InRenderTarget;
	SceneCapture->bCaptureEveryFrame = bCaptureContinuously;
	SceneCapture->bAlwaysPersistRenderingState = bCaptureContinuously;
	CaptureOnce();
}

void APPMascotPreviewActor::SetPreviewYaw(float InYawDegrees)
{
	PreviewYawDegrees = FMath::UnwindDegrees(InYawDegrees);
	if (PreviewMesh)
	{
		PreviewMesh->SetRelativeRotation(FRotator(0.0f, -90.0f + PreviewYawDegrees, 0.0f));
	}
	CaptureOnce();
}

void APPMascotPreviewActor::AddPreviewYaw(float DeltaYawDegrees)
{
	SetPreviewYaw(PreviewYawDegrees + DeltaYawDegrees);
}

void APPMascotPreviewActor::CaptureOnce()
{
	if (SceneCapture && SceneCapture->TextureTarget)
	{
		SceneCapture->CaptureScene();
	}
}
