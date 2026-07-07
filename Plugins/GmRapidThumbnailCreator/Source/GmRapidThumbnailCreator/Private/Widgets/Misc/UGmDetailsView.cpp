// Copyright Dev.GaeMyo 2024. All Rights Reserved.


#include "Widgets/Misc/UGmDetailsView.h"

#define LOCTEXT_NAMESPACE "GmUMG"

const FText UUGmDetailsView::GetPaletteCategory()
{
	// void*
	// void (UGmEUW_ThumbnailCreator::*b)(FName InP){&UGmEUW_ThumbnailCreator::Hi};
	// OnPropertyChanged.AddUniqueDynamic(Hi, &UGmEUW_ThumbnailCreator::Hi);
	return LOCTEXT("GmEditor", "GmEditor");
}

#undef LOCTEXT_NAMESPACE