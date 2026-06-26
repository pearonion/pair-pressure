#include "VNHPlayerController.h"

#include "VNHGameMode.h"
#include "VNHShopperCharacter.h"

void AVNHPlayerController::RequestPublicTest(EVNHPublicTestType TestType)
{
	ServerRequestPublicTest(TestType);
}

void AVNHPlayerController::RequestAccusation(AVNHShopperCharacter* AccusedShopper)
{
	ServerRequestAccusation(AccusedShopper);
}

void AVNHPlayerController::RequestActNatural()
{
	ServerRequestActNatural();
}

void AVNHPlayerController::ServerRequestPublicTest_Implementation(EVNHPublicTestType TestType)
{
	if (AVNHGameMode* VNHGameMode = GetWorld()->GetAuthGameMode<AVNHGameMode>())
	{
		VNHGameMode->RequestPublicTest(this, TestType);
	}
}

void AVNHPlayerController::ServerRequestAccusation_Implementation(AVNHShopperCharacter* AccusedShopper)
{
	if (AVNHGameMode* VNHGameMode = GetWorld()->GetAuthGameMode<AVNHGameMode>())
	{
		VNHGameMode->RequestAccusation(this, AccusedShopper);
	}
}

void AVNHPlayerController::ServerRequestActNatural_Implementation()
{
	if (AVNHShopperCharacter* Shopper = Cast<AVNHShopperCharacter>(GetPawn()))
	{
		Shopper->UseActNatural();
	}
}
