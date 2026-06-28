#pragma once

#include "CoreMinimal.h"
#include "GameFramework/HUD.h"
#include "VNHGameplayTypes.h"
#include "VNHDebugHUD.generated.h"

class AVNHGameState;
class AVNHPlayerState;
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
	FString GetPhaseLabel(EVNHRoundPhase RoundPhase) const;
	FString GetQuickChatLabel(const AVNHGameState* VNHGameState) const;
	FLinearColor GetRoleAccentColor(const AVNHPlayerState* VNHPlayerState) const;
	void DrawDebugText(const FString& Text, const FLinearColor& Color, float X, float Y, float Scale = 1.0f);
	void DrawPolishedButton(const FDebugButton& Button, const FVector2D& Position, const FVector2D& Size, float DeltaSeconds);
	void DrawSection15Panel(float X, float Y, float W, float H, const FLinearColor& AccentColor, float Alpha = 0.94f);
	void DrawRolePhaseOverlay();
	void DrawLobbyHUD(const AVNHGameState* VNHGameState);
	void DrawRoleRevealScreen(const AVNHGameState* VNHGameState, const AVNHPlayerState* VNHPlayerState);
	void DrawRoleGameplayHUD(const AVNHGameState* VNHGameState, const AVNHPlayerState* VNHPlayerState);
	void DrawAccusationScreen(const AVNHGameState* VNHGameState, const AVNHPlayerState* VNHPlayerState);
	void DrawRevealScreen(const AVNHGameState* VNHGameState);
	void DrawInteractionPanel();
	void PlayUISound(USoundBase* Sound, float VolumeMultiplier = 1.0f) const;
};
