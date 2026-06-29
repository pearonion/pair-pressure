#include "VNHCharacterCustomizerWidget.h"

#include "Blueprint/WidgetTree.h"
#include "Components/Border.h"
#include "Components/Button.h"
#include "Components/CanvasPanel.h"
#include "Components/CanvasPanelSlot.h"
#include "Components/HorizontalBox.h"
#include "Components/ScrollBox.h"
#include "Components/TextBlock.h"
#include "Components/UniformGridPanel.h"
#include "Components/UniformGridSlot.h"
#include "Components/VerticalBox.h"
#include "Components/VerticalBoxSlot.h"
#include "Kismet/KismetSystemLibrary.h"
#include "VNHGameInstance.h"
#include "VNHPlayerController.h"

namespace
{
FSlateFontInfo Font(int32 Size, FName Typeface = TEXT("Bold"))
{
	FSlateFontInfo Result;
	Result.Size = Size;
	Result.TypefaceFontName = Typeface;
	return Result;
}

UTextBlock* MakeLabel(UWidgetTree* WidgetTree, const FText& Text, int32 Size, const FLinearColor& Color)
{
	UTextBlock* Label = WidgetTree->ConstructWidget<UTextBlock>();
	Label->SetText(Text);
	Label->SetFont(Font(Size));
	Label->SetColorAndOpacity(FSlateColor(Color));
	Label->SetJustification(ETextJustify::Center);
	Label->SetAutoWrapText(true);
	return Label;
}

UButton* MakeButton(UWidgetTree* WidgetTree, const FText& Text, int32 Size = 18)
{
	UButton* Button = WidgetTree->ConstructWidget<UButton>();
	Button->SetBackgroundColor(FLinearColor(0.93f, 0.05f, 0.38f, 1.0f));
	Button->SetContent(MakeLabel(WidgetTree, Text, Size, FLinearColor::White));
	return Button;
}

void Pad(UWidget* Widget, const FMargin& Padding)
{
	if (UVerticalBoxSlot* Slot = Cast<UVerticalBoxSlot>(Widget->Slot))
	{
		Slot->SetPadding(Padding);
	}
}
}

void UVNHCharacterCustomizerWidget::SetLobbyMode(bool bInLobbyMode)
{
	bLobbyMode = bInLobbyMode;
	if (WidgetTree && WidgetTree->RootWidget)
	{
		Rebuild();
	}
}

void UVNHCharacterCustomizerWidget::NativeConstruct()
{
	Super::NativeConstruct();
	Rebuild();
}

void UVNHCharacterCustomizerWidget::Rebuild()
{
	if (!WidgetTree)
	{
		return;
	}

	WidgetTree->RootWidget = nullptr;

	UCanvasPanel* Root = WidgetTree->ConstructWidget<UCanvasPanel>(UCanvasPanel::StaticClass(), TEXT("CustomizerRoot"));
	WidgetTree->RootWidget = Root;

	UBorder* Backdrop = WidgetTree->ConstructWidget<UBorder>(UBorder::StaticClass(), TEXT("CustomizerBackdrop"));
	Backdrop->SetBrushColor(FLinearColor(0.02f, 0.52f, 0.72f, bLobbyMode ? 0.92f : 0.98f));
	Root->AddChild(Backdrop);
	if (UCanvasPanelSlot* BackdropSlot = Cast<UCanvasPanelSlot>(Backdrop->Slot))
	{
		BackdropSlot->SetAnchors(FAnchors(0.0f, 0.0f, 1.0f, 1.0f));
		BackdropSlot->SetOffsets(FMargin(0.0f));
	}

	UHorizontalBox* Layout = WidgetTree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass(), TEXT("CustomizerLayout"));
	Root->AddChild(Layout);
	if (UCanvasPanelSlot* LayoutSlot = Cast<UCanvasPanelSlot>(Layout->Slot))
	{
		LayoutSlot->SetAnchors(FAnchors(0.0f, 0.0f, 1.0f, 1.0f));
		LayoutSlot->SetOffsets(FMargin(44.0f, 34.0f, 44.0f, 34.0f));
	}

	UVerticalBox* LeftPanel = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass(), TEXT("CustomizerLeftPanel"));
	Layout->AddChild(LeftPanel);

	TitleText = MakeLabel(
		WidgetTree,
		bLobbyMode ? NSLOCTEXT("VNH", "LobbyCustomizerTitle", "30S DRIP CHECK") : NSLOCTEXT("VNH", "MainCustomizerTitle", "CHARACTER CUSTOMIZER"),
		30,
		FLinearColor::White);
	LeftPanel->AddChild(TitleText);
	Pad(TitleText, FMargin(0.0f, 0.0f, 0.0f, 18.0f));

	AddPresetButton(LeftPanel, 0);
	AddPresetButton(LeftPanel, 1);
	AddPresetButton(LeftPanel, 2);

	UButton* RandomButton = MakeButton(WidgetTree, NSLOCTEXT("VNH", "CustomizerRandom", "MAKE ME UNEMPLOYABLE"), 15);
	RandomButton->OnClicked.AddUniqueDynamic(this, &UVNHCharacterCustomizerWidget::HandleRandomClicked);
	LeftPanel->AddChild(RandomButton);
	Pad(RandomButton, FMargin(0.0f, 10.0f, 16.0f, 10.0f));

	UScrollBox* CategoryScroll = WidgetTree->ConstructWidget<UScrollBox>(UScrollBox::StaticClass(), TEXT("CategoryScroll"));
	LeftPanel->AddChild(CategoryScroll);
	Pad(CategoryScroll, FMargin(0.0f, 4.0f, 16.0f, 4.0f));

	UVerticalBox* Categories = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass(), TEXT("CategoryButtons"));
	CategoryScroll->AddChild(Categories);
	AddCategoryButton(Categories, EVNHCustomizationSlot::Body, NSLOCTEXT("VNH", "CustomizeBody", "BODY"));
	AddCategoryButton(Categories, EVNHCustomizationSlot::Hair, NSLOCTEXT("VNH", "CustomizeHair", "HAIR"));
	AddCategoryButton(Categories, EVNHCustomizationSlot::Face, NSLOCTEXT("VNH", "CustomizeFace", "FACE / NO FACE"));
	AddCategoryButton(Categories, EVNHCustomizationSlot::Hat, NSLOCTEXT("VNH", "CustomizeHat", "HAT"));
	AddCategoryButton(Categories, EVNHCustomizationSlot::Mustache, NSLOCTEXT("VNH", "CustomizeMustache", "MUSTACHE"));
	AddCategoryButton(Categories, EVNHCustomizationSlot::Outfit, NSLOCTEXT("VNH", "CustomizeOutfit", "OUTFIT"));
	AddCategoryButton(Categories, EVNHCustomizationSlot::Outwear, NSLOCTEXT("VNH", "CustomizeOutwear", "OUTWEAR"));

	UVerticalBox* RightPanel = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass(), TEXT("CustomizerRightPanel"));
	Layout->AddChild(RightPanel);

	PreviewText = MakeLabel(WidgetTree, FText::GetEmpty(), 24, FLinearColor::White);
	RightPanel->AddChild(PreviewText);
	Pad(PreviewText, FMargin(40.0f, 24.0f, 0.0f, 24.0f));

	StatusText = MakeLabel(
		WidgetTree,
		bLobbyMode
			? NSLOCTEXT("VNH", "LobbyCustomizerStatus", "READY locks your look. Timer does not care about your fashion crisis.")
			: NSLOCTEXT("VNH", "MainCustomizerStatus", "Saved instantly. Your lobby pawn previews changes when one exists."),
		18,
		FLinearColor(1.0f, 0.95f, 0.18f, 1.0f));
	RightPanel->AddChild(StatusText);
	Pad(StatusText, FMargin(40.0f, 4.0f, 0.0f, 24.0f));

	UUniformGridPanel* ActionGrid = WidgetTree->ConstructWidget<UUniformGridPanel>(UUniformGridPanel::StaticClass(), TEXT("CustomizerActionGrid"));
	RightPanel->AddChild(ActionGrid);
	AddOptionButton(ActionGrid, NSLOCTEXT("VNH", "CustomizerPrevious", "< PREV"), -1, 0, 0);
	AddOptionButton(ActionGrid, NSLOCTEXT("VNH", "CustomizerNext", "NEXT >"), 1, 1, 0);

	UButton* BackButton = MakeButton(WidgetTree, bLobbyMode ? NSLOCTEXT("VNH", "CustomizerReady", "READY") : NSLOCTEXT("VNH", "CustomizerBack", "SAVE + BACK"), 18);
	if (bLobbyMode)
	{
		BackButton->OnClicked.AddUniqueDynamic(this, &UVNHCharacterCustomizerWidget::HandleReadyClicked);
	}
	else
	{
		BackButton->OnClicked.AddUniqueDynamic(this, &UVNHCharacterCustomizerWidget::HandleBackClicked);
	}
	RightPanel->AddChild(BackButton);
	Pad(BackButton, FMargin(40.0f, 18.0f, 0.0f, 0.0f));

	WidgetTree->ForEachWidget([](UWidget* Widget)
	{
		if (Widget)
		{
			Widget->ClearFlags(RF_Transactional);
			if (Widget->Slot)
			{
				Widget->Slot->ClearFlags(RF_Transactional);
			}
		}
	});

	RefreshLabels();
}

void UVNHCharacterCustomizerWidget::AddCategoryButton(UVerticalBox* Parent, EVNHCustomizationSlot CustomizationSlot, const FText& Label)
{
	if (!Parent || !WidgetTree)
	{
		return;
	}

	UButton* Button = MakeButton(WidgetTree, Label, 16);
	switch (CustomizationSlot)
	{
	case EVNHCustomizationSlot::Body:
		Button->OnClicked.AddUniqueDynamic(this, &UVNHCharacterCustomizerWidget::HandleBodyClicked);
		break;
	case EVNHCustomizationSlot::Hair:
		Button->OnClicked.AddUniqueDynamic(this, &UVNHCharacterCustomizerWidget::HandleHairClicked);
		break;
	case EVNHCustomizationSlot::Face:
		Button->OnClicked.AddUniqueDynamic(this, &UVNHCharacterCustomizerWidget::HandleFaceClicked);
		break;
	case EVNHCustomizationSlot::Hat:
		Button->OnClicked.AddUniqueDynamic(this, &UVNHCharacterCustomizerWidget::HandleHatClicked);
		break;
	case EVNHCustomizationSlot::Mustache:
		Button->OnClicked.AddUniqueDynamic(this, &UVNHCharacterCustomizerWidget::HandleMustacheClicked);
		break;
	case EVNHCustomizationSlot::Outfit:
		Button->OnClicked.AddUniqueDynamic(this, &UVNHCharacterCustomizerWidget::HandleOutfitClicked);
		break;
	case EVNHCustomizationSlot::Outwear:
		Button->OnClicked.AddUniqueDynamic(this, &UVNHCharacterCustomizerWidget::HandleOutwearClicked);
		break;
	default:
		break;
	}

	Parent->AddChild(Button);
	Pad(Button, FMargin(0.0f, 5.0f, 16.0f, 5.0f));
}

void UVNHCharacterCustomizerWidget::AddPresetButton(UVerticalBox* Parent, int32 PresetIndex)
{
	if (!Parent || !WidgetTree)
	{
		return;
	}

	UButton* Button = MakeButton(WidgetTree, FText::FromString(FString::Printf(TEXT("PRESET %d"), PresetIndex + 1)), 16);
	if (PresetIndex == 0)
	{
		Button->OnClicked.AddUniqueDynamic(this, &UVNHCharacterCustomizerWidget::HandlePresetOneClicked);
	}
	else if (PresetIndex == 1)
	{
		Button->OnClicked.AddUniqueDynamic(this, &UVNHCharacterCustomizerWidget::HandlePresetTwoClicked);
	}
	else
	{
		Button->OnClicked.AddUniqueDynamic(this, &UVNHCharacterCustomizerWidget::HandlePresetThreeClicked);
	}
	Parent->AddChild(Button);
	Pad(Button, FMargin(0.0f, 4.0f, 16.0f, 4.0f));
}

void UVNHCharacterCustomizerWidget::AddOptionButton(UUniformGridPanel* Parent, const FText& Label, int32 Direction, int32 Column, int32 Row)
{
	if (!Parent || !WidgetTree)
	{
		return;
	}

	UButton* Button = MakeButton(WidgetTree, Label, 18);
	if (Direction < 0)
	{
		Button->OnClicked.AddUniqueDynamic(this, &UVNHCharacterCustomizerWidget::HandlePreviousClicked);
	}
	else
	{
		Button->OnClicked.AddUniqueDynamic(this, &UVNHCharacterCustomizerWidget::HandleNextClicked);
	}
	UUniformGridSlot* SlotWidget = Parent->AddChildToUniformGrid(Button, Row, Column);
	if (SlotWidget)
	{
		SlotWidget->SetHorizontalAlignment(HAlign_Fill);
		SlotWidget->SetVerticalAlignment(VAlign_Fill);
	}
}

void UVNHCharacterCustomizerWidget::RefreshLabels()
{
	if (PreviewText)
	{
		if (UVNHGameInstance* VNHGameInstance = GetVNHGameInstance())
		{
			PreviewText->SetText(FText::FromString(VNHGameInstance->GetActiveCustomizationSummary()));
		}
	}
}

void UVNHCharacterCustomizerWidget::ApplyAndPreview(int32 Direction)
{
	if (UVNHGameInstance* VNHGameInstance = GetVNHGameInstance())
	{
		VNHGameInstance->CycleCustomizationSlot(ActiveSlot, Direction);
	}
	RefreshLabels();
}

UVNHGameInstance* UVNHCharacterCustomizerWidget::GetVNHGameInstance() const
{
	return GetGameInstance<UVNHGameInstance>();
}

void UVNHCharacterCustomizerWidget::HandleBackClicked()
{
	if (UVNHGameInstance* VNHGameInstance = GetVNHGameInstance())
	{
		VNHGameInstance->HideCharacterCustomizer();
	}
	RemoveFromParent();
}

void UVNHCharacterCustomizerWidget::HandleReadyClicked()
{
	if (UVNHGameInstance* VNHGameInstance = GetVNHGameInstance())
	{
		VNHGameInstance->HideCharacterCustomizer();
	}
	RemoveFromParent();
}

void UVNHCharacterCustomizerWidget::HandleRandomClicked()
{
	if (UVNHGameInstance* VNHGameInstance = GetVNHGameInstance())
	{
		VNHGameInstance->RandomizeActiveCustomization();
	}
	RefreshLabels();
}

void UVNHCharacterCustomizerWidget::HandlePreviousClicked()
{
	ApplyAndPreview(-1);
}

void UVNHCharacterCustomizerWidget::HandleNextClicked()
{
	ApplyAndPreview(1);
}

void UVNHCharacterCustomizerWidget::HandleBodyClicked()
{
	ActiveSlot = EVNHCustomizationSlot::Body;
	ApplyAndPreview();
}

void UVNHCharacterCustomizerWidget::HandleHairClicked()
{
	ActiveSlot = EVNHCustomizationSlot::Hair;
	ApplyAndPreview();
}

void UVNHCharacterCustomizerWidget::HandleFaceClicked()
{
	ActiveSlot = EVNHCustomizationSlot::Face;
	ApplyAndPreview();
}

void UVNHCharacterCustomizerWidget::HandleHatClicked()
{
	ActiveSlot = EVNHCustomizationSlot::Hat;
	ApplyAndPreview();
}

void UVNHCharacterCustomizerWidget::HandleMustacheClicked()
{
	ActiveSlot = EVNHCustomizationSlot::Mustache;
	ApplyAndPreview();
}

void UVNHCharacterCustomizerWidget::HandleOutfitClicked()
{
	ActiveSlot = EVNHCustomizationSlot::Outfit;
	ApplyAndPreview();
}

void UVNHCharacterCustomizerWidget::HandleOutwearClicked()
{
	ActiveSlot = EVNHCustomizationSlot::Outwear;
	ApplyAndPreview();
}

void UVNHCharacterCustomizerWidget::HandlePresetOneClicked()
{
	if (UVNHGameInstance* VNHGameInstance = GetVNHGameInstance())
	{
		VNHGameInstance->SelectCharacterPreset(0);
	}
	RefreshLabels();
}

void UVNHCharacterCustomizerWidget::HandlePresetTwoClicked()
{
	if (UVNHGameInstance* VNHGameInstance = GetVNHGameInstance())
	{
		VNHGameInstance->SelectCharacterPreset(1);
	}
	RefreshLabels();
}

void UVNHCharacterCustomizerWidget::HandlePresetThreeClicked()
{
	if (UVNHGameInstance* VNHGameInstance = GetVNHGameInstance())
	{
		VNHGameInstance->SelectCharacterPreset(2);
	}
	RefreshLabels();
}
