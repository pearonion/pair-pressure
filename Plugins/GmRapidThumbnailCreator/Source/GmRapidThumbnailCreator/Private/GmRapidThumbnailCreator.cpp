// Copyright Dev.GaeMyo 2024. All Rights Reserved.

#include "GmRapidThumbnailCreator.h"

#include "EditorUtilitySubsystem.h"
#include "EditorUtilityWidgetBlueprint.h"
#include "Cmds/GmRapidThumbnailCreatorCommands.h"

#define LOCTEXT_NAMESPACE "FGmRapidThumbnailCreatorModule"

void FGmRapidThumbnailCreatorModule::StartupModule()
{
	FGmRapidThumbnailCreatorStyle::Initialize();
	FGmRapidThumbnailCreatorStyle::ReloadTextures();

	FGmRapidThumbnailCreatorCommands::Register();

	ThumbnailCreatorBtnCmd = MakeShareable(new FUICommandList);

	ThumbnailCreatorBtnCmd->MapAction(FGmRapidThumbnailCreatorCommands::Get().PluginAction,
		FExecuteAction::CreateStatic(&FGmRapidThumbnailCreatorModule::OnClickedCreatorBtn),
		FCanExecuteAction());

	UToolMenus::RegisterStartupCallback(FSimpleMulticastDelegate::FDelegate::CreateRaw(this,
		&FGmRapidThumbnailCreatorModule::RegisterMenuBtn));
}

void FGmRapidThumbnailCreatorModule::ShutdownModule()
{
	UToolMenus::UnRegisterStartupCallback(this);
	UToolMenus::UnregisterOwner(this);

	FGmRapidThumbnailCreatorStyle::Shutdown();
	FGmRapidThumbnailCreatorCommands::Unregister();
}

void FGmRapidThumbnailCreatorModule::OnClickedCreatorBtn()
{
	const FSoftObjectPath EditorWidgetPath(
		"/Script/Blutility.EditorUtilityWidgetBlueprint'/GmRapidThumbnailCreator/UI/EditorWidgets/EUW_ThumbnailCreator'");
	
	if (UEditorUtilityWidgetBlueprint* EditorWidget{
		Cast<UEditorUtilityWidgetBlueprint>(EditorWidgetPath.TryLoad())})
	{
		GEditor->GetEditorSubsystem<UEditorUtilitySubsystem>()->SpawnAndRegisterTab(EditorWidget);
	}
}

void FGmRapidThumbnailCreatorModule::RegisterMenuBtn()
{
	FToolMenuOwnerScoped OwnerScoped(this);
	{
		UToolMenus::Get()->ExtendMenu("LevelEditor.MainMenu.Window")->FindOrAddSection("WindowLayout").
		AddMenuEntryWithCommandList(FGmRapidThumbnailCreatorCommands::Get().PluginAction, ThumbnailCreatorBtnCmd);
	}

	{
		UToolMenus::Get()->ExtendMenu("LevelEditor.LevelEditorToolBar.PlayToolBar")->FindOrAddSection("PluginTools").
		AddEntry(FToolMenuEntry::InitToolBarButton(FGmRapidThumbnailCreatorCommands::Get().PluginAction)).
		SetCommandList(ThumbnailCreatorBtnCmd);
	}
}

#undef LOCTEXT_NAMESPACE
	
IMPLEMENT_MODULE(FGmRapidThumbnailCreatorModule, GmRapidThumbnailCreator)