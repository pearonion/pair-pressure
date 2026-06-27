#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "VNHLobbyPlayButton.generated.h"

class UBoxComponent;
class UStaticMeshComponent;

UCLASS()
class VNHSIMULATOR_API AVNHLobbyPlayButton : public AActor
{
	GENERATED_BODY()

public:
	AVNHLobbyPlayButton();

	UFUNCTION(BlueprintCallable, Category = "VNH|Lobby")
	bool ActivateLobbyStart(APlayerController* RequestingPlayer);

protected:
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "VNH|Lobby")
	TObjectPtr<UStaticMeshComponent> ButtonMesh;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "VNH|Lobby")
	TObjectPtr<UBoxComponent> InteractionVolume;
};
