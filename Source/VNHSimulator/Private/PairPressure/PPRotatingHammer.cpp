#include "PairPressure/PPRotatingHammer.h"

#include "Components/StaticMeshComponent.h"
#include "Components/TextRenderComponent.h"
#include "GameFramework/RotatingMovementComponent.h"
#include "PairPressure/PPPhysicalStateComponent.h"
#include "UObject/ConstructorHelpers.h"

APPRotatingHammer::APPRotatingHammer()
{
	PrimaryActorTick.bCanEverTick = false;
	bReplicates = true;
	SetReplicateMovement(true);
	Tags.AddUnique(TEXT("PP_SpinnerObstacle"));

	SceneRoot = CreateDefaultSubobject<USceneComponent>(TEXT("SceneRoot"));
	SetRootComponent(SceneRoot);

	BaseMesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("BaseMesh"));
	BaseMesh->SetupAttachment(SceneRoot);
	BaseMesh->SetRelativeLocation(FVector(0.0f, 0.0f, 110.0f));
	BaseMesh->SetRelativeScale3D(FVector(0.75f, 0.75f, 2.2f));

	RotatingRoot = CreateDefaultSubobject<USceneComponent>(TEXT("RotatingRoot"));
	RotatingRoot->SetupAttachment(SceneRoot);
	RotatingRoot->SetRelativeLocation(FVector(0.0f, 0.0f, 235.0f));

	ArmMesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("ArmMesh"));
	ArmMesh->SetupAttachment(RotatingRoot);
	ArmMesh->SetRelativeLocation(FVector(240.0f, 0.0f, 0.0f));
	ArmMesh->SetRelativeScale3D(FVector(4.8f, 0.18f, 0.18f));
	ArmMesh->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);

	HammerHeadMesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("HammerHeadMesh"));
	HammerHeadMesh->SetupAttachment(RotatingRoot);
	HammerHeadMesh->SetRelativeLocation(FVector(480.0f, 0.0f, 0.0f));
	HammerHeadMesh->SetRelativeScale3D(FVector(0.85f, 1.45f, 1.0f));
	HammerHeadMesh->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
	HammerHeadMesh->SetNotifyRigidBodyCollision(true);
	HammerHeadMesh->OnComponentHit.AddDynamic(this, &APPRotatingHammer::HandleHammerHit);

	OwnerLabelComponent = CreateDefaultSubobject<UTextRenderComponent>(TEXT("OwnerLabel"));
	OwnerLabelComponent->SetupAttachment(SceneRoot);
	OwnerLabelComponent->SetRelativeLocation(FVector(0.0f, 0.0f, 355.0f));
	OwnerLabelComponent->SetRelativeRotation(FRotator(0.0f, 90.0f, 0.0f));
	OwnerLabelComponent->SetHorizontalAlignment(EHorizTextAligment::EHTA_Center);
	OwnerLabelComponent->SetWorldSize(34.0f);
	OwnerLabelComponent->SetTextRenderColor(FColor(255, 170, 35));

	RotatingMovement = CreateDefaultSubobject<URotatingMovementComponent>(TEXT("RotatingMovement"));
	RotatingMovement->SetUpdatedComponent(RotatingRoot);
	RotatingMovement->bRotationInLocalSpace = true;

	static ConstructorHelpers::FObjectFinder<UStaticMesh> CubeMeshFinder(TEXT("/Engine/BasicShapes/Cube.Cube"));
	static ConstructorHelpers::FObjectFinder<UStaticMesh> CylinderMeshFinder(TEXT("/Engine/BasicShapes/Cylinder.Cylinder"));
	if (CubeMeshFinder.Succeeded())
	{
		ArmMesh->SetStaticMesh(CubeMeshFinder.Object);
		HammerHeadMesh->SetStaticMesh(CubeMeshFinder.Object);
	}
	if (CylinderMeshFinder.Succeeded())
	{
		BaseMesh->SetStaticMesh(CylinderMeshFinder.Object);
	}
}

void APPRotatingHammer::SetObstacleOwnerLabel(const FText& NewOwnerLabel)
{
	ObstacleOwnerLabel = NewOwnerLabel;
	if (OwnerLabelComponent)
	{
		OwnerLabelComponent->SetText(ObstacleOwnerLabel);
	}
}

void APPRotatingHammer::OnConstruction(const FTransform& Transform)
{
	Super::OnConstruction(Transform);
	if (RotatingMovement)
	{
		RotatingMovement->RotationRate = FRotator(0.0f, DegreesPerSecond, 0.0f);
	}
	if (OwnerLabelComponent)
	{
		OwnerLabelComponent->SetText(ObstacleOwnerLabel);
	}
}

void APPRotatingHammer::HandleHammerHit(UPrimitiveComponent* HitComponent, AActor* OtherActor,
	UPrimitiveComponent* OtherComponent, FVector NormalImpulse, const FHitResult& Hit)
{
	if (!HasAuthority() || !OtherActor || OtherActor == this)
	{
		return;
	}
	if (!RotatingMovement || !RotatingMovement->IsActive()
		|| FMath::IsNearlyZero(RotatingMovement->RotationRate.Yaw))
	{
		// Contact alone is not a knockdown. This mirrors the shared impact sensor's
		// motion gate so walking into a stopped hammer remains harmless.
		return;
	}

	if (UPPPhysicalStateComponent* PhysicalState = UPPPhysicalStateComponent::FindPhysicalStateComponent(OtherActor))
	{
		FVector ImpactDirection =
			(OtherActor->GetActorLocation() - Hit.ImpactPoint).GetSafeNormal2D();
		if (ImpactDirection.IsNearlyZero())
		{
			ImpactDirection =
				(OtherActor->GetActorLocation() - GetActorLocation()).GetSafeNormal2D();
		}
		PhysicalState->RequestCourseObstacleRagdoll(
			ImpactDirection,
			FMath::Abs(DegreesPerSecond),
			this,
			HitComponent,
			Hit.ImpactPoint);
	}
}
