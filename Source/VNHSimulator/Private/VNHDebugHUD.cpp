#include "VNHDebugHUD.h"

#include "CanvasItem.h"
#include "Engine/Canvas.h"
#include "Engine/Font.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/PlayerState.h"
#include "Kismet/GameplayStatics.h"
#include "Sound/SoundBase.h"
#include "UObject/ConstructorHelpers.h"
#include "VNHGameState.h"
#include "VNHPlayerController.h"
#include "VNHPlayerState.h"
#include "VNHShopperCharacter.h"

AVNHDebugHUD::AVNHDebugHUD()
{
	static ConstructorHelpers::FObjectFinder<UFont> FontFinder(TEXT("/Game/ThirdPerson/Demo/BurbankBigCondensed-Black_Font.BurbankBigCondensed-Black_Font"));
	if (FontFinder.Succeeded())
	{
		DebugFont = FontFinder.Object;
	}

	static ConstructorHelpers::FObjectFinder<USoundBase> HoverSoundFinder(TEXT("/Engine/VREditor/Sounds/VR_click1_Cue.VR_click1_Cue"));
	if (HoverSoundFinder.Succeeded())
	{
		HoverSound = HoverSoundFinder.Object;
	}

	static ConstructorHelpers::FObjectFinder<USoundBase> ClickSoundFinder(TEXT("/Engine/VREditor/Sounds/VR_confirm_Cue.VR_confirm_Cue"));
	if (ClickSoundFinder.Succeeded())
	{
		ClickSound = ClickSoundFinder.Object;
	}
}

void AVNHDebugHUD::DrawHUD()
{
	Super::DrawHUD();

	if (!Canvas)
	{
		return;
	}

	DrawRolePhaseOverlay();
	DrawTargetActionRadial();

	// The polished UMG deck owns all runtime HUD now. Keep this HUD class only
	// for input-mode/toggle compatibility and old console-button hitbox plumbing.
	if (!bDrawLegacyCanvasHud)
	{
		return;
	}

	const FString RoundStatusText = BuildRoundStatusText();
	const float StatusW = 390.0f;
	const float StatusH = 42.0f;
	const float StatusX = FMath::Max(16.0f, Canvas->ClipX - StatusW - 16.0f);
	const float StatusY = 12.0f;
	DrawRect(FLinearColor(0.015f, 0.02f, 0.035f, 0.9f), StatusX + 4.0f, StatusY + 4.0f, StatusW, StatusH);
	DrawRect(FLinearColor(0.05f, 0.09f, 0.15f, 0.95f), StatusX, StatusY, StatusW, StatusH);
	DrawRect(FLinearColor(0.0f, 0.78f, 1.0f, 1.0f), StatusX, StatusY, 6.0f, StatusH);
	DrawRect(FLinearColor(1.0f, 0.82f, 0.05f, 1.0f), StatusX + StatusW - 54.0f, StatusY, 54.0f, 4.0f);
	DrawDebugText(RoundStatusText, FLinearColor::White, StatusX + 16.0f, StatusY + 8.0f, 1.32f);
	DrawInteractionPanel();

	if (!bDebugPanelVisible)
	{
		return;
	}

	const TArray<FDebugButton> Buttons = BuildButtons();
	constexpr float PanelX = 10.0f;
	constexpr float PanelY = 44.0f;
	constexpr float ButtonW = 180.0f;
	constexpr float ButtonH = 28.0f;
	constexpr float Gap = 5.0f;
	constexpr int32 ColumnCount = 2;
	constexpr float TitleH = 34.0f;
	const int32 RowCount = FMath::CeilToInt(static_cast<float>(Buttons.Num()) / static_cast<float>(ColumnCount));
	const float PanelW = ColumnCount * ButtonW + (ColumnCount - 1) * Gap + 24.0f;
	const float PanelH = TitleH + 18.0f + RowCount * (ButtonH + Gap);

	const float Now = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.0f;
	const float DeltaSeconds = GetWorld() ? GetWorld()->GetDeltaSeconds() : 1.0f / 60.0f;

	DrawRect(FLinearColor(0.005f, 0.008f, 0.015f, 0.78f), PanelX + 7.0f, PanelY + 8.0f, PanelW, PanelH);
	DrawRect(FLinearColor(0.025f, 0.045f, 0.075f, 0.94f), PanelX, PanelY, PanelW, PanelH);
	DrawRect(FLinearColor(0.0f, 0.78f, 1.0f, 1.0f), PanelX, PanelY, 7.0f, PanelH);
	DrawRect(FLinearColor(1.0f, 0.82f, 0.05f, 1.0f), PanelX + 7.0f, PanelY, 92.0f, 5.0f);
	DrawRect(FLinearColor(0.0f, 0.78f, 1.0f, 0.45f), PanelX + PanelW - 76.0f, PanelY + PanelH - 5.0f, 76.0f, 5.0f);
	DrawDebugText(FString(TEXT("UEFN TEST DECK")), FLinearColor::White, PanelX + 16.0f, PanelY + 5.0f, 1.58f);
	DrawDebugText(FString(TEXT("POLISH ACTIVE")), FLinearColor(1.0f, 0.82f, 0.05f, 1.0f), PanelX + PanelW - 122.0f, PanelY + 10.0f, 0.92f);

	for (int32 Index = 0; Index < Buttons.Num(); ++Index)
	{
		const FDebugButton& Button = Buttons[Index];
		const int32 Column = Index % ColumnCount;
		const int32 Row = Index / ColumnCount;
		const FVector2D Position(PanelX + 12.0f + Column * (ButtonW + Gap), PanelY + TitleH + 10.0f + Row * (ButtonH + Gap));
		const FVector2D Size(ButtonW, ButtonH);
		AddHitBox(Position, Size, Button.Id, true, 0);
		DrawPolishedButton(Button, Position, Size, DeltaSeconds);
	}

	if (PressedButtonId != NAME_None && Now - LastClickTimeSeconds > 0.18f)
	{
		PressedButtonId = NAME_None;
	}
}

void AVNHDebugHUD::NotifyHitBoxClick(FName BoxName)
{
	Super::NotifyHitBoxClick(BoxName);

	APlayerController* PlayerController = GetOwningPlayerController();
	if (!PlayerController)
	{
		return;
	}

	for (const FDebugButton& Button : BuildButtons())
	{
		if (Button.Id == BoxName)
		{
			PressedButtonId = BoxName;
			LastClickTimeSeconds = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.0f;
			PlayUISound(ClickSound, 0.72f);
			PlayerController->ConsoleCommand(Button.Command, true);
			return;
		}
	}
}

void AVNHDebugHUD::NotifyHitBoxRelease(FName BoxName)
{
	Super::NotifyHitBoxRelease(BoxName);

	if (PressedButtonId == BoxName)
	{
		PressedButtonId = NAME_None;
	}
}

void AVNHDebugHUD::NotifyHitBoxBeginCursorOver(FName BoxName)
{
	Super::NotifyHitBoxBeginCursorOver(BoxName);

	if (HoveredButtonId != BoxName)
	{
		HoveredButtonId = BoxName;
		PlayUISound(HoverSound, 0.28f);
	}
}

void AVNHDebugHUD::NotifyHitBoxEndCursorOver(FName BoxName)
{
	Super::NotifyHitBoxEndCursorOver(BoxName);

	if (HoveredButtonId == BoxName)
	{
		HoveredButtonId = NAME_None;
	}
}

void AVNHDebugHUD::SetDebugPanelVisible(bool bNewVisible)
{
	bDebugPanelVisible = bNewVisible;
}

void AVNHDebugHUD::ToggleDebugPanel()
{
	bDebugPanelVisible = !bDebugPanelVisible;
}

TArray<AVNHDebugHUD::FDebugButton> AVNHDebugHUD::BuildButtons() const
{
	return {
		{TEXT("StartRound"), TEXT("Start Round"), TEXT("vnh.StartRound")},
		{TEXT("StartRoutines"), TEXT("Kick NPC Routines"), TEXT("vnh.StartRoutines")},
		{TEXT("PossessAlien0"), TEXT("Alien 0"), TEXT("vnh.PossessHuman 0 Alien")},
		{TEXT("PossessHunter1"), TEXT("Hunter Test"), TEXT("vnh.PossessHuman 1 Hunter")},
		{TEXT("Freeze"), TEXT("Freeze Test"), TEXT("vnh.TriggerTest Freeze")},
		{TEXT("LookEntrance"), TEXT("Look Here"), TEXT("vnh.TriggerTest LookHere")},
		{TEXT("ClearAisle"), TEXT("Clear Aisle"), TEXT("vnh.TriggerTest ClearAisle")},
		{TEXT("Question"), TEXT("Question Target"), TEXT("vnh.Interact")},
		{TEXT("Mark"), TEXT("Mark Target"), TEXT("vnh.MarkTarget")},
		{TEXT("FakeAccuse"), TEXT("Fake Accuse"), TEXT("vnh.FakeAccuse")},
		{TEXT("Accuse"), TEXT("Accuse Target"), TEXT("vnh.AccuseTarget")},
	};
}

FString AVNHDebugHUD::BuildRoundStatusText() const
{
	const AVNHGameState* VNHGameState = GetWorld() ? GetWorld()->GetGameState<AVNHGameState>() : nullptr;
	if (!VNHGameState)
	{
		return TEXT("Round: no VNHGameState");
	}

	const float PhaseEndsAt = VNHGameState->GetPhaseEndsAtServerTime();
	const float RemainingSeconds = PhaseEndsAt > 0.0f ? FMath::Max(0.0f, PhaseEndsAt - VNHGameState->GetServerWorldTimeSeconds()) : 0.0f;

	return FString::Printf(
		TEXT("Round %d | %s | %.0fs | Cmd %d Q %d Acc %d"),
		VNHGameState->GetRoundNumber(),
		*GetPhaseLabel(VNHGameState->GetRoundPhase()),
		RemainingSeconds,
		VNHGameState->GetTestsRemaining(),
		VNHGameState->GetDirectQuestionsRemaining(),
		VNHGameState->GetAccusationsRemaining());
}

FString AVNHDebugHUD::GetPhaseLabel(EVNHRoundPhase RoundPhase) const
{
	switch (RoundPhase)
	{
	case EVNHRoundPhase::WaitingForPlayers:
		return TEXT("Waiting");
	case EVNHRoundPhase::AssigningRoles:
		return TEXT("Role Reveal");
	case EVNHRoundPhase::AlienSetup:
		return TEXT("Alien Setup");
	case EVNHRoundPhase::AlienHeadStart:
		return TEXT("Alien Head Start");
	case EVNHRoundPhase::Investigation:
		return TEXT("Investigation");
	case EVNHRoundPhase::Accusation:
		return TEXT("Accusation");
	case EVNHRoundPhase::Reveal:
		return TEXT("Reveal");
	case EVNHRoundPhase::Resetting:
		return TEXT("Resetting");
	default:
		return TEXT("Unknown");
	}
}

FString AVNHDebugHUD::GetQuickChatLabel(const AVNHGameState* VNHGameState) const
{
	if (!VNHGameState || VNHGameState->GetLastQuickChatMessage().Serial <= 0)
	{
		return TEXT("Quick chat: Browse / Shirt / Friend / NoThanks / WrongSize");
	}

	const FVNHQuickChatMessage Message = VNHGameState->GetLastQuickChatMessage();
	const FString SpeakerName = Message.Speaker ? Message.Speaker->GetPlayerName() : TEXT("Someone");
	return FString::Printf(TEXT("%s: %s"), *SpeakerName, *Message.Text.ToString());
}

FLinearColor AVNHDebugHUD::GetRoleAccentColor(const AVNHPlayerState* VNHPlayerState) const
{
	if (!VNHPlayerState)
	{
		return FLinearColor(0.0f, 0.78f, 1.0f, 1.0f);
	}

	switch (VNHPlayerState->GetRole())
	{
	case EVNHPlayerRole::Human:
		return FLinearColor(0.30f, 0.86f, 0.58f, 1.0f);
	case EVNHPlayerRole::Alien:
		return FLinearColor(0.78f, 0.42f, 1.0f, 1.0f);
	case EVNHPlayerRole::Hunter:
		return FLinearColor(1.0f, 0.72f, 0.12f, 1.0f);
	default:
		return FLinearColor(0.0f, 0.78f, 1.0f, 1.0f);
	}
}

void AVNHDebugHUD::DrawDebugText(const FString& Text, const FLinearColor& Color, float X, float Y, float Scale)
{
	if (Canvas && DebugFont)
	{
		FCanvasTextItem TextItem(FVector2D(X, Y), FText::FromString(Text), DebugFont, Color);
		TextItem.Scale = FVector2D(Scale, Scale);
		TextItem.EnableShadow(FLinearColor(0.0f, 0.0f, 0.0f, 0.75f));
		Canvas->DrawItem(TextItem);
		return;
	}

	DrawText(Text, Color, X, Y);
}

void AVNHDebugHUD::DrawPolishedButton(const FDebugButton& Button, const FVector2D& Position, const FVector2D& Size, float DeltaSeconds)
{
	float& HoverAmount = ButtonHoverAmounts.FindOrAdd(Button.Id);
	const float TargetHover = HoveredButtonId == Button.Id ? 1.0f : 0.0f;
	HoverAmount = FMath::FInterpTo(HoverAmount, TargetHover, DeltaSeconds, 15.0f);

	const float ClickAge = (GetWorld() ? GetWorld()->GetTimeSeconds() : 0.0f) - LastClickTimeSeconds;
	const bool bPressed = PressedButtonId == Button.Id && ClickAge <= 0.18f;
	const float PressAmount = bPressed ? 1.0f - FMath::Clamp(ClickAge / 0.18f, 0.0f, 1.0f) : 0.0f;
	const float Lift = HoverAmount * 4.0f - PressAmount * 2.0f;
	const float WidthBoost = HoverAmount * 12.0f - PressAmount * 4.0f;
	const FVector2D DrawPosition(Position.X - HoverAmount * 2.0f, Position.Y - Lift);
	const FVector2D DrawSize(Size.X + WidthBoost, Size.Y);

	const FLinearColor ShadowColor(0.0f, 0.0f, 0.0f, 0.48f);
	const FLinearColor BaseColor = FLinearColor(
		FMath::Lerp(0.08f, 0.0f, HoverAmount),
		FMath::Lerp(0.12f, 0.40f, HoverAmount),
		FMath::Lerp(0.20f, 0.78f, HoverAmount),
		0.98f);
	const FLinearColor PressColor(1.0f, 0.82f, 0.05f, 1.0f);
	const FLinearColor AccentColor = FLinearColor::LerpUsingHSV(FLinearColor(0.0f, 0.78f, 1.0f, 1.0f), PressColor, PressAmount);
	const FLinearColor TextColor = FLinearColor::LerpUsingHSV(FLinearColor::White, FLinearColor(0.015f, 0.025f, 0.04f, 1.0f), PressAmount);

	DrawRect(ShadowColor, DrawPosition.X + 4.0f, DrawPosition.Y + 5.0f, DrawSize.X, DrawSize.Y);
	DrawRect(BaseColor, DrawPosition.X, DrawPosition.Y, DrawSize.X, DrawSize.Y);
	DrawRect(FLinearColor(0.01f, 0.02f, 0.04f, 0.74f), DrawPosition.X + 4.0f, DrawPosition.Y + 4.0f, DrawSize.X - 8.0f, DrawSize.Y - 8.0f);
	DrawRect(AccentColor, DrawPosition.X, DrawPosition.Y, 7.0f + HoverAmount * 4.0f, DrawSize.Y);
	DrawRect(AccentColor, DrawPosition.X, DrawPosition.Y, DrawSize.X, 2.0f + PressAmount * 3.0f);
	DrawRect(FLinearColor(1.0f, 0.82f, 0.05f, 0.9f * HoverAmount), DrawPosition.X + DrawSize.X - 34.0f, DrawPosition.Y + DrawSize.Y - 4.0f, 34.0f, 4.0f);

	DrawDebugText(Button.Label, TextColor, DrawPosition.X + 14.0f + PressAmount * 2.0f, DrawPosition.Y + 4.0f + PressAmount * 2.0f, 1.22f + HoverAmount * 0.08f);
}

void AVNHDebugHUD::DrawTargetActionRadial()
{
	AVNHPlayerController* VNHPlayerController = Cast<AVNHPlayerController>(GetOwningPlayerController());
	const AVNHShopperCharacter* Target = VNHPlayerController ? VNHPlayerController->GetTargetedShopper() : nullptr;
	if (!Canvas || !VNHPlayerController || !VNHPlayerController->IsAssignedHunter())
	{
		return;
	}

	if (!Target)
	{
		return;
	}

	FVector2D TargetScreenPosition;
	if (!VNHPlayerController->ProjectWorldLocationToScreen(Target->GetActorLocation() + FVector(0.0f, 0.0f, 72.0f), TargetScreenPosition))
	{
		return;
	}

	const FVector2D Center(
		FMath::Clamp(TargetScreenPosition.X + 210.0f, 170.0f, Canvas->ClipX - 170.0f),
		FMath::Clamp(TargetScreenPosition.Y, 150.0f, Canvas->ClipY - 150.0f));

	struct FActionBubble
	{
		const TCHAR* Key;
		const TCHAR* Label;
		const TCHAR* SubLabel;
		FVector2D Offset;
		FLinearColor Accent;
	};

	const FActionBubble Actions[] = {
		{TEXT("E"), TEXT("Ask"), TEXT("Direct Question"), FVector2D(0.0f, -86.0f), FLinearColor(0.0f, 0.78f, 1.0f, 1.0f)},
		{TEXT("R"), TEXT("Mark"), TEXT("Suspect"), FVector2D(98.0f, -8.0f), FLinearColor(1.0f, 0.82f, 0.05f, 1.0f)},
		{TEXT("F"), TEXT("Fake"), TEXT("Pressure"), FVector2D(0.0f, 78.0f), FLinearColor(0.78f, 0.42f, 1.0f, 1.0f)},
		{TEXT("X"), TEXT("Accuse"), TEXT("Commit"), FVector2D(-112.0f, -8.0f), FLinearColor(1.0f, 0.32f, 0.16f, 1.0f)}
	};

	DrawRect(FLinearColor(0.0f, 0.0f, 0.0f, 0.38f), Center.X - 56.0f, Center.Y - 56.0f, 112.0f, 112.0f);
	DrawRect(FLinearColor(0.02f, 0.03f, 0.045f, 0.88f), Center.X - 48.0f, Center.Y - 48.0f, 96.0f, 96.0f);
	DrawRect(FLinearColor(1.0f, 1.0f, 1.0f, 0.08f), Center.X - 38.0f, Center.Y - 1.0f, 76.0f, 2.0f);
	DrawRect(FLinearColor(1.0f, 1.0f, 1.0f, 0.08f), Center.X - 1.0f, Center.Y - 38.0f, 2.0f, 76.0f);
	DrawDebugText(TEXT("TARGET"), FLinearColor::White, Center.X - 34.0f, Center.Y - 13.0f, 0.78f);

	for (const FActionBubble& Action : Actions)
	{
		const FVector2D BubbleCenter = Center + Action.Offset;
		constexpr float KeySize = 52.0f;
		constexpr float LabelW = 146.0f;
		constexpr float LabelH = 42.0f;

		DrawRect(FLinearColor(0.0f, 0.0f, 0.0f, 0.5f), BubbleCenter.X - KeySize * 0.5f + 4.0f, BubbleCenter.Y - KeySize * 0.5f + 5.0f, KeySize, KeySize);
		DrawRect(FLinearColor(0.94f, 0.96f, 0.98f, 0.98f), BubbleCenter.X - KeySize * 0.5f, BubbleCenter.Y - KeySize * 0.5f, KeySize, KeySize);
		DrawRect(Action.Accent, BubbleCenter.X - KeySize * 0.5f, BubbleCenter.Y - KeySize * 0.5f, 6.0f, KeySize);
		DrawDebugText(Action.Key, FLinearColor(0.02f, 0.025f, 0.035f, 1.0f), BubbleCenter.X - 11.0f, BubbleCenter.Y - 18.0f, 1.5f);

		const float LabelX = BubbleCenter.X + 34.0f;
		const float LabelY = BubbleCenter.Y - LabelH * 0.5f;
		DrawRect(FLinearColor(0.0f, 0.0f, 0.0f, 0.42f), LabelX + 4.0f, LabelY + 5.0f, LabelW, LabelH);
		DrawRect(FLinearColor(0.02f, 0.03f, 0.045f, 0.92f), LabelX, LabelY, LabelW, LabelH);
		DrawRect(Action.Accent, LabelX, LabelY, 5.0f, LabelH);
		DrawDebugText(Action.Label, FLinearColor::White, LabelX + 12.0f, LabelY + 2.0f, 1.16f);
		DrawDebugText(Action.SubLabel, FLinearColor(0.78f, 0.88f, 1.0f, 1.0f), LabelX + 12.0f, LabelY + 23.0f, 0.72f);
	}

	DrawDebugText(TEXT("LMB Cancel"), FLinearColor(0.86f, 0.92f, 1.0f, 1.0f), Center.X - 42.0f, Center.Y + 58.0f, 0.82f);
}

void AVNHDebugHUD::DrawSection15Panel(float X, float Y, float W, float H, const FLinearColor& AccentColor, float Alpha)
{
	DrawRect(FLinearColor(0.0f, 0.0f, 0.0f, 0.56f), X + 7.0f, Y + 8.0f, W, H);
	DrawRect(FLinearColor(0.018f, 0.024f, 0.034f, Alpha), X, Y, W, H);
	DrawRect(AccentColor, X, Y, 7.0f, H);
	DrawRect(FLinearColor(1.0f, 0.82f, 0.05f, 0.95f), X + 7.0f, Y, FMath::Min(154.0f, W - 14.0f), 5.0f);
	DrawRect(FLinearColor(1.0f, 1.0f, 1.0f, 0.08f), X + 14.0f, Y + H - 2.0f, W - 28.0f, 1.0f);
}

void AVNHDebugHUD::DrawInteractionPanel()
{
	AVNHPlayerController* VNHPlayerController = Cast<AVNHPlayerController>(GetOwningPlayerController());
	if (!Canvas || !VNHPlayerController)
	{
		return;
	}

	const FString PromptText = VNHPlayerController->GetInteractionPromptText();
	if (!PromptText.IsEmpty())
	{
		const float PromptW = 420.0f;
		const float PromptH = 38.0f;
		const float PromptX = (Canvas->ClipX - PromptW) * 0.5f;
		const float PromptY = Canvas->ClipY - 96.0f;
		DrawRect(FLinearColor(0.005f, 0.008f, 0.015f, 0.62f), PromptX + 5.0f, PromptY + 5.0f, PromptW, PromptH);
		DrawRect(FLinearColor(0.05f, 0.09f, 0.15f, 0.92f), PromptX, PromptY, PromptW, PromptH);
		DrawRect(FLinearColor(1.0f, 0.82f, 0.05f, 1.0f), PromptX, PromptY, 6.0f, PromptH);
		DrawDebugText(PromptText, FLinearColor::White, PromptX + 14.0f, PromptY + 7.0f, 1.32f);
	}

	const FString InteractionText = VNHPlayerController->GetLastInteractionText();
	if (!InteractionText.IsEmpty())
	{
		const float ResponseW = FMath::Min(700.0f, Canvas->ClipX - 32.0f);
		const float ResponseH = 48.0f;
		const float ResponseX = (Canvas->ClipX - ResponseW) * 0.5f;
		const float ResponseY = Canvas->ClipY - 54.0f;
		DrawRect(FLinearColor(0.005f, 0.008f, 0.015f, 0.66f), ResponseX + 5.0f, ResponseY + 5.0f, ResponseW, ResponseH);
		DrawRect(FLinearColor(0.025f, 0.045f, 0.075f, 0.94f), ResponseX, ResponseY, ResponseW, ResponseH);
		DrawRect(FLinearColor(0.0f, 0.78f, 1.0f, 1.0f), ResponseX, ResponseY, 6.0f, ResponseH);
		DrawRect(FLinearColor(1.0f, 0.82f, 0.05f, 0.95f), ResponseX + ResponseW - 58.0f, ResponseY, 58.0f, 4.0f);
		DrawDebugText(InteractionText, FLinearColor(0.9f, 0.95f, 1.0f, 1.0f), ResponseX + 14.0f, ResponseY + 9.0f, 1.27f);
	}
}

void AVNHDebugHUD::DrawRolePhaseOverlay()
{
	const AVNHGameState* VNHGameState = GetWorld() ? GetWorld()->GetGameState<AVNHGameState>() : nullptr;
	const AVNHPlayerController* VNHPlayerController = Cast<AVNHPlayerController>(GetOwningPlayerController());
	const AVNHPlayerState* VNHPlayerState = VNHPlayerController ? VNHPlayerController->GetPlayerState<AVNHPlayerState>() : nullptr;
	if (!Canvas || !VNHGameState)
	{
		return;
	}

	const EVNHRoundPhase RoundPhase = VNHGameState->GetRoundPhase();
	const FString MapName = GetWorld() ? GetWorld()->GetMapName() : FString();
	const bool bIsLobbyMap = MapName.Contains(TEXT("Lobby"));

	if (bIsLobbyMap && RoundPhase == EVNHRoundPhase::WaitingForPlayers)
	{
		DrawLobbyHUD(VNHGameState);
		return;
	}

	switch (RoundPhase)
	{
	case EVNHRoundPhase::AssigningRoles:
		DrawRoleRevealScreen(VNHGameState, VNHPlayerState);
		return;
	case EVNHRoundPhase::AlienSetup:
	case EVNHRoundPhase::AlienHeadStart:
	case EVNHRoundPhase::Investigation:
		DrawRoleGameplayHUD(VNHGameState, VNHPlayerState);
		return;
	case EVNHRoundPhase::Accusation:
		DrawAccusationScreen(VNHGameState, VNHPlayerState);
		return;
	case EVNHRoundPhase::Reveal:
		DrawRevealScreen(VNHGameState);
		return;
	default:
		return;
	}
}

void AVNHDebugHUD::DrawLobbyHUD(const AVNHGameState* VNHGameState)
{
	const float OverlayW = FMath::Min(780.0f, Canvas->ClipX - 48.0f);
	const float OverlayH = 270.0f;
	const float OverlayX = (Canvas->ClipX - OverlayW) * 0.5f;
	const float OverlayY = FMath::Max(50.0f, Canvas->ClipY * 0.12f);
	DrawSection15Panel(OverlayX, OverlayY, OverlayW, OverlayH, FLinearColor(0.0f, 0.78f, 1.0f, 1.0f));

	DrawDebugText(TEXT("PRIVATE LOBBY"), FLinearColor::White, OverlayX + 28.0f, OverlayY + 22.0f, 2.0f);
	DrawDebugText(TEXT("Visibility: private listen session"), FLinearColor(0.72f, 0.86f, 1.0f, 1.0f), OverlayX + 28.0f, OverlayY + 72.0f, 1.1f);
	DrawDebugText(TEXT("Host must start when everyone is ready."), FLinearColor(1.0f, 0.82f, 0.05f, 1.0f), OverlayX + 28.0f, OverlayY + 102.0f, 1.18f);

	const int32 PlayerCount = VNHGameState ? VNHGameState->PlayerArray.Num() : 0;
	DrawDebugText(FString::Printf(TEXT("Players connected: %d / 3"), PlayerCount), FLinearColor(0.88f, 0.95f, 1.0f, 1.0f), OverlayX + 28.0f, OverlayY + 138.0f, 1.32f);
	for (int32 Index = 0; VNHGameState && Index < VNHGameState->PlayerArray.Num() && Index < 4; ++Index)
	{
		const APlayerState* PlayerState = VNHGameState->PlayerArray[Index];
		const FString HostSuffix = Index == 0 ? TEXT("  [HOST]") : TEXT("");
		DrawDebugText(FString::Printf(TEXT("%d. %s%s"), Index + 1, *GetNameSafe(PlayerState), *HostSuffix), FLinearColor::White, OverlayX + 330.0f, OverlayY + 72.0f + Index * 32.0f, 1.12f);
	}
}

void AVNHDebugHUD::DrawRoleRevealScreen(const AVNHGameState* VNHGameState, const AVNHPlayerState* VNHPlayerState)
{
	const FLinearColor Accent = GetRoleAccentColor(VNHPlayerState);
	const float OverlayW = FMath::Min(800.0f, Canvas->ClipX - 48.0f);
	const float OverlayH = 270.0f;
	const float OverlayX = (Canvas->ClipX - OverlayW) * 0.5f;
	const float OverlayY = FMath::Max(54.0f, Canvas->ClipY * 0.15f);
	DrawSection15Panel(OverlayX, OverlayY, OverlayW, OverlayH, Accent);

	const FString RoleText = VNHPlayerState ? VNHPlayerState->GetPrivateRoleRevealText().ToString() : TEXT("Waiting for role");
	const FString GoalText = VNHPlayerState ? VNHPlayerState->GetRoleGoalText().ToString() : TEXT("Wait for the host to start the round.");
	const FString ErrandText = VNHPlayerState ? VNHPlayerState->GetLightErrandText().ToString() : FString();
	const FString ToolsText = VNHGameState->GetHunterToolsText().ToString();
	const float RemainingSeconds = VNHGameState && VNHGameState->GetPhaseEndsAtServerTime() > 0.0f
		? FMath::Max(0.0f, VNHGameState->GetPhaseEndsAtServerTime() - VNHGameState->GetServerWorldTimeSeconds())
		: 0.0f;

	DrawDebugText(RoleText, FLinearColor::White, OverlayX + 28.0f, OverlayY + 24.0f, 2.0f);
	DrawDebugText(GoalText, FLinearColor(0.88f, 0.95f, 1.0f, 1.0f), OverlayX + 28.0f, OverlayY + 88.0f, 1.28f);

	if (!ErrandText.IsEmpty())
	{
		DrawDebugText(FString::Printf(TEXT("Cover errand: %s"), *ErrandText), FLinearColor(1.0f, 0.82f, 0.05f, 1.0f), OverlayX + 28.0f, OverlayY + 134.0f, 1.18f);
	}

	DrawDebugText(TEXT("Controls: Move / Interact / Act Natural / Quick Chat"), FLinearColor(0.70f, 0.88f, 1.0f, 1.0f), OverlayX + 28.0f, OverlayY + OverlayH - 78.0f, 1.08f);
	DrawDebugText(ToolsText, FLinearColor(0.68f, 0.88f, 1.0f, 1.0f), OverlayX + 28.0f, OverlayY + OverlayH - 48.0f, 1.08f);
	DrawDebugText(FString::Printf(TEXT("Starting in %.0fs"), RemainingSeconds), Accent, OverlayX + OverlayW - 180.0f, OverlayY + 28.0f, 1.35f);
}

void AVNHDebugHUD::DrawRoleGameplayHUD(const AVNHGameState* VNHGameState, const AVNHPlayerState* VNHPlayerState)
{
	const FLinearColor Accent = GetRoleAccentColor(VNHPlayerState);
	const float PanelW = FMath::Min(560.0f, Canvas->ClipX - 32.0f);
	const float PanelH = 138.0f;
	const float PanelX = 18.0f;
	const float PanelY = Canvas->ClipY - PanelH - 18.0f;
	DrawSection15Panel(PanelX, PanelY, PanelW, PanelH, Accent, 0.86f);

	const FString RoleName = VNHPlayerState ? VNHPlayerState->GetRoleDisplayText().ToString() : TEXT("Unassigned");
	const FString GoalText = VNHPlayerState ? VNHPlayerState->GetRoleGoalText().ToString() : TEXT("Wait for role assignment.");
	DrawDebugText(FString::Printf(TEXT("%s HUD"), *RoleName.ToUpper()), FLinearColor::White, PanelX + 18.0f, PanelY + 14.0f, 1.35f);
	DrawDebugText(GoalText, FLinearColor(0.86f, 0.94f, 1.0f, 1.0f), PanelX + 18.0f, PanelY + 48.0f, 1.0f);

	if (VNHPlayerState && VNHPlayerState->IsHunter())
	{
		DrawDebugText(VNHGameState ? VNHGameState->GetHunterToolsText().ToString() : TEXT("Commands 0/2 | Questions 0/3 | Accuse 0/1"), Accent, PanelX + 18.0f, PanelY + 82.0f, 1.05f);
		DrawDebugText(TEXT("Mark suspects, then confirm one accusation."), FLinearColor::White, PanelX + 18.0f, PanelY + 108.0f, 0.92f);
		return;
	}

	const FString ErrandText = VNHPlayerState ? VNHPlayerState->GetLightErrandText().ToString() : FString();
	if (!ErrandText.IsEmpty())
	{
		DrawDebugText(FString::Printf(TEXT("Errand: %s"), *ErrandText), Accent, PanelX + 18.0f, PanelY + 82.0f, 1.02f);
	}

	const FString QuickChat = GetQuickChatLabel(VNHGameState);
	const FString PrivateHint = VNHPlayerState && VNHPlayerState->IsAlien()
		? TEXT("Private thought: move weird, then recover before anyone notices.")
		: TEXT("Quick-chat hints: Browse, Shirt, Friend, NoThanks, WrongSize.");
	DrawDebugText(PrivateHint, FLinearColor::White, PanelX + 18.0f, PanelY + 108.0f, 0.92f);
	DrawDebugText(QuickChat, FLinearColor(0.72f, 0.88f, 1.0f, 1.0f), 18.0f, PanelY - 30.0f, 0.95f);
}

void AVNHDebugHUD::DrawAccusationScreen(const AVNHGameState* VNHGameState, const AVNHPlayerState* VNHPlayerState)
{
	const FLinearColor Accent = FLinearColor(1.0f, 0.32f, 0.16f, 1.0f);
	const float OverlayW = FMath::Min(800.0f, Canvas->ClipX - 48.0f);
	const float OverlayH = 230.0f;
	const float OverlayX = (Canvas->ClipX - OverlayW) * 0.5f;
	const float OverlayY = FMath::Max(54.0f, Canvas->ClipY * 0.16f);
	DrawSection15Panel(OverlayX, OverlayY, OverlayW, OverlayH, Accent);

	const float RemainingSeconds = VNHGameState && VNHGameState->GetPhaseEndsAtServerTime() > 0.0f
		? FMath::Max(0.0f, VNHGameState->GetPhaseEndsAtServerTime() - VNHGameState->GetServerWorldTimeSeconds())
		: 0.0f;
	DrawDebugText(TEXT("ACCUSATION"), FLinearColor::White, OverlayX + 28.0f, OverlayY + 24.0f, 2.0f);
	DrawDebugText(TEXT("Everyone freezes. Hunter confirms one target."), FLinearColor(0.95f, 0.95f, 1.0f, 1.0f), OverlayX + 28.0f, OverlayY + 86.0f, 1.26f);
	DrawDebugText(VNHPlayerState && VNHPlayerState->IsHunter() ? TEXT("Use your suspect marks. Accuse only when sure.") : TEXT("Stay still. Do not reveal private role information."), Accent, OverlayX + 28.0f, OverlayY + 132.0f, 1.16f);
	DrawDebugText(FString::Printf(TEXT("Reveal in %.0fs"), RemainingSeconds), FLinearColor::White, OverlayX + OverlayW - 170.0f, OverlayY + 28.0f, 1.25f);
}

void AVNHDebugHUD::DrawRevealScreen(const AVNHGameState* VNHGameState)
{
	const float OverlayW = FMath::Min(840.0f, Canvas->ClipX - 48.0f);
	const float OverlayH = 245.0f;
	const float OverlayX = (Canvas->ClipX - OverlayW) * 0.5f;
	const float OverlayY = FMath::Max(54.0f, Canvas->ClipY * 0.15f);
	DrawSection15Panel(OverlayX, OverlayY, OverlayW, OverlayH, FLinearColor(1.0f, 0.82f, 0.05f, 1.0f));

	const FString Summary = VNHGameState ? VNHGameState->GetRevealSummaryText().ToString() : TEXT("No reveal data.");
	const FVNHAccusationResult Result = VNHGameState ? VNHGameState->GetAccusationResult() : FVNHAccusationResult();
	const FString AwardsText = !Result.bResolved
		? TEXT("Awards: Most Normal Alien  |  Hunter Ran Out Of Clock")
		: Result.bCorrect
			? TEXT("Awards: Hunter Was Cooking  |  Least Normal Human")
			: TEXT("Awards: Wrongfully Accused Spotlight  |  Most Normal Alien");
	const FString FooterText = !Result.bResolved
		? TEXT("No accusation landed. The Alien wins by social camouflage.")
		: Result.bCorrect
			? TEXT("Clean read. The Hunter found the hidden Alien.")
			: TEXT("Celebrate the false positive, then reset. Public play should not reward sabotage.");
	DrawDebugText(TEXT("ROUND REVEAL"), FLinearColor::White, OverlayX + 28.0f, OverlayY + 24.0f, 2.0f);
	DrawDebugText(Summary, FLinearColor(0.88f, 0.95f, 1.0f, 1.0f), OverlayX + 28.0f, OverlayY + 88.0f, 1.25f);
	DrawDebugText(AwardsText, FLinearColor(1.0f, 0.82f, 0.05f, 1.0f), OverlayX + 28.0f, OverlayY + 140.0f, 1.05f);
	DrawDebugText(FooterText, FLinearColor::White, OverlayX + 28.0f, OverlayY + 180.0f, 0.95f);
}

void AVNHDebugHUD::PlayUISound(USoundBase* Sound, float VolumeMultiplier) const
{
	if (Sound && GetWorld())
	{
		UGameplayStatics::PlaySound2D(GetWorld(), Sound, VolumeMultiplier);
	}
}
