#include "VNHLobbyPlayButton.h"

#include "Components/BoxComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Components/TextRenderComponent.h"
#include "Engine/StaticMesh.h"
#include "GameFramework/Pawn.h"
#include "Kismet/GameplayStatics.h"
#include "UObject/ConstructorHelpers.h"
#include "VNHGameMode.h"

AVNHLobbyPlayButton::AVNHLobbyPlayButton()
{
	PrimaryActorTick.bCanEverTick = true;

	ButtonMesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("ButtonMesh"));
	SetRootComponent(ButtonMesh);
	ButtonMesh->SetCollisionProfileName(TEXT("BlockAll"));
	ButtonMesh->SetRenderCustomDepth(false);

	static ConstructorHelpers::FObjectFinder<UStaticMesh> CylinderMesh(TEXT("/Engine/BasicShapes/Cylinder.Cylinder"));
	if (CylinderMesh.Succeeded())
	{
		ButtonMesh->SetStaticMesh(CylinderMesh.Object);
		ButtonMesh->SetWorldScale3D(FVector(1.35f, 1.35f, 0.45f));
	}

	InteractionVolume = CreateDefaultSubobject<UBoxComponent>(TEXT("InteractionVolume"));
	InteractionVolume->SetupAttachment(ButtonMesh);
	InteractionVolume->SetBoxExtent(FVector(InteractionRadius, InteractionRadius, 170.0f));
	InteractionVolume->SetRelativeLocation(FVector(0.0f, 0.0f, 85.0f));
	InteractionVolume->SetCollisionProfileName(TEXT("OverlapAllDynamic"));

	PromptText = CreateDefaultSubobject<UTextRenderComponent>(TEXT("PromptText"));
	PromptText->SetupAttachment(ButtonMesh);
	PromptText->SetRelativeLocation(FVector(0.0f, 0.0f, 185.0f));
	PromptText->SetRelativeRotation(FRotator(0.0f, 180.0f, 0.0f));
	PromptText->SetHorizontalAlignment(EHTA_Center);
	PromptText->SetVerticalAlignment(EVRTA_TextCenter);
	PromptText->SetText(FText::FromString(TEXT("HOST START\nHOLD E")));
	PromptText->SetTextRenderColor(FColor(0, 255, 235));
	PromptText->SetWorldSize(32.0f);

	Tags.AddUnique(TEXT("VNH.LobbyStart"));
	Tags.AddUnique(TEXT("VNH.Interactable"));
}

void AVNHLobbyPlayButton::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	if (!PromptText)
	{
		return;
	}

	const APlayerController* PlayerController = UGameplayStatics::GetPlayerController(this, 0);
	if (!PlayerController)
	{
		return;
	}

	FVector CameraLocation = FVector::ZeroVector;
	FRotator CameraRotation = FRotator::ZeroRotator;
	PlayerController->GetPlayerViewPoint(CameraLocation, CameraRotation);
	const FRotator LookAtRotation = (CameraLocation - PromptText->GetComponentLocation()).Rotation();
	PromptText->SetWorldRotation(FRotator(0.0f, LookAtRotation.Yaw, 0.0f));
}

bool AVNHLobbyPlayButton::ActivateLobbyStart(APlayerController* RequestingPlayer)
{
	if (AVNHGameMode* VNHGameMode = GetWorld() ? GetWorld()->GetAuthGameMode<AVNHGameMode>() : nullptr)
	{
		return VNHGameMode->StartRoundFromLobby(RequestingPlayer);
	}

	return false;
}

bool AVNHLobbyPlayButton::IsPawnWithinInteractionRange(const APawn* Pawn) const
{
	if (!Pawn)
	{
		return false;
	}

	return FVector::DistSquared(Pawn->GetActorLocation(), GetActorLocation()) <= FMath::Square(InteractionRadius);
}
