#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "VNHLobbyPlayButton.generated.h"

class APlayerController;
class APawn;
class UBoxComponent;
class UStaticMeshComponent;
class UTextRenderComponent;

UCLASS()
class VNHSIMULATOR_API AVNHLobbyPlayButton : public AActor
{
	GENERATED_BODY()

public:
	AVNHLobbyPlayButton();

	virtual void Tick(float DeltaSeconds) override;

	UFUNCTION(BlueprintCallable, Category = "VNH|Lobby")
	bool ActivateLobbyStart(APlayerController* RequestingPlayer);

	UFUNCTION(BlueprintPure, Category = "VNH|Lobby")
	bool IsPawnWithinInteractionRange(const APawn* Pawn) const;

	UFUNCTION(BlueprintPure, Category = "VNH|Lobby")
	float GetInteractionRadius() const { return InteractionRadius; }

protected:
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "VNH|Lobby")
	TObjectPtr<UStaticMeshComponent> ButtonMesh;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "VNH|Lobby")
	TObjectPtr<UBoxComponent> InteractionVolume;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "VNH|Lobby")
	TObjectPtr<UTextRenderComponent> PromptText;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "VNH|Lobby")
	float InteractionRadius = 340.0f;
};
