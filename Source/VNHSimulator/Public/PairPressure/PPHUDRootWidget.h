#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "PairPressure/PPGameplayTypes.h"
#include "PPHUDRootWidget.generated.h"

class UPPPhysicalStateComponent;
class UPPTeamMemberComponent;
class UTextBlock;

UCLASS(BlueprintType, Blueprintable)
class VNHSIMULATOR_API UPPHUDRootWidget : public UUserWidget
{
	GENERATED_BODY()

public:
	UFUNCTION(BlueprintCallable, Category = "Pair Pressure|UI")
	void RefreshPresentation();

	UFUNCTION(BlueprintPure, Category = "Pair Pressure|UI")
	FPPHUDSnapshot BuildHUDSnapshot() const;

protected:
	virtual void NativeConstruct() override;
	virtual void NativeDestruct() override;

	UFUNCTION(BlueprintImplementableEvent, Category = "Pair Pressure|UI")
	void OnPresentationDataChanged(const FPPHUDSnapshot& Snapshot);

	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional))
	TObjectPtr<UTextBlock> ModeLabelText;

	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional))
	TObjectPtr<UTextBlock> PartnerStateText;

	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional))
	TObjectPtr<UTextBlock> PartnerDirectionText;

	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional))
	TObjectPtr<UTextBlock> DazeSignalText;

	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional))
	TObjectPtr<UTextBlock> HeldItemText;

	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional))
	TObjectPtr<UTextBlock> HomeStatusText;

	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional))
	TObjectPtr<UTextBlock> InteractionPromptText;

private:
	UFUNCTION()
	void HandlePhysicalStateChanged(EPPPhysicalState NewState, float DazeNormalized);

	UFUNCTION()
	void HandleDazeChanged(float DazeNormalized);

	UFUNCTION()
	void HandlePartnerChanged(int32 NewTeamId, AActor* NewPartner);

	void ResolveFeatureComponents();

	UPROPERTY(Transient)
	TObjectPtr<UPPPhysicalStateComponent> PhysicalStateComponent;

	UPROPERTY(Transient)
	TObjectPtr<UPPTeamMemberComponent> TeamMemberComponent;

	FTimerHandle PresentationTimerHandle;
};
