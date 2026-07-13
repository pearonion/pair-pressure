#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "PPTeamFinishZone.generated.h"

class UBoxComponent;
class UStaticMeshComponent;
class UTextRenderComponent;

DECLARE_DYNAMIC_MULTICAST_DELEGATE_ThreeParams(FPPTeamFinished, int32, TeamId, AActor*, FirstPartner, AActor*, SecondPartner);

UCLASS(BlueprintType, Blueprintable)
class VNHSIMULATOR_API APPTeamFinishZone : public AActor
{
	GENERATED_BODY()

public:
	APPTeamFinishZone();

	UPROPERTY(BlueprintAssignable, Category = "Pair Pressure|Finish")
	FPPTeamFinished OnTeamFinished;

	UFUNCTION(BlueprintPure, Category = "Pair Pressure|Finish")
	bool HasTeamFinished(int32 TeamId) const { return FinishedTeams.Contains(TeamId); }

protected:
	UFUNCTION()
	void HandleZoneBeginOverlap(UPrimitiveComponent* OverlappedComponent, AActor* OtherActor,
		UPrimitiveComponent* OtherComponent, int32 OtherBodyIndex, bool bFromSweep, const FHitResult& SweepResult);

	UFUNCTION()
	void HandleZoneEndOverlap(UPrimitiveComponent* OverlappedComponent, AActor* OtherActor,
		UPrimitiveComponent* OtherComponent, int32 OtherBodyIndex);

private:
	void EvaluateTeamFinish(AActor* EnteringActor);
	void AwardTeamFinishScore(AActor* FirstPartner, AActor* SecondPartner) const;

	UPROPERTY(VisibleAnywhere, Category = "Pair Pressure|Finish")
	TObjectPtr<USceneComponent> SceneRoot;

	UPROPERTY(VisibleAnywhere, Category = "Pair Pressure|Finish")
	TObjectPtr<UBoxComponent> FinishVolume;

	UPROPERTY(VisibleAnywhere, Category = "Pair Pressure|Finish")
	TObjectPtr<UStaticMeshComponent> FinishPlatform;

	UPROPERTY(VisibleAnywhere, Category = "Pair Pressure|Finish")
	TObjectPtr<UTextRenderComponent> FinishLabel;

	TMap<int32, TSet<TWeakObjectPtr<AActor>>> TeamOccupants;
	TSet<int32> FinishedTeams;
};
