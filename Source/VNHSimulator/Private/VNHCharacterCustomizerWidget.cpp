#include "VNHCharacterCustomizerWidget.h"

#include "Blueprint/WidgetTree.h"
#include "Components/Border.h"
#include "Components/Button.h"
#include "Components/CanvasPanel.h"
#include "Components/CanvasPanelSlot.h"
#include "Components/CheckBox.h"
#include "Components/HorizontalBox.h"
#include "Components/HorizontalBoxSlot.h"
#include "Components/Image.h"
#include "Components/PanelSlot.h"
#include "Components/PanelWidget.h"
#include "Components/ScrollBox.h"
#include "Components/TextBlock.h"
#include "Components/UniformGridPanel.h"
#include "Components/UniformGridSlot.h"
#include "Components/VerticalBox.h"
#include "Components/VerticalBoxSlot.h"
#include "Engine/Font.h"
#include "Engine/Texture2D.h"
#include "Engine/TextureRenderTarget2D.h"
#include "InputCoreTypes.h"
#include "VNHCustomizationPreviewActor.h"
#include "VNHGameState.h"
#include "Kismet/KismetSystemLibrary.h"
#include "VNHGameInstance.h"
#include "VNHPlayerController.h"
#include "VNHPlayerState.h"

namespace
{
constexpr int32 ItemsPerPage = 20;

FSlateFontInfo Font(int32 Size, FName Typeface = TEXT("Bold"))
{
	FSlateFontInfo Result;
	Result.FontObject = LoadObject<UObject>(nullptr, TEXT("/Engine/EngineFonts/Roboto.Roboto"));
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
		if (bUsingDesignerWidget || BindDesignerWidgets())
		{
			bUsingDesignerWidget = true;
			ApplyModeText();
			RefreshLabels();
			RefreshLobbyCountdown();
		}
		else
		{
			Rebuild();
		}
	}
}

void UVNHCharacterCustomizerWidget::NativeConstruct()
{
	Super::NativeConstruct();
	SetIsFocusable(true);
	ActiveSlot = EVNHCustomizationSlot::Outfit;
	ActivePage = 0;
	bUsingDesignerWidget = BindDesignerWidgets();
	if (bUsingDesignerWidget)
	{
		BindDesignerEvents();
		ApplyModeText();
		RefreshLabels();
		RefreshItemGrid();
		RefreshLobbyCountdown();
	}
	else
	{
		Rebuild();
	}

	EnsurePreviewActor();
	RefreshPreviewActor();
	BindPreviewRenderTarget();
}

void UVNHCharacterCustomizerWidget::NativeDestruct()
{
	DestroyPreviewActor();
	Super::NativeDestruct();
}

void UVNHCharacterCustomizerWidget::NativeTick(const FGeometry& MyGeometry, float InDeltaTime)
{
	Super::NativeTick(MyGeometry, InDeltaTime);

	if (bLobbyMode)
	{
		RefreshLobbyCountdown();
	}
}

FReply UVNHCharacterCustomizerWidget::NativeOnMouseButtonDown(const FGeometry& InGeometry, const FPointerEvent& InMouseEvent)
{
	const FKey EffectingButton = InMouseEvent.GetEffectingButton();
	if ((EffectingButton == EKeys::RightMouseButton || EffectingButton == EKeys::LeftMouseButton) && PreviewRenderImage)
	{
		const FGeometry PreviewGeometry = PreviewRenderImage->GetCachedGeometry();
		if (PreviewGeometry.IsUnderLocation(InMouseEvent.GetScreenSpacePosition()))
		{
			bDraggingPreview = true;
			LastPreviewDragScreenPosition = InMouseEvent.GetScreenSpacePosition();
			if (PreviewActor)
			{
				PreviewActor->SetAutoRotate(false);
			}
			return FReply::Handled().CaptureMouse(TakeWidget());
		}
	}

	return Super::NativeOnMouseButtonDown(InGeometry, InMouseEvent);
}

FReply UVNHCharacterCustomizerWidget::NativeOnMouseButtonUp(const FGeometry& InGeometry, const FPointerEvent& InMouseEvent)
{
	const FKey EffectingButton = InMouseEvent.GetEffectingButton();
	if (bDraggingPreview && (EffectingButton == EKeys::RightMouseButton || EffectingButton == EKeys::LeftMouseButton))
	{
		bDraggingPreview = false;
		return FReply::Handled().ReleaseMouseCapture();
	}

	return Super::NativeOnMouseButtonUp(InGeometry, InMouseEvent);
}

FReply UVNHCharacterCustomizerWidget::NativeOnMouseMove(const FGeometry& InGeometry, const FPointerEvent& InMouseEvent)
{
	if (bDraggingPreview && PreviewActor)
	{
		const FVector2D CurrentPosition = InMouseEvent.GetScreenSpacePosition();
		const float DeltaX = CurrentPosition.X - LastPreviewDragScreenPosition.X;
		LastPreviewDragScreenPosition = CurrentPosition;
		PreviewActor->AddYawInput(DeltaX * 0.45f);
		return FReply::Handled();
	}

	return Super::NativeOnMouseMove(InGeometry, InMouseEvent);
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
		bLobbyMode ? NSLOCTEXT("VNH", "LobbyCustomizerTitle", "DRIP CHECK") : NSLOCTEXT("VNH", "MainCustomizerTitle", "CHARACTER CUSTOMIZER"),
		30,
		FLinearColor::White);
	LeftPanel->AddChild(TitleText);
	Pad(TitleText, FMargin(0.0f, 0.0f, 0.0f, 18.0f));

	AddPresetButton(LeftPanel, 0);
	AddPresetButton(LeftPanel, 1);
	AddPresetButton(LeftPanel, 2);

	UButton* RandomizeButton = MakeButton(WidgetTree, NSLOCTEXT("VNH", "CustomizerRandom", "MAKE ME UNEMPLOYABLE"), 15);
	RandomizeButton->OnClicked.AddUniqueDynamic(this, &UVNHCharacterCustomizerWidget::HandleRandomClicked);
	LeftPanel->AddChild(RandomizeButton);
	Pad(RandomizeButton, FMargin(0.0f, 10.0f, 16.0f, 10.0f));

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
	AddCategoryButton(Categories, EVNHCustomizationSlot::Pants, NSLOCTEXT("VNH", "CustomizePants", "PANTS"));
	AddCategoryButton(Categories, EVNHCustomizationSlot::Shoes, NSLOCTEXT("VNH", "CustomizeShoes", "SHOES"));
	AddCategoryButton(Categories, EVNHCustomizationSlot::Accessory, NSLOCTEXT("VNH", "CustomizeAccessory", "ACCESSORY"));

	UVerticalBox* RightPanel = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass(), TEXT("CustomizerRightPanel"));
	Layout->AddChild(RightPanel);

	PreviewText = MakeLabel(WidgetTree, FText::GetEmpty(), 24, FLinearColor::White);
	RightPanel->AddChild(PreviewText);
	Pad(PreviewText, FMargin(40.0f, 24.0f, 0.0f, 24.0f));

	PreviewRenderImage = WidgetTree->ConstructWidget<UImage>(UImage::StaticClass(), TEXT("PreviewRenderImage_Runtime"));
	PreviewRenderImage->SetDesiredSizeOverride(FVector2D(480.0f, 520.0f));
	RightPanel->AddChild(PreviewRenderImage);
	Pad(PreviewRenderImage, FMargin(40.0f, 0.0f, 0.0f, 10.0f));
	EnsurePreviewAnimationToggle();

	StatusText = MakeLabel(
		WidgetTree,
		bLobbyMode
			? NSLOCTEXT("VNH", "LobbyCustomizerStatus", "READY locks your look. The timer does not care about your fashion crisis.")
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
	RefreshLobbyCountdown();
}

bool UVNHCharacterCustomizerWidget::BindDesignerWidgets()
{
	if (!WidgetTree || !WidgetTree->FindWidget(TEXT("CustomizerRoot")))
	{
		return false;
	}

	TitleText = Cast<UTextBlock>(WidgetTree->FindWidget(TEXT("TitleText")));
	PreviewText = Cast<UTextBlock>(WidgetTree->FindWidget(TEXT("PreviewText")));
	StatusText = Cast<UTextBlock>(WidgetTree->FindWidget(TEXT("StatusText")));
	PreviewRenderImage = Cast<UImage>(WidgetTree->FindWidget(TEXT("PreviewRenderImage")));
	PreviewFemaleAnimationsCheckBox = Cast<UCheckBox>(WidgetTree->FindWidget(TEXT("PreviewFemaleAnimationsCheckBox")));
	PreviewFemaleAnimationsLabel = Cast<UTextBlock>(WidgetTree->FindWidget(TEXT("PreviewFemaleAnimationsLabel")));
	PresetOneButton = Cast<UButton>(WidgetTree->FindWidget(TEXT("PresetOneButton")));
	PresetTwoButton = Cast<UButton>(WidgetTree->FindWidget(TEXT("PresetTwoButton")));
	PresetThreeButton = Cast<UButton>(WidgetTree->FindWidget(TEXT("PresetThreeButton")));
	SavePresetButton = Cast<UButton>(WidgetTree->FindWidget(TEXT("SavePresetButton")));
	LoadPresetButton = Cast<UButton>(WidgetTree->FindWidget(TEXT("LoadPresetButton")));
	BlankCanvasButton = Cast<UButton>(WidgetTree->FindWidget(TEXT("BlankCanvasButton")));
	RandomButton = Cast<UButton>(WidgetTree->FindWidget(TEXT("RandomButton")));
	BodyButton = Cast<UButton>(WidgetTree->FindWidget(TEXT("BodyButton")));
	HairButton = Cast<UButton>(WidgetTree->FindWidget(TEXT("HairButton")));
	FaceButton = Cast<UButton>(WidgetTree->FindWidget(TEXT("FaceButton")));
	HatButton = Cast<UButton>(WidgetTree->FindWidget(TEXT("HatButton")));
	MustacheButton = Cast<UButton>(WidgetTree->FindWidget(TEXT("MustacheButton")));
	OutfitButton = Cast<UButton>(WidgetTree->FindWidget(TEXT("OutfitButton")));
	OutwearButton = Cast<UButton>(WidgetTree->FindWidget(TEXT("OutwearButton")));
	PantsButton = Cast<UButton>(WidgetTree->FindWidget(TEXT("PantsButton")));
	ShoesButton = Cast<UButton>(WidgetTree->FindWidget(TEXT("ShoesButton")));
	AccessoryButton = Cast<UButton>(WidgetTree->FindWidget(TEXT("AccessoryButton")));
	PreviousButton = Cast<UButton>(WidgetTree->FindWidget(TEXT("PreviousButton")));
	NextButton = Cast<UButton>(WidgetTree->FindWidget(TEXT("NextButton")));
	BackReadyButton = Cast<UButton>(WidgetTree->FindWidget(TEXT("BackReadyButton")));
	CloseButton = Cast<UButton>(WidgetTree->FindWidget(TEXT("CloseButton")));

	if (!PreviewRenderImage)
	{
		if (UVerticalBox* RightPanel = Cast<UVerticalBox>(WidgetTree->FindWidget(TEXT("RightPanel"))))
		{
			PreviewRenderImage = WidgetTree->ConstructWidget<UImage>(UImage::StaticClass(), TEXT("PreviewRenderImage_Runtime"));
			PreviewRenderImage->SetDesiredSizeOverride(FVector2D(480.0f, 520.0f));
			PreviewRenderImage->SetVisibility(ESlateVisibility::HitTestInvisible);
			if (UVerticalBoxSlot* PreviewSlot = RightPanel->AddChildToVerticalBox(PreviewRenderImage))
			{
				PreviewSlot->SetHorizontalAlignment(HAlign_Fill);
				PreviewSlot->SetVerticalAlignment(VAlign_Fill);
				PreviewSlot->SetPadding(FMargin(22.0f, 8.0f, 22.0f, 8.0f));
				FSlateChildSize FillSize;
				FillSize.SizeRule = ESlateSizeRule::Fill;
				FillSize.Value = 1.0f;
				PreviewSlot->SetSize(FillSize);
			}
		}
	}

	EnsurePreviewAnimationToggle();
	ItemSlotButtons.Reset();
	ItemSlotImages.Reset();
	ItemSlotLabels.Reset();
	for (int32 Index = 1; Index <= ItemsPerPage; ++Index)
	{
		UButton* Button = Cast<UButton>(WidgetTree->FindWidget(*FString::Printf(TEXT("LookItemSlot_%d"), Index)));
		UImage* Icon = Cast<UImage>(WidgetTree->FindWidget(*FString::Printf(TEXT("LookItemSlot_%d_Icon"), Index)));
		UTextBlock* Label = Cast<UTextBlock>(WidgetTree->FindWidget(*FString::Printf(TEXT("LookItemSlot_%d_Label"), Index)));
		if (Button && (!Icon || !Label))
		{
			UVerticalBox* ItemStack = WidgetTree->ConstructWidget<UVerticalBox>();
			Icon = WidgetTree->ConstructWidget<UImage>();
			Icon->SetDesiredSizeOverride(FVector2D(118.0f, 82.0f));

			Label = WidgetTree->ConstructWidget<UTextBlock>();
			Label->SetFont(Font(12));
			Label->SetColorAndOpacity(FSlateColor(FLinearColor::White));
			Label->SetJustification(ETextJustify::Center);
			Label->SetAutoWrapText(true);
			Label->SetWrapTextAt(126.0f);

			ItemStack->AddChildToVerticalBox(Icon);
			ItemStack->AddChildToVerticalBox(Label);
			Button->SetContent(ItemStack);
		}

		ItemSlotButtons.Add(Button);
		ItemSlotImages.Add(Icon);
		ItemSlotLabels.Add(Label);
	}

	EnsurePresetControlBar();
	NormalizeItemGridSlots();
	return TitleText && PreviewText && StatusText && PreviousButton && NextButton && BackReadyButton;
}

void UVNHCharacterCustomizerWidget::EnsurePreviewAnimationToggle()
{
	if (!WidgetTree)
	{
		return;
	}
	if (PreviewFemaleAnimationsCheckBox)
	{
		PreviewFemaleAnimationsCheckBox->SetIsChecked(bUseFemalePreviewAnimations);
		PreviewFemaleAnimationsCheckBox->OnCheckStateChanged.AddUniqueDynamic(this, &UVNHCharacterCustomizerWidget::HandlePreviewFemaleAnimationsChanged);
		return;
	}

	UHorizontalBox* ToggleRow = WidgetTree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass(), TEXT("PreviewAnimationToggleRow_Runtime"));
	PreviewFemaleAnimationsCheckBox = WidgetTree->ConstructWidget<UCheckBox>(UCheckBox::StaticClass(), TEXT("PreviewFemaleAnimationsCheckBox"));
	PreviewFemaleAnimationsCheckBox->SetIsChecked(bUseFemalePreviewAnimations);
	PreviewFemaleAnimationsCheckBox->OnCheckStateChanged.AddUniqueDynamic(this, &UVNHCharacterCustomizerWidget::HandlePreviewFemaleAnimationsChanged);

	PreviewFemaleAnimationsLabel = MakeLabel(
		WidgetTree,
		NSLOCTEXT("VNH", "CustomizerFemalePreviewAnimations", "FEMALE PREVIEW ANIMS"),
		13,
		FLinearColor(0.05f, 1.0f, 0.92f, 1.0f));

	if (UHorizontalBoxSlot* CheckSlot = ToggleRow->AddChildToHorizontalBox(PreviewFemaleAnimationsCheckBox))
	{
		CheckSlot->SetPadding(FMargin(0.0f, 0.0f, 8.0f, 0.0f));
		CheckSlot->SetHorizontalAlignment(HAlign_Center);
		CheckSlot->SetVerticalAlignment(VAlign_Center);
	}
	if (UHorizontalBoxSlot* LabelSlot = ToggleRow->AddChildToHorizontalBox(PreviewFemaleAnimationsLabel))
	{
		LabelSlot->SetHorizontalAlignment(HAlign_Left);
		LabelSlot->SetVerticalAlignment(VAlign_Center);
	}

	UPanelWidget* ParentPanel = nullptr;
	int32 InsertIndex = INDEX_NONE;
	if (PreviewRenderImage)
	{
		ParentPanel = PreviewRenderImage->GetParent();
		if (ParentPanel)
		{
			InsertIndex = ParentPanel->GetChildIndex(PreviewRenderImage) + 1;
		}
	}
	if (!ParentPanel)
	{
		ParentPanel = Cast<UPanelWidget>(WidgetTree->FindWidget(TEXT("RightPanel")));
	}
	if (!ParentPanel)
	{
		ParentPanel = Cast<UPanelWidget>(WidgetTree->FindWidget(TEXT("CustomizerRightPanel")));
	}

	if (ParentPanel)
	{
		UPanelSlot* AddedSlot = InsertIndex > 0
			? ParentPanel->InsertChildAt(InsertIndex, ToggleRow)
			: ParentPanel->AddChild(ToggleRow);
		if (UVerticalBoxSlot* VerticalSlot = Cast<UVerticalBoxSlot>(AddedSlot))
		{
			VerticalSlot->SetPadding(FMargin(22.0f, 0.0f, 22.0f, 10.0f));
			VerticalSlot->SetHorizontalAlignment(HAlign_Center);
			VerticalSlot->SetVerticalAlignment(VAlign_Center);
		}
	}
}

void UVNHCharacterCustomizerWidget::EnsurePresetControlBar()
{
	if (!WidgetTree)
	{
		return;
	}

	UVerticalBox* CenterPanel = Cast<UVerticalBox>(WidgetTree->FindWidget(TEXT("CenterPanel")));
	if (!CenterPanel)
	{
		return;
	}

	UHorizontalBox* PresetControlsBar = Cast<UHorizontalBox>(WidgetTree->FindWidget(TEXT("PresetControlsBar_Runtime")));
	if (!PresetControlsBar)
	{
		PresetControlsBar = WidgetTree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass(), TEXT("PresetControlsBar_Runtime"));
		if (UVerticalBoxSlot* BarSlot = CenterPanel->AddChildToVerticalBox(PresetControlsBar))
		{
			BarSlot->SetHorizontalAlignment(HAlign_Fill);
			BarSlot->SetVerticalAlignment(VAlign_Center);
			BarSlot->SetPadding(FMargin(8.0f, 8.0f, 8.0f, 0.0f));
			FSlateChildSize BarSize;
			BarSize.SizeRule = ESlateSizeRule::Automatic;
			BarSlot->SetSize(BarSize);
		}
	}

	auto AddButtonToPresetBar = [PresetControlsBar](UButton* Button, float FillValue)
	{
		if (!Button || !PresetControlsBar)
		{
			return;
		}

		Button->RemoveFromParent();
		if (UHorizontalBoxSlot* ButtonSlot = PresetControlsBar->AddChildToHorizontalBox(Button))
		{
			ButtonSlot->SetPadding(FMargin(4.0f));
			ButtonSlot->SetHorizontalAlignment(HAlign_Fill);
			ButtonSlot->SetVerticalAlignment(VAlign_Fill);
			FSlateChildSize ButtonSize;
			ButtonSize.SizeRule = ESlateSizeRule::Fill;
			ButtonSize.Value = FillValue;
			ButtonSlot->SetSize(ButtonSize);
		}
	};

	AddButtonToPresetBar(PresetOneButton, 1.0f);
	AddButtonToPresetBar(PresetTwoButton, 1.0f);
	AddButtonToPresetBar(PresetThreeButton, 1.0f);

	if (SavePresetButton)
	{
		SavePresetButton->SetVisibility(ESlateVisibility::Collapsed);
		SavePresetButton->SetIsEnabled(false);
		SavePresetButton->RemoveFromParent();
	}

	if (LoadPresetButton)
	{
		LoadPresetButton->SetVisibility(ESlateVisibility::Collapsed);
		LoadPresetButton->SetIsEnabled(false);
		LoadPresetButton->RemoveFromParent();
	}

	auto MakePresetActionButton = [this](FName WidgetName, const FText& Label)
	{
		UButton* Button = WidgetTree ? WidgetTree->ConstructWidget<UButton>(UButton::StaticClass(), WidgetName) : nullptr;
		if (!Button)
		{
			return Button;
		}

		Button->SetBackgroundColor(FLinearColor(0.03f, 0.78f, 0.72f, 1.0f));
		Button->SetContent(MakeLabel(WidgetTree, Label, 13, FLinearColor::Black));
		return Button;
	};

	if (!BlankCanvasButton)
	{
		BlankCanvasButton = MakePresetActionButton(FName(TEXT("BlankCanvasButton")), NSLOCTEXT("VNH", "CustomizerBlankCanvas", "BLANK CANVAS"));
		AddButtonToPresetBar(BlankCanvasButton, 1.2f);
	}
	else
	{
		AddButtonToPresetBar(BlankCanvasButton, 1.2f);
	}
}

void UVNHCharacterCustomizerWidget::NormalizeItemGridSlots()
{
	constexpr int32 GridColumns = 5;
	for (int32 Index = 0; Index < ItemSlotButtons.Num(); ++Index)
	{
		UButton* Button = ItemSlotButtons[Index];
		if (!Button)
		{
			continue;
		}

		if (UUniformGridSlot* GridSlot = Cast<UUniformGridSlot>(Button->Slot))
		{
			GridSlot->SetColumn(Index % GridColumns);
			GridSlot->SetRow(Index / GridColumns);
			GridSlot->SetHorizontalAlignment(HAlign_Fill);
			GridSlot->SetVerticalAlignment(VAlign_Fill);
		}
	}
}

void UVNHCharacterCustomizerWidget::BindDesignerEvents()
{
	if (PresetOneButton)
	{
		PresetOneButton->OnClicked.AddUniqueDynamic(this, &UVNHCharacterCustomizerWidget::HandlePresetOneClicked);
	}
	if (PresetTwoButton)
	{
		PresetTwoButton->OnClicked.AddUniqueDynamic(this, &UVNHCharacterCustomizerWidget::HandlePresetTwoClicked);
	}
	if (PresetThreeButton)
	{
		PresetThreeButton->OnClicked.AddUniqueDynamic(this, &UVNHCharacterCustomizerWidget::HandlePresetThreeClicked);
	}
	if (SavePresetButton)
	{
		SavePresetButton->SetVisibility(ESlateVisibility::Collapsed);
		SavePresetButton->SetIsEnabled(false);
	}
	if (LoadPresetButton)
	{
		LoadPresetButton->SetVisibility(ESlateVisibility::Collapsed);
		LoadPresetButton->SetIsEnabled(false);
	}
	if (BlankCanvasButton)
	{
		BlankCanvasButton->OnClicked.AddUniqueDynamic(this, &UVNHCharacterCustomizerWidget::HandleBlankCanvasClicked);
	}
	if (RandomButton)
	{
		RandomButton->OnClicked.AddUniqueDynamic(this, &UVNHCharacterCustomizerWidget::HandleRandomClicked);
	}
	if (BodyButton)
	{
		BodyButton->OnClicked.AddUniqueDynamic(this, &UVNHCharacterCustomizerWidget::HandleBodyClicked);
	}
	if (HairButton)
	{
		HairButton->OnClicked.AddUniqueDynamic(this, &UVNHCharacterCustomizerWidget::HandleHairClicked);
	}
	if (FaceButton)
	{
		FaceButton->OnClicked.AddUniqueDynamic(this, &UVNHCharacterCustomizerWidget::HandleFaceClicked);
	}
	if (HatButton)
	{
		HatButton->OnClicked.AddUniqueDynamic(this, &UVNHCharacterCustomizerWidget::HandleHatClicked);
	}
	if (MustacheButton)
	{
		MustacheButton->OnClicked.AddUniqueDynamic(this, &UVNHCharacterCustomizerWidget::HandleMustacheClicked);
	}
	if (OutfitButton)
	{
		OutfitButton->OnClicked.AddUniqueDynamic(this, &UVNHCharacterCustomizerWidget::HandleOutfitClicked);
	}
	if (OutwearButton)
	{
		OutwearButton->OnClicked.AddUniqueDynamic(this, &UVNHCharacterCustomizerWidget::HandleOutwearClicked);
	}
	if (PantsButton)
	{
		PantsButton->OnClicked.AddUniqueDynamic(this, &UVNHCharacterCustomizerWidget::HandlePantsClicked);
	}
	if (ShoesButton)
	{
		ShoesButton->OnClicked.AddUniqueDynamic(this, &UVNHCharacterCustomizerWidget::HandleShoesClicked);
	}
	if (AccessoryButton)
	{
		AccessoryButton->OnClicked.AddUniqueDynamic(this, &UVNHCharacterCustomizerWidget::HandleAccessoryClicked);
	}
	if (PreviousButton)
	{
		PreviousButton->OnClicked.AddUniqueDynamic(this, &UVNHCharacterCustomizerWidget::HandlePreviousClicked);
	}
	if (NextButton)
	{
		NextButton->OnClicked.AddUniqueDynamic(this, &UVNHCharacterCustomizerWidget::HandleNextClicked);
	}
	if (BackReadyButton)
	{
		BackReadyButton->OnClicked.AddUniqueDynamic(this, &UVNHCharacterCustomizerWidget::HandleBackReadyClicked);
	}
	if (CloseButton)
	{
		CloseButton->OnClicked.AddUniqueDynamic(this, &UVNHCharacterCustomizerWidget::HandleBackClicked);
	}
	if (ItemSlotButtons.IsValidIndex(0) && ItemSlotButtons[0])
	{
		ItemSlotButtons[0]->OnClicked.AddUniqueDynamic(this, &UVNHCharacterCustomizerWidget::HandleItemSlot1Clicked);
	}
	if (ItemSlotButtons.IsValidIndex(1) && ItemSlotButtons[1])
	{
		ItemSlotButtons[1]->OnClicked.AddUniqueDynamic(this, &UVNHCharacterCustomizerWidget::HandleItemSlot2Clicked);
	}
	if (ItemSlotButtons.IsValidIndex(2) && ItemSlotButtons[2])
	{
		ItemSlotButtons[2]->OnClicked.AddUniqueDynamic(this, &UVNHCharacterCustomizerWidget::HandleItemSlot3Clicked);
	}
	if (ItemSlotButtons.IsValidIndex(3) && ItemSlotButtons[3])
	{
		ItemSlotButtons[3]->OnClicked.AddUniqueDynamic(this, &UVNHCharacterCustomizerWidget::HandleItemSlot4Clicked);
	}
	if (ItemSlotButtons.IsValidIndex(4) && ItemSlotButtons[4])
	{
		ItemSlotButtons[4]->OnClicked.AddUniqueDynamic(this, &UVNHCharacterCustomizerWidget::HandleItemSlot5Clicked);
	}
	if (ItemSlotButtons.IsValidIndex(5) && ItemSlotButtons[5])
	{
		ItemSlotButtons[5]->OnClicked.AddUniqueDynamic(this, &UVNHCharacterCustomizerWidget::HandleItemSlot6Clicked);
	}
	if (ItemSlotButtons.IsValidIndex(6) && ItemSlotButtons[6])
	{
		ItemSlotButtons[6]->OnClicked.AddUniqueDynamic(this, &UVNHCharacterCustomizerWidget::HandleItemSlot7Clicked);
	}
	if (ItemSlotButtons.IsValidIndex(7) && ItemSlotButtons[7])
	{
		ItemSlotButtons[7]->OnClicked.AddUniqueDynamic(this, &UVNHCharacterCustomizerWidget::HandleItemSlot8Clicked);
	}
	if (ItemSlotButtons.IsValidIndex(8) && ItemSlotButtons[8])
	{
		ItemSlotButtons[8]->OnClicked.AddUniqueDynamic(this, &UVNHCharacterCustomizerWidget::HandleItemSlot9Clicked);
	}
	if (ItemSlotButtons.IsValidIndex(9) && ItemSlotButtons[9])
	{
		ItemSlotButtons[9]->OnClicked.AddUniqueDynamic(this, &UVNHCharacterCustomizerWidget::HandleItemSlot10Clicked);
	}
	if (ItemSlotButtons.IsValidIndex(10) && ItemSlotButtons[10])
	{
		ItemSlotButtons[10]->OnClicked.AddUniqueDynamic(this, &UVNHCharacterCustomizerWidget::HandleItemSlot11Clicked);
	}
	if (ItemSlotButtons.IsValidIndex(11) && ItemSlotButtons[11])
	{
		ItemSlotButtons[11]->OnClicked.AddUniqueDynamic(this, &UVNHCharacterCustomizerWidget::HandleItemSlot12Clicked);
	}
	if (ItemSlotButtons.IsValidIndex(12) && ItemSlotButtons[12])
	{
		ItemSlotButtons[12]->OnClicked.AddUniqueDynamic(this, &UVNHCharacterCustomizerWidget::HandleItemSlot13Clicked);
	}
	if (ItemSlotButtons.IsValidIndex(13) && ItemSlotButtons[13])
	{
		ItemSlotButtons[13]->OnClicked.AddUniqueDynamic(this, &UVNHCharacterCustomizerWidget::HandleItemSlot14Clicked);
	}
	if (ItemSlotButtons.IsValidIndex(14) && ItemSlotButtons[14])
	{
		ItemSlotButtons[14]->OnClicked.AddUniqueDynamic(this, &UVNHCharacterCustomizerWidget::HandleItemSlot15Clicked);
	}
	if (ItemSlotButtons.IsValidIndex(15) && ItemSlotButtons[15])
	{
		ItemSlotButtons[15]->OnClicked.AddUniqueDynamic(this, &UVNHCharacterCustomizerWidget::HandleItemSlot16Clicked);
	}
	if (ItemSlotButtons.IsValidIndex(16) && ItemSlotButtons[16])
	{
		ItemSlotButtons[16]->OnClicked.AddUniqueDynamic(this, &UVNHCharacterCustomizerWidget::HandleItemSlot17Clicked);
	}
	if (ItemSlotButtons.IsValidIndex(17) && ItemSlotButtons[17])
	{
		ItemSlotButtons[17]->OnClicked.AddUniqueDynamic(this, &UVNHCharacterCustomizerWidget::HandleItemSlot18Clicked);
	}
	if (ItemSlotButtons.IsValidIndex(18) && ItemSlotButtons[18])
	{
		ItemSlotButtons[18]->OnClicked.AddUniqueDynamic(this, &UVNHCharacterCustomizerWidget::HandleItemSlot19Clicked);
	}
	if (ItemSlotButtons.IsValidIndex(19) && ItemSlotButtons[19])
	{
		ItemSlotButtons[19]->OnClicked.AddUniqueDynamic(this, &UVNHCharacterCustomizerWidget::HandleItemSlot20Clicked);
	}
}

void UVNHCharacterCustomizerWidget::ApplyModeText()
{
	if (TitleText)
	{
		TitleText->SetText(bLobbyMode ? NSLOCTEXT("VNH", "LobbyCustomizerTitle", "DRIP CHECK") : NSLOCTEXT("VNH", "MainCustomizerTitle", "CHARACTER CUSTOMIZER"));
	}

	if (StatusText)
	{
		StatusText->SetText(bLobbyMode
			? NSLOCTEXT("VNH", "LobbyCustomizerStatus", "READY locks your look. The timer does not care about your fashion crisis.")
			: NSLOCTEXT("VNH", "MainCustomizerStatus", "Saved instantly. Your lobby pawn previews changes when one exists."));
	}

	if (BackReadyButton)
	{
		if (UTextBlock* ButtonText = Cast<UTextBlock>(BackReadyButton->GetContent()))
		{
			ButtonText->SetText(bLobbyMode ? NSLOCTEXT("VNH", "CustomizerReady", "READY") : NSLOCTEXT("VNH", "CustomizerBack", "SAVE + BACK"));
		}
	}
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
	case EVNHCustomizationSlot::Pants:
		Button->OnClicked.AddUniqueDynamic(this, &UVNHCharacterCustomizerWidget::HandlePantsClicked);
		break;
	case EVNHCustomizationSlot::Shoes:
		Button->OnClicked.AddUniqueDynamic(this, &UVNHCharacterCustomizerWidget::HandleShoesClicked);
		break;
	case EVNHCustomizationSlot::Accessory:
		Button->OnClicked.AddUniqueDynamic(this, &UVNHCharacterCustomizerWidget::HandleAccessoryClicked);
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
		PresetOneButton = Button;
		Button->OnClicked.AddUniqueDynamic(this, &UVNHCharacterCustomizerWidget::HandlePresetOneClicked);
	}
	else if (PresetIndex == 1)
	{
		PresetTwoButton = Button;
		Button->OnClicked.AddUniqueDynamic(this, &UVNHCharacterCustomizerWidget::HandlePresetTwoClicked);
	}
	else
	{
		PresetThreeButton = Button;
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
	RefreshPresetButtonStyles();

	if (TitleText)
	{
		switch (ActiveSlot)
		{
		case EVNHCustomizationSlot::Body:
			TitleText->SetText(NSLOCTEXT("VNH", "CustomizerSlotBody", "BODY COLOR"));
			break;
		case EVNHCustomizationSlot::Hair:
			TitleText->SetText(NSLOCTEXT("VNH", "CustomizerSlotHair", "HEAD"));
			break;
		case EVNHCustomizationSlot::Face:
			TitleText->SetText(NSLOCTEXT("VNH", "CustomizerSlotFace", "FACE"));
			break;
		case EVNHCustomizationSlot::Hat:
			TitleText->SetText(NSLOCTEXT("VNH", "CustomizerSlotHat", "HAT"));
			break;
		case EVNHCustomizationSlot::Mustache:
			TitleText->SetText(NSLOCTEXT("VNH", "CustomizerSlotMustache", "STACHE"));
			break;
		case EVNHCustomizationSlot::Outfit:
			TitleText->SetText(NSLOCTEXT("VNH", "CustomizerSlotOutfit", "SHIRT"));
			break;
		case EVNHCustomizationSlot::Outwear:
			TitleText->SetText(NSLOCTEXT("VNH", "CustomizerSlotOutwear", "OUTERWEAR"));
			break;
		case EVNHCustomizationSlot::Pants:
			TitleText->SetText(NSLOCTEXT("VNH", "CustomizerSlotPants", "PANTS"));
			break;
		case EVNHCustomizationSlot::Shoes:
			TitleText->SetText(NSLOCTEXT("VNH", "CustomizerSlotShoes", "SHOES"));
			break;
		case EVNHCustomizationSlot::Accessory:
			TitleText->SetText(NSLOCTEXT("VNH", "CustomizerSlotAccessory", "PROPS"));
			break;
		default:
			break;
		}
	}

	RefreshItemGrid();
}

void UVNHCharacterCustomizerWidget::RefreshPresetButtonStyles()
{
	UVNHGameInstance* VNHGameInstance = GetVNHGameInstance();
	const int32 ActivePresetIndex = VNHGameInstance ? VNHGameInstance->GetActiveCustomization().PresetIndex : 0;
	auto ApplyPresetStyle = [ActivePresetIndex](UButton* Button, int32 PresetIndex)
	{
		if (!Button)
		{
			return;
		}

		const bool bSelected = ActivePresetIndex == PresetIndex;
		Button->SetBackgroundColor(bSelected
			? FLinearColor(0.0f, 0.92f, 0.82f, 1.0f)
			: FLinearColor(0.04f, 0.18f, 0.20f, 0.92f));
		if (UTextBlock* ButtonText = Cast<UTextBlock>(Button->GetContent()))
		{
			ButtonText->SetColorAndOpacity(FSlateColor(bSelected ? FLinearColor::Black : FLinearColor::White));
		}
	};

	ApplyPresetStyle(PresetOneButton, 0);
	ApplyPresetStyle(PresetTwoButton, 1);
	ApplyPresetStyle(PresetThreeButton, 2);
}

void UVNHCharacterCustomizerWidget::RefreshItemGrid()
{
	UVNHGameInstance* VNHGameInstance = GetVNHGameInstance();
	if (!VNHGameInstance)
	{
		return;
	}

	const int32 OptionCount = VNHGameInstance->GetCustomizationSlotOptionCount(ActiveSlot);
	const int32 PageCount = FMath::Max(1, FMath::DivideAndRoundUp(OptionCount, ItemsPerPage));
	ActivePage = FMath::Clamp(ActivePage, 0, PageCount - 1);

	if (PreviewText)
	{
		PreviewText->SetText(FText::FromString(FString::Printf(TEXT("%d / %d"), ActivePage + 1, PageCount)));
	}

	const bool bMultiplePages = PageCount > 1;
	if (PreviousButton)
	{
		PreviousButton->SetVisibility(bMultiplePages ? ESlateVisibility::Visible : ESlateVisibility::Collapsed);
		PreviousButton->SetIsEnabled(bMultiplePages);
	}
	if (NextButton)
	{
		NextButton->SetVisibility(bMultiplePages ? ESlateVisibility::Visible : ESlateVisibility::Collapsed);
		NextButton->SetIsEnabled(bMultiplePages);
	}

	for (int32 LocalIndex = 0; LocalIndex < ItemSlotButtons.Num(); ++LocalIndex)
	{
		UButton* Button = ItemSlotButtons[LocalIndex];
		if (!Button)
		{
			continue;
		}

		const int32 OptionIndex = (ActivePage * ItemsPerPage) + LocalIndex;
		const bool bHasOption = OptionIndex < OptionCount;
		Button->SetVisibility(bHasOption ? ESlateVisibility::Visible : ESlateVisibility::Collapsed);
		Button->SetIsEnabled(bHasOption);

		UTextBlock* Label = ItemSlotLabels.IsValidIndex(LocalIndex) ? ItemSlotLabels[LocalIndex].Get() : nullptr;
		if (!Label && WidgetTree)
		{
			Label = Cast<UTextBlock>(WidgetTree->FindWidget(*FString::Printf(TEXT("LookItemSlot_%d_Label"), LocalIndex + 1)));
		}
		if (Label)
		{
			Label->SetFont(Font(15));
			Label->SetColorAndOpacity(FSlateColor(FLinearColor::White));
			Label->SetJustification(ETextJustify::Center);
			Label->SetAutoWrapText(true);
			Label->SetWrapTextAt(190.0f);
			Label->SetVisibility(ESlateVisibility::HitTestInvisible);
			Label->SetText(bHasOption
				? FText::FromString(VNHGameInstance->GetCustomizationSlotOptionLabel(ActiveSlot, OptionIndex))
				: FText::GetEmpty());
		}

		UImage* Icon = ItemSlotImages.IsValidIndex(LocalIndex) ? ItemSlotImages[LocalIndex].Get() : nullptr;
		if (!Icon && WidgetTree)
		{
			Icon = Cast<UImage>(WidgetTree->FindWidget(*FString::Printf(TEXT("LookItemSlot_%d_Icon"), LocalIndex + 1)));
		}
		if (Icon)
		{
			TSoftObjectPtr<UTexture2D> IconAsset;
			if (bHasOption)
			{
				IconAsset = VNHGameInstance->GetCustomizationSlotOptionIcon(ActiveSlot, OptionIndex);
			}

			if (!IconAsset.IsNull())
			{
				if (UTexture2D* Texture = IconAsset.LoadSynchronous())
				{
					Icon->SetBrushFromTexture(Texture, true);
					Icon->SetVisibility(ESlateVisibility::HitTestInvisible);
					Icon->SetRenderOpacity(1.0f);
				}
			}
			else
			{
				Icon->SetVisibility(ESlateVisibility::Collapsed);
				Icon->SetRenderOpacity(0.0f);
			}
		}
	}
}

void UVNHCharacterCustomizerWidget::RefreshLobbyCountdown()
{
	if (!bLobbyMode)
	{
		return;
	}

	const AVNHGameState* VNHGameState = GetWorld() ? GetWorld()->GetGameState<AVNHGameState>() : nullptr;
	const float PhaseEndsAt = VNHGameState ? VNHGameState->GetPhaseEndsAtServerTime() : 0.0f;
	const float RemainingSeconds = PhaseEndsAt > 0.0f
		? FMath::Max(0.0f, PhaseEndsAt - VNHGameState->GetServerWorldTimeSeconds())
		: 0.0f;
	const int32 DisplaySeconds = FMath::CeilToInt(RemainingSeconds);
	int32 ReadyPlayers = 0;
	int32 TotalPlayers = 0;
	if (VNHGameState)
	{
		for (APlayerState* PlayerState : VNHGameState->PlayerArray)
		{
			if (const AVNHPlayerState* VNHPlayerState = Cast<AVNHPlayerState>(PlayerState))
			{
				++TotalPlayers;
				ReadyPlayers += VNHPlayerState->IsPreRoundReady() ? 1 : 0;
			}
		}
	}

	if (StatusText)
	{
		const FString Status = DisplaySeconds <= 5
			? FString::Printf(TEXT("FINAL FIT PANIC // %02d // READY %d/%d"), DisplaySeconds, ReadyPlayers, TotalPlayers)
			: FString::Printf(TEXT("READY locks your look // %02d // READY %d/%d"), DisplaySeconds, ReadyPlayers, TotalPlayers);
		StatusText->SetText(FText::FromString(Status));
	}
}

void UVNHCharacterCustomizerWidget::EnsurePreviewActor()
{
	if (PreviewActor || !GetWorld())
	{
		return;
	}

	FActorSpawnParameters SpawnParameters;
	SpawnParameters.Owner = GetOwningPlayer();
	SpawnParameters.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	SpawnParameters.ObjectFlags |= RF_Transient;

	PreviewActor = GetWorld()->SpawnActor<AVNHCustomizationPreviewActor>(
		AVNHCustomizationPreviewActor::StaticClass(),
		FVector(0.0f, 0.0f, -50000.0f),
		FRotator::ZeroRotator,
		SpawnParameters);
	if (PreviewActor)
	{
		PreviewActor->SetUseFemalePreviewAnimations(bUseFemalePreviewAnimations);
	}
}

void UVNHCharacterCustomizerWidget::DestroyPreviewActor()
{
	if (PreviewActor)
	{
		PreviewActor->Destroy();
		PreviewActor = nullptr;
	}
}

void UVNHCharacterCustomizerWidget::RefreshPreviewActor()
{
	EnsurePreviewActor();
	if (PreviewActor)
	{
		PreviewActor->SetUseFemalePreviewAnimations(bUseFemalePreviewAnimations);
		if (UVNHGameInstance* VNHGameInstance = GetVNHGameInstance())
		{
			PreviewActor->ApplyCustomization(VNHGameInstance->GetActiveCustomization());
		}
	}
}

void UVNHCharacterCustomizerWidget::BindPreviewRenderTarget()
{
	if (!PreviewRenderImage || !PreviewActor || !PreviewActor->GetOrCreateRenderTarget())
	{
		return;
	}

	FSlateBrush PreviewBrush = PreviewRenderImage->GetBrush();
	PreviewBrush.SetResourceObject(PreviewActor->GetOrCreateRenderTarget());
	PreviewBrush.ImageSize = FVector2D(768.0f, 1184.0f);
	PreviewRenderImage->SetBrush(PreviewBrush);
	PreviewRenderImage->SetVisibility(ESlateVisibility::Visible);
	PreviewRenderImage->SetRenderOpacity(1.0f);

	RefreshPreviewActor();
}

void UVNHCharacterCustomizerWidget::ApplyAndPreview(int32 Direction)
{
	if (UVNHGameInstance* VNHGameInstance = GetVNHGameInstance())
	{
		const int32 OptionCount = VNHGameInstance->GetCustomizationSlotOptionCount(ActiveSlot);
		const int32 PageCount = FMath::Max(1, FMath::DivideAndRoundUp(OptionCount, ItemsPerPage));
		if (PageCount <= 1)
		{
			ActivePage = 0;
			RefreshLabels();
			return;
		}

		ActivePage = (ActivePage + Direction + PageCount) % PageCount;
	}
	else
	{
		ActivePage += Direction;
	}
	RefreshLabels();
}

void UVNHCharacterCustomizerWidget::SelectCategory(EVNHCustomizationSlot NewActiveSlot)
{
	ActiveSlot = NewActiveSlot;
	ActivePage = 0;
	RefreshLabels();
}

void UVNHCharacterCustomizerWidget::HandleItemSlotClicked(int32 LocalSlotIndex)
{
	bool bRemovingItem = false;
	if (UVNHGameInstance* VNHGameInstance = GetVNHGameInstance())
	{
		const int32 OptionIndex = (ActivePage * ItemsPerPage) + LocalSlotIndex;
		bRemovingItem = VNHGameInstance->IsCustomizationSlotOptionEmpty(ActiveSlot, OptionIndex);
		VNHGameInstance->SelectCustomizationSlotOption(ActiveSlot, OptionIndex);
	}
	RefreshPreviewActor();
	PlayPreviewChangeAnimation(ActiveSlot, bRemovingItem);
	RefreshLabels();
}

void UVNHCharacterCustomizerWidget::PlayPreviewChangeAnimation(EVNHCustomizationSlot CustomizationSlot, bool bRemovingItem)
{
	if (PreviewActor)
	{
		PreviewActor->PlaySlotChangeAnimation(CustomizationSlot, bRemovingItem);
	}
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
	if (AVNHPlayerController* VNHPlayerController = Cast<AVNHPlayerController>(GetOwningPlayer()))
	{
		VNHPlayerController->RequestPreRoundCustomizationReady();
	}

	if (UVNHGameInstance* VNHGameInstance = GetVNHGameInstance())
	{
		VNHGameInstance->HideCharacterCustomizer();
	}
	RemoveFromParent();
}

void UVNHCharacterCustomizerWidget::HandleBackReadyClicked()
{
	if (bLobbyMode)
	{
		HandleReadyClicked();
	}
	else
	{
		HandleBackClicked();
	}
}

void UVNHCharacterCustomizerWidget::HandleRandomClicked()
{
	if (UVNHGameInstance* VNHGameInstance = GetVNHGameInstance())
	{
		VNHGameInstance->RandomizeActiveCustomization();
	}
	RefreshPreviewActor();
	PlayPreviewChangeAnimation(EVNHCustomizationSlot::Outfit, false);
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
	SelectCategory(EVNHCustomizationSlot::Body);
}

void UVNHCharacterCustomizerWidget::HandleHairClicked()
{
	SelectCategory(EVNHCustomizationSlot::Hair);
}

void UVNHCharacterCustomizerWidget::HandleFaceClicked()
{
	SelectCategory(EVNHCustomizationSlot::Face);
}

void UVNHCharacterCustomizerWidget::HandleHatClicked()
{
	SelectCategory(EVNHCustomizationSlot::Hat);
}

void UVNHCharacterCustomizerWidget::HandleMustacheClicked()
{
	SelectCategory(EVNHCustomizationSlot::Mustache);
}

void UVNHCharacterCustomizerWidget::HandleOutfitClicked()
{
	SelectCategory(EVNHCustomizationSlot::Outfit);
}

void UVNHCharacterCustomizerWidget::HandleOutwearClicked()
{
	SelectCategory(EVNHCustomizationSlot::Outwear);
}

void UVNHCharacterCustomizerWidget::HandlePantsClicked()
{
	SelectCategory(EVNHCustomizationSlot::Pants);
}

void UVNHCharacterCustomizerWidget::HandleShoesClicked()
{
	SelectCategory(EVNHCustomizationSlot::Shoes);
}

void UVNHCharacterCustomizerWidget::HandleAccessoryClicked()
{
	SelectCategory(EVNHCustomizationSlot::Accessory);
}

void UVNHCharacterCustomizerWidget::HandleItemSlot1Clicked()
{
	HandleItemSlotClicked(0);
}

void UVNHCharacterCustomizerWidget::HandleItemSlot2Clicked()
{
	HandleItemSlotClicked(1);
}

void UVNHCharacterCustomizerWidget::HandleItemSlot3Clicked()
{
	HandleItemSlotClicked(2);
}

void UVNHCharacterCustomizerWidget::HandleItemSlot4Clicked()
{
	HandleItemSlotClicked(3);
}

void UVNHCharacterCustomizerWidget::HandleItemSlot5Clicked()
{
	HandleItemSlotClicked(4);
}

void UVNHCharacterCustomizerWidget::HandleItemSlot6Clicked()
{
	HandleItemSlotClicked(5);
}

void UVNHCharacterCustomizerWidget::HandleItemSlot7Clicked()
{
	HandleItemSlotClicked(6);
}

void UVNHCharacterCustomizerWidget::HandleItemSlot8Clicked()
{
	HandleItemSlotClicked(7);
}

void UVNHCharacterCustomizerWidget::HandleItemSlot9Clicked()
{
	HandleItemSlotClicked(8);
}

void UVNHCharacterCustomizerWidget::HandleItemSlot10Clicked()
{
	HandleItemSlotClicked(9);
}

void UVNHCharacterCustomizerWidget::HandleItemSlot11Clicked()
{
	HandleItemSlotClicked(10);
}

void UVNHCharacterCustomizerWidget::HandleItemSlot12Clicked()
{
	HandleItemSlotClicked(11);
}

void UVNHCharacterCustomizerWidget::HandleItemSlot13Clicked()
{
	HandleItemSlotClicked(12);
}

void UVNHCharacterCustomizerWidget::HandleItemSlot14Clicked()
{
	HandleItemSlotClicked(13);
}

void UVNHCharacterCustomizerWidget::HandleItemSlot15Clicked()
{
	HandleItemSlotClicked(14);
}

void UVNHCharacterCustomizerWidget::HandleItemSlot16Clicked()
{
	HandleItemSlotClicked(15);
}

void UVNHCharacterCustomizerWidget::HandleItemSlot17Clicked()
{
	HandleItemSlotClicked(16);
}

void UVNHCharacterCustomizerWidget::HandleItemSlot18Clicked()
{
	HandleItemSlotClicked(17);
}

void UVNHCharacterCustomizerWidget::HandleItemSlot19Clicked()
{
	HandleItemSlotClicked(18);
}

void UVNHCharacterCustomizerWidget::HandleItemSlot20Clicked()
{
	HandleItemSlotClicked(19);
}

void UVNHCharacterCustomizerWidget::HandlePresetOneClicked()
{
	if (UVNHGameInstance* VNHGameInstance = GetVNHGameInstance())
	{
		VNHGameInstance->SelectCharacterPreset(0);
	}
	ActivePage = 0;
	RefreshPreviewActor();
	RefreshLabels();
}

void UVNHCharacterCustomizerWidget::HandlePresetTwoClicked()
{
	if (UVNHGameInstance* VNHGameInstance = GetVNHGameInstance())
	{
		VNHGameInstance->SelectCharacterPreset(1);
	}
	ActivePage = 0;
	RefreshPreviewActor();
	RefreshLabels();
}

void UVNHCharacterCustomizerWidget::HandlePresetThreeClicked()
{
	if (UVNHGameInstance* VNHGameInstance = GetVNHGameInstance())
	{
		VNHGameInstance->SelectCharacterPreset(2);
	}
	ActivePage = 0;
	RefreshPreviewActor();
	RefreshLabels();
}

void UVNHCharacterCustomizerWidget::HandleSavePresetClicked()
{
	if (UVNHGameInstance* VNHGameInstance = GetVNHGameInstance())
	{
		VNHGameInstance->SaveActiveCharacterPreset();
	}
	if (StatusText)
	{
		StatusText->SetText(NSLOCTEXT("VNH", "CustomizerPresetSaved", "PRESET SAVED // your current fit is locked into this preset."));
	}
	RefreshPreviewActor();
	RefreshLabels();
}

void UVNHCharacterCustomizerWidget::HandleLoadPresetClicked()
{
	if (UVNHGameInstance* VNHGameInstance = GetVNHGameInstance())
	{
		VNHGameInstance->LoadActiveCharacterPreset();
	}
	if (StatusText)
	{
		StatusText->SetText(NSLOCTEXT("VNH", "CustomizerPresetLoaded", "PRESET LOADED // previous decisions have consequences."));
	}
	ActivePage = 0;
	RefreshPreviewActor();
	RefreshLabels();
}

void UVNHCharacterCustomizerWidget::HandleBlankCanvasClicked()
{
	if (UVNHGameInstance* VNHGameInstance = GetVNHGameInstance())
	{
		VNHGameInstance->ClearActiveCustomizationCosmetics();
	}
	if (StatusText)
	{
		StatusText->SetText(NSLOCTEXT("VNH", "CustomizerBlankCanvasApplied", "BLANK CANVAS // base body only."));
	}
	ActivePage = 0;
	RefreshPreviewActor();
	PlayPreviewChangeAnimation(EVNHCustomizationSlot::Outfit, true);
	RefreshLabels();
}

void UVNHCharacterCustomizerWidget::HandlePreviewFemaleAnimationsChanged(bool bIsChecked)
{
	bUseFemalePreviewAnimations = bIsChecked;
	if (PreviewActor)
	{
		PreviewActor->SetUseFemalePreviewAnimations(bUseFemalePreviewAnimations);
	}
}
