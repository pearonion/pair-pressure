#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "VNHMainMenuWidget.generated.h"

class UButton;
class UEditableTextBox;
class UTextBlock;
class UVNHGameInstance;

UCLASS()
class VNHSIMULATOR_API UVNHMainMenuWidget : public UUserWidget
{
	GENERATED_BODY()

public:
	virtual void NativeConstruct() override;

	UFUNCTION(BlueprintCallable, Category = "VNH|Menu")
	void RefreshStatusText();

protected:
	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional))
	TObjectPtr<UButton> HostPrivateButton;

	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional))
	TObjectPtr<UButton> HostPublicButton;

	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional))
	TObjectPtr<UButton> JoinButton;

	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional))
	TObjectPtr<UButton> QuitButton;

	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional))
	TObjectPtr<UButton> CharacterCustomizerButton;

	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional))
	TObjectPtr<UEditableTextBox> JoinAddressTextBox;

	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional))
	TObjectPtr<UTextBlock> MenuStatusText;

private:
	UFUNCTION()
	void HandleHostPrivateClicked();

	UFUNCTION()
	void HandleHostPublicClicked();

	UFUNCTION()
	void HandleJoinClicked();

	UFUNCTION()
	void HandleQuitClicked();

	UFUNCTION()
	void HandleCharacterCustomizerClicked();

	UVNHGameInstance* GetVNHGameInstance() const;
	void SetStatus(const FText& StatusText);
};
