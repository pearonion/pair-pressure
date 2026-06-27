#pragma once

#include "CoreMinimal.h"
#include "GameFramework/HUD.h"
#include "VNHDebugHUD.generated.h"

class UFont;
class USoundBase;

UCLASS()
class VNHSIMULATOR_API AVNHDebugHUD : public AHUD
{
	GENERATED_BODY()

public:
	AVNHDebugHUD();

	virtual void DrawHUD() override;
	virtual void NotifyHitBoxClick(FName BoxName) override;
	virtual void NotifyHitBoxRelease(FName BoxName) override;
	virtual void NotifyHitBoxBeginCursorOver(FName BoxName) override;
	virtual void NotifyHitBoxEndCursorOver(FName BoxName) override;

	UFUNCTION(BlueprintCallable, Category = "VNH|Debug")
	void SetDebugPanelVisible(bool bNewVisible);

	UFUNCTION(BlueprintPure, Category = "VNH|Debug")
	bool IsDebugPanelVisible() const { return bDebugPanelVisible; }

	UFUNCTION(BlueprintCallable, Category = "VNH|Debug")
	void ToggleDebugPanel();

private:
	struct FDebugButton
	{
		FName Id;
		FString Label;
		FString Command;
	};

	bool bDebugPanelVisible = false;
	bool bDrawLegacyCanvasHud = false;

	UPROPERTY()
	TObjectPtr<UFont> DebugFont;

	UPROPERTY()
	TObjectPtr<USoundBase> HoverSound;

	UPROPERTY()
	TObjectPtr<USoundBase> ClickSound;

	FName HoveredButtonId;
	FName PressedButtonId;
	float LastClickTimeSeconds = -100.0f;
	TMap<FName, float> ButtonHoverAmounts;

	TArray<FDebugButton> BuildButtons() const;
	FString BuildRoundStatusText() const;
	void DrawDebugText(const FString& Text, const FLinearColor& Color, float X, float Y, float Scale = 1.0f);
	void DrawPolishedButton(const FDebugButton& Button, const FVector2D& Position, const FVector2D& Size, float DeltaSeconds);
	void DrawInteractionPanel();
	void PlayUISound(USoundBase* Sound, float VolumeMultiplier = 1.0f) const;
};
