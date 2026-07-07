// Copyright Dev.GaeMyo 2024. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

#include "Styles/GmRapidThumbnailCreatorStyle.h"

#include "GmRapidThumbnailCreatorCommands.h"

class FUICommandInfo;

class FGmRapidThumbnailCreatorCommands final : public TCommands<FGmRapidThumbnailCreatorCommands>
{
	
public:

	FGmRapidThumbnailCreatorCommands()
		:
	TCommands/*<FGmRapidThumbnailCreatorCommands>*/(TEXT("GmRapidThumbnailCreator"),
		NSLOCTEXT("Contexts", "GmRapidThumbnailCreator",
			"GmRapidThumbnailCreator Plugin"), NAME_None,
			FGmRapidThumbnailCreatorStyle::GetStyleSetName())
	{}

	// TCommands<> interface
	virtual void RegisterCommands() override;
	
	TSharedPtr<FUICommandInfo> PluginAction;
	
};
