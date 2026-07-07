// Copyright Dev.GaeMyo 2024. All Rights Reserved.

#include "Cmds/GmRapidThumbnailCreatorCommands.h"

#define LOCTEXT_NAMESPACE "FGmRapidThumbnailCreatorModule"

void FGmRapidThumbnailCreatorCommands::RegisterCommands()
{
	UI_COMMAND(PluginAction, "GmRapidThumbnailCreator", "Open Rapid Thumbnail Creator",
		EUserInterfaceActionType::Button, FInputChord());
}

#undef LOCTEXT_NAMESPACE
