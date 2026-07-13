#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "PPRotatingHammer.generated.h"

class URotatingMovementComponent;
class UStaticMeshComponent;
class UTextRenderComponent;

UCLASS(BlueprintType, Blueprintable)
class VNHSIMULATOR_API APPRotatingHammer : public AActor
{
	GENERATED_BODY()

public:
	APPRotatingHammer();

	UFUNCTION(BlueprintCallable, Category = "Pair Pressure|Obstacle")
	void SetObstacleOwnerLabel(const FText& NewOwnerLabel);

protected:
	virtual void OnConstruction(const FTransform& Transform) override;

	UFUNCTION()
	void HandleHammerHit(UPrimitiveComponent* HitComponent, AActor* OtherActor,
		UPrimitiveComponent* OtherComponent, FVector NormalImpulse, const FHitResult& Hit);

private:
	UPROPERTY(VisibleAnywhere, Category = "Pair Pressure|Obstacle")
	TObjectPtr<USceneComponent> SceneRoot;

	UPROPERTY(VisibleAnywhere, Category = "Pair Pressure|Obstacle")
	TObjectPtr<USceneComponent> RotatingRoot;

	UPROPERTY(VisibleAnywhere, Category = "Pair Pressure|Obstacle")
	TObjectPtr<UStaticMeshComponent> BaseMesh;

	UPROPERTY(VisibleAnywhere, Category = "Pair Pressure|Obstacle")
	TObjectPtr<UStaticMeshComponent> ArmMesh;

	UPROPERTY(VisibleAnywhere, Category = "Pair Pressure|Obstacle")
	TObjectPtr<UStaticMeshComponent> HammerHeadMesh;

	UPROPERTY(VisibleAnywhere, Category = "Pair Pressure|Obstacle")
	TObjectPtr<UTextRenderComponent> OwnerLabelComponent;

	UPROPERTY(VisibleAnywhere, Category = "Pair Pressure|Obstacle")
	TObjectPtr<URotatingMovementComponent> RotatingMovement;

	UPROPERTY(EditAnywhere, Category = "Pair Pressure|Obstacle", meta = (ClampMin = "10.0", ClampMax = "240.0"))
	float DegreesPerSecond = 95.0f;

	UPROPERTY(EditAnywhere, Category = "Pair Pressure|Obstacle")
	FText ObstacleOwnerLabel = FText::FromString(TEXT("UNCLAIMED"));
};
