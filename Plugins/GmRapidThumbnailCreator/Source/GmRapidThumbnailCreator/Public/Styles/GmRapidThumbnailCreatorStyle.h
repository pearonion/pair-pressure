// Copyright Dev.GaeMyo 2024. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

#include "Styling/SlateStyle.h"

class FSlateStyleSet;

class FGmRapidThumbnailCreatorStyle
{
	
public:

	static void Initialize();
	static void Shutdown();
	
	static void ReloadTextures();
	static const ISlateStyle& Get();
	static FName GetStyleSetName();

private:

	static TSharedRef<FSlateStyleSet> Create();
	static TSharedPtr<FSlateStyleSet> StyleInstance;
};