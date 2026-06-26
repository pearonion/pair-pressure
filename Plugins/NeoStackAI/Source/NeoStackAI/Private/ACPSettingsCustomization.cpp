// Copyright 2026 Betide Studio. All Rights Reserved.

#include "ACPSettingsCustomization.h"
#include "ACPSettings.h"
#include "DetailLayoutBuilder.h"
#include "DetailCategoryBuilder.h"
#include "DetailWidgetRow.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Layout/SBox.h"
#include "Styling/CoreStyle.h"
#include "Styling/SlateColor.h"
#include "Modules/ModuleManager.h"
#include "PropertyEditorModule.h"
#include "IDetailPropertyRow.h"

#define LOCTEXT_NAMESPACE "ACPSettingsCustomization"

TSharedRef<IDetailCustomization> FACPSettingsCustomization::MakeInstance()
{
	return MakeShareable(new FACPSettingsCustomization);
}

namespace
{
	/** Helper to add an italic description row at the top of a category */
	void AddCategoryDescription(IDetailCategoryBuilder& Category, const FText& SearchText, const FText& Description)
	{
		Category.AddCustomRow(SearchText)
			.WholeRowContent()
			[
				SNew(SBox)
				.Padding(FMargin(0.0f, 0.0f, 0.0f, 4.0f))
				[
					SNew(STextBlock)
					.Text(Description)
					.Font(FCoreStyle::GetDefaultFontStyle("Italic", 9))
					.ColorAndOpacity(FSlateColor::UseSubduedForeground())
					.AutoWrapText(true)
				]
			];
	}
}

void FACPSettingsCustomization::CustomizeDetails(IDetailLayoutBuilder& DetailBuilder)
{
	// Recovery banner — renders above all other categories.
	IDetailCategoryBuilder& RecoveryCategory = DetailBuilder.EditCategory(
		"Recovery & Startup", FText::GetEmpty(), ECategoryPriority::Transform);
	AddCategoryDescription(RecoveryCategory,
		LOCTEXT("RecoveryBannerSearch", "Recovery"),
		LOCTEXT("RecoveryBannerDesc",
			"Most settings for NeoStack AI live inside the plugin panel (open the chat window, then Settings). "
			"This page is only for startup, authentication, and recovery options that must work before the web UI loads."));

	// Set category ordering via priority (Auto-Update first so users see token/credits setup immediately)
	DetailBuilder.EditCategory("Auto-Update", FText::GetEmpty(), ECategoryPriority::Important);
	DetailBuilder.EditCategory("General", FText::GetEmpty(), ECategoryPriority::Important);

	// ACP Agents - parent category with description
	IDetailCategoryBuilder& ACPCategory = DetailBuilder.EditCategory("ACP Agents");
	AddCategoryDescription(ACPCategory,
		LOCTEXT("ACPAgentsSearch", "ACP Agents"),
		LOCTEXT("ACPAgentsDesc",
			"ACP (Agent Client Protocol) agents are external CLI tools (Claude Code, Gemini CLI, Codex, etc.) "
			"that the plugin spawns as subprocesses and communicates with over stdin/stdout."));

	// MCP Server - add description
	IDetailCategoryBuilder& MCPCategory = DetailBuilder.EditCategory("MCP Server");
	AddCategoryDescription(MCPCategory,
		LOCTEXT("MCPServerSearch", "MCP Server"),
		LOCTEXT("MCPServerDesc",
			"The MCP (Model Context Protocol) server exposes the plugin's tools over HTTP, allowing any "
			"MCP-compatible agent to connect. The server starts automatically (default port 9315). Check the status bar for the active port."));

	// Auto-Update
	IDetailCategoryBuilder& AutoUpdateCategory = DetailBuilder.EditCategory("Auto-Update");
	AddCategoryDescription(AutoUpdateCategory,
		LOCTEXT("AutoUpdateSearch", "Auto-Update"),
		LOCTEXT("AutoUpdateDesc",
			"Download plugin updates directly from NeoStack.dev. Updates use the NeoStack Cloud API key "
			"configured in Settings > Chat & Agents > Chat Providers > NeoStack Cloud."));

	// Ensure remaining categories appear in order
	DetailBuilder.EditCategory("Tools");
	DetailBuilder.EditCategory("Debug");
}

void FACPSettingsCustomization::Register()
{
	FPropertyEditorModule& PropertyModule = FModuleManager::LoadModuleChecked<FPropertyEditorModule>("PropertyEditor");
	PropertyModule.RegisterCustomClassLayout(
		UACPSettings::StaticClass()->GetFName(),
		FOnGetDetailCustomizationInstance::CreateStatic(&FACPSettingsCustomization::MakeInstance)
	);
}

void FACPSettingsCustomization::Unregister()
{
	if (FModuleManager::Get().IsModuleLoaded("PropertyEditor"))
	{
		FPropertyEditorModule& PropertyModule = FModuleManager::GetModuleChecked<FPropertyEditorModule>("PropertyEditor");
		PropertyModule.UnregisterCustomClassLayout(UACPSettings::StaticClass()->GetFName());
	}
}

#undef LOCTEXT_NAMESPACE
