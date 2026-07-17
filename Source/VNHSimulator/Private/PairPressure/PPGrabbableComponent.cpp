#include "PairPressure/PPGrabbableComponent.h"

#include "Components/PrimitiveComponent.h"
#include "Components/SceneComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "GameFramework/Actor.h"
#include "GameFramework/Character.h"
#include "Net/UnrealNetwork.h"

UPPGrabbableComponent::UPPGrabbableComponent()
{
	SetIsReplicatedByDefault(true);
	PrimaryComponentTick.bCanEverTick = false;
}

void UPPGrabbableComponent::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(UPPGrabbableComponent, CurrentGrabber);
}

bool UPPGrabbableComponent::CanBeGrabbed_Implementation(AActor* RequestingGrabber) const
{
	return bGrabEnabled && RequestingGrabber && RequestingGrabber != GetOwner()
		&& (!CurrentGrabber || CurrentGrabber == RequestingGrabber);
}

FPPGrabProfile UPPGrabbableComponent::GetGrabProfile_Implementation() const
{
	return GrabProfile;
}

UPrimitiveComponent* UPPGrabbableComponent::GetGrabPrimitive_Implementation() const
{
	AActor* OwnerActor = GetOwner();
	if (!OwnerActor)
	{
		return nullptr;
	}

	UPrimitiveComponent* RootPrimitive = Cast<UPrimitiveComponent>(OwnerActor->GetRootComponent());
	const bool bNeedsPhysics = GrabProfile.TargetType == EPPGrabTargetType::GameplayItem
		|| GrabProfile.TargetType == EPPGrabTargetType::LargePushable;
	if (RootPrimitive && (!bNeedsPhysics || RootPrimitive->IsSimulatingPhysics()))
	{
		return RootPrimitive;
	}

	TInlineComponentArray<UPrimitiveComponent*> PrimitiveComponents;
	OwnerActor->GetComponents(PrimitiveComponents);
	for (UPrimitiveComponent* PrimitiveComponent : PrimitiveComponents)
	{
		if (PrimitiveComponent && PrimitiveComponent->IsSimulatingPhysics())
		{
			return PrimitiveComponent;
		}
	}

	return RootPrimitive ? RootPrimitive : (PrimitiveComponents.IsEmpty() ? nullptr : PrimitiveComponents[0]);
}

FVector UPPGrabbableComponent::GetGrabPoint_Implementation(AActor* RequestingGrabber) const
{
	if (const AActor* OwnerActor = GetOwner())
	{
		TInlineComponentArray<USceneComponent*> SceneComponents;
		OwnerActor->GetComponents(SceneComponents);
		for (const USceneComponent* SceneComponent : SceneComponents)
		{
			if (SceneComponent && (SceneComponent->GetFName() == GripPointComponentName
				|| SceneComponent->ComponentHasTag(TEXT("GripPoint"))))
			{
				return SceneComponent->GetComponentLocation();
			}
		}

		if (GrabProfile.TargetType == EPPGrabTargetType::Player)
		{
			if (const ACharacter* OwnerCharacter = Cast<ACharacter>(OwnerActor))
			{
				if (const USkeletalMeshComponent* CharacterMesh = OwnerCharacter->GetMesh())
				{
					for (const FName PlayerAnchorName : {FName(TEXT("spine_03")), FName(TEXT("spine_02")), FName(TEXT("pelvis"))})
					{
						if (CharacterMesh->DoesSocketExist(PlayerAnchorName))
						{
							return CharacterMesh->GetSocketLocation(PlayerAnchorName);
						}
					}
				}
			}
			return OwnerActor->GetActorLocation() + FVector(0.0f, 0.0f, 55.0f);
		}

		if (const UPrimitiveComponent* PrimitiveComponent = GetGrabPrimitive_Implementation())
		{
			return PrimitiveComponent->GetCenterOfMass();
		}

		return OwnerActor->GetActorLocation();
	}

	return FVector::ZeroVector;
}

void UPPGrabbableComponent::OnGrabStarted_Implementation(AActor* NewGrabber)
{
	if (GetOwner() && GetOwner()->HasAuthority())
	{
		CurrentGrabber = NewGrabber;
	}
}

void UPPGrabbableComponent::OnGrabEnded_Implementation(AActor* PreviousGrabber)
{
	if (GetOwner() && GetOwner()->HasAuthority() && CurrentGrabber == PreviousGrabber)
	{
		CurrentGrabber = nullptr;
	}
}
