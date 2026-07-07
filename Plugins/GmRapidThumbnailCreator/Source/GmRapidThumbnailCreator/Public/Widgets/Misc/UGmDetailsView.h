// Copyright Dev.GaeMyo 2024. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

#include "Components/DetailsView.h"
#include "UGmDetailsView.generated.h"

UCLASS()
class GMRAPIDTHUMBNAILCREATOR_API UUGmDetailsView : public UDetailsView
{
	GENERATED_BODY()

public:

	FOnPropertyValueChanged& GmGetPropertyChangedDel()
	{
		return OnPropertyChanged;
	}

	virtual const FText GetPaletteCategory() override;

};
