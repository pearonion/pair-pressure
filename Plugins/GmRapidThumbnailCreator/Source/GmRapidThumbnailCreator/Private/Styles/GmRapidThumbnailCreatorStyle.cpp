// Copyright Dev.GaeMyo 2024. All Rights Reserved.

#include "Styles/GmRapidThumbnailCreatorStyle.h"
#include "Styling/SlateStyleRegistry.h"
#include "Interfaces/IPluginManager.h"
#include "Styling/SlateStyleMacros.h"

TSharedPtr<FSlateStyleSet> FGmRapidThumbnailCreatorStyle::StyleInstance{nullptr};

void FGmRapidThumbnailCreatorStyle::Initialize()
{
	if (!StyleInstance.IsValid())
	{
		StyleInstance = Create();
		FSlateStyleRegistry::RegisterSlateStyle(*StyleInstance);
	}
}

void FGmRapidThumbnailCreatorStyle::Shutdown()
{
	FSlateStyleRegistry::UnRegisterSlateStyle(*StyleInstance);
	ensure(StyleInstance.IsUnique());
	StyleInstance.Reset();
}

FName FGmRapidThumbnailCreatorStyle::GetStyleSetName()
{
	return TEXT("GmRapidThumbnailCreatorStyle");
}

const FVector2D Icon24x24(24.f, 24.f);

#define RootToContentDir Style->RootToContentDir

TSharedRef<FSlateStyleSet> FGmRapidThumbnailCreatorStyle::Create()
{
	TSharedRef<FSlateStyleSet> Style{MakeShareable(new FSlateStyleSet("GmRapidThumbnailCreatorStyle"))};
	
	Style->SetContentRoot(IPluginManager::Get().FindPlugin("GmRapidThumbnailCreator")->GetBaseDir() / TEXT("Resources"));
	Style->Set("GmRapidThumbnailCreator.PluginAction",
		new IMAGE_BRUSH_SVG(TEXT("GmRapidThumbnailCreatorIconsvg"), Icon24x24));
	
	return Style;
}

void FGmRapidThumbnailCreatorStyle::ReloadTextures()
{
	if (FSlateApplication::IsInitialized())
	{
		FSlateApplication::Get().GetRenderer()->ReloadTextureResources();
	}
}

const ISlateStyle& FGmRapidThumbnailCreatorStyle::Get()
{
	return *StyleInstance;
}
