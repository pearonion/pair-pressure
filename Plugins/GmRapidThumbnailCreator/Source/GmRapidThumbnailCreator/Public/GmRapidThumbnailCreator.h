// Copyright Dev.GaeMyo 2024. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"

class FUICommandList;

class FGmRapidThumbnailCreatorModule : public IModuleInterface
{
public:

	/** IModuleInterface implementation */
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;

	static void OnClickedCreatorBtn();

private:

	void RegisterMenuBtn();

	TSharedPtr<FUICommandList> ThumbnailCreatorBtnCmd;
	
};
