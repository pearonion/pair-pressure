#include "VNHLobbyPlayButton.h"

#include "Components/BoxComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "UObject/ConstructorHelpers.h"
#include "VNHGameMode.h"

AVNHLobbyPlayButton::AVNHLobbyPlayButton()
{
	PrimaryActorTick.bCanEverTick = false;

	ButtonMesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("ButtonMesh"));
	SetRootComponent(ButtonMesh);
	ButtonMesh->SetCollisionProfileName(TEXT("BlockAll"));

	static ConstructorHelpers::FObjectFinder<UStaticMesh> CubeMesh(TEXT("/Engine/BasicShapes/Cube.Cube"));
	if (CubeMesh.Succeeded())
	{
		ButtonMesh->SetStaticMesh(CubeMesh.Object);
		ButtonMesh->SetWorldScale3D(FVector(2.0f, 2.0f, 0.25f));
	}

	InteractionVolume = CreateDefaultSubobject<UBoxComponent>(TEXT("InteractionVolume"));
	InteractionVolume->SetupAttachment(ButtonMesh);
	InteractionVolume->SetBoxExtent(FVector(260.0f, 260.0f, 160.0f));
	InteractionVolume->SetRelativeLocation(FVector(0.0f, 0.0f, 85.0f));
	InteractionVolume->SetCollisionProfileName(TEXT("OverlapAllDynamic"));
}

bool AVNHLobbyPlayButton::ActivateLobbyStart(APlayerController* RequestingPlayer)
{
	if (AVNHGameMode* VNHGameMode = GetWorld() ? GetWorld()->GetAuthGameMode<AVNHGameMode>() : nullptr)
	{
		return VNHGameMode->StartRoundFromLobby(RequestingPlayer);
	}

	return false;
}
