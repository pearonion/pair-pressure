#pragma once

#include "CoreMinimal.h"
#include "UObject/Interface.h"
#include "PPMascotSelectionInterface.generated.h"

UINTERFACE(BlueprintType)
class VNHSIMULATOR_API UPPMascotSelectionInterface : public UInterface
{
	GENERATED_BODY()
};

class VNHSIMULATOR_API IPPMascotSelectionInterface
{
	GENERATED_BODY()

public:
	UFUNCTION(BlueprintNativeEvent, BlueprintCallable, Category = "Pair Pressure|Mascot")
	FName GetSelectedMascotRowName() const;

	UFUNCTION(BlueprintNativeEvent, BlueprintCallable, Category = "Pair Pressure|Mascot")
	void ApplySelectedMascotRowName(FName InMascotRowName);
};
