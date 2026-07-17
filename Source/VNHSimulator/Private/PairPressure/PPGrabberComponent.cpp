#include "PairPressure/PPGrabberComponent.h"

#include "Components/CapsuleComponent.h"
#include "Components/PrimitiveComponent.h"
#include "Components/SceneComponent.h"
#include "DrawDebugHelpers.h"
#include "Engine/EngineTypes.h"
#include "Engine/OverlapResult.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "GameFramework/Character.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Animation/AnimInstance.h"
#include "Animation/AnimSequence.h"
#include "Engine/DataTable.h"
#include "Net/UnrealNetwork.h"
#include "PhysicsEngine/PhysicsAsset.h"
#include "PhysicsEngine/PhysicsHandleComponent.h"
#include "PairPressure/PPGrabbableComponent.h"
#include "PairPressure/PPPhysicalStateComponent.h"
#include "PairPressure/PPTeamMemberComponent.h"
#include "UObject/UnrealType.h"
#include "VNHAlienLocomotionComponent.h"
#include "VNHLog.h"
#include "VNHShopperCharacter.h"
#include "TimerManager.h"

namespace
{
constexpr float PPGrabAlignmentScoreWeight = 1000.0f;
constexpr float PPGrabPriorityScoreWeight = 100.0f;
constexpr float PPGrabDistanceScoreWeight = 125.0f;
constexpr float PPGrabLineOfSightScore = 50.0f;
}

UPPGrabberComponent::UPPGrabberComponent()
{
	SetIsReplicatedByDefault(true);
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.bStartWithTickEnabled = false;
}

void UPPGrabberComponent::BeginPlay()
{
	Super::BeginPlay();
	PhysicsHandle = GetOwner() ? GetOwner()->FindComponentByClass<UPhysicsHandleComponent>() : nullptr;
	if (const ACharacter* OwnerCharacter = Cast<ACharacter>(GetOwner()))
	{
		if (const USkeletalMeshComponent* OwnerMesh = OwnerCharacter->GetMesh())
		{
			IncomingGrabInitialMeshRelativeTransform = OwnerMesh->GetRelativeTransform();
		}
	}
}

void UPPGrabberComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

	if (!GetOwner() || !GetOwner()->HasAuthority())
	{
		return;
	}
	if (ImmuneGrabber.IsValid() && GetWorld() && GetWorld()->GetTimeSeconds() >= ImmunityEndTimeSeconds)
	{
		ImmuneGrabber.Reset();
		ImmunityEndTimeSeconds = 0.0;
	}

	switch (GrabState)
	{
	case EPPGrabState::Reaching:
		UpdateReachSearch(DeltaTime);
		break;
	case EPPGrabState::HoldingItem:
		UpdateHeldItem(DeltaTime);
		break;
	case EPPGrabState::GrabbingPlayer:
	case EPPGrabState::MutualGrab:
		UpdatePlayerGrab(DeltaTime);
		break;
	case EPPGrabState::PushingObject:
		UpdatePushable(DeltaTime);
		break;
	case EPPGrabState::HangingFromLedge:
		UpdateLedgeGrab(DeltaTime);
		break;
	default:
		break;
	}
}

void UPPGrabberComponent::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(UPPGrabberComponent, GrabState);
	DOREPLIFETIME(UPPGrabberComponent, GrabTarget);
	DOREPLIFETIME(UPPGrabberComponent, IncomingGrabber);
	DOREPLIFETIME(UPPGrabberComponent, PresentationGrabPoint);
	DOREPLIFETIME(UPPGrabberComponent, ActiveProfile);
}

void UPPGrabberComponent::BeginGrab_Implementation(const FVector& CameraForward)
{
	AActor* OwnerActor = GetOwner();
	if (!OwnerActor || GrabState != EPPGrabState::None)
	{
		return;
	}

	const FVector SafeCameraForward = CameraForward.GetSafeNormal();
	if (SafeCameraForward.IsNearlyZero())
	{
		return;
	}

	if (!OwnerActor->HasAuthority())
	{
		const FPPGrabTargetData PredictedTarget = FindBestTarget(SafeCameraForward);
		if (bDrawGrabDebug && GetWorld())
		{
			const FVector PredictedStart = GetGrabOrigin();
			DrawDebugSphere(
				GetWorld(),
				PredictedTarget.IsValid() ? FVector(PredictedTarget.GrabPoint) : PredictedStart + SafeCameraForward * SearchReach,
				PredictedTarget.IsValid() ? 14.0f : SearchRadius,
				16,
				FColor::Cyan,
				false,
				0.25f);
		}
		if (UVNHAlienLocomotionComponent* LocomotionComponent = OwnerActor->FindComponentByClass<UVNHAlienLocomotionComponent>())
		{
			LocomotionComponent->SetGrabMovementMultiplier(0.94f);
		}
		OnGrabStateChanged.Broadcast(EPPGrabState::Reaching, PredictedTarget.TargetActor);
		ServerBeginGrab(SafeCameraForward);
		return;
	}

	LastValidatedCameraForward = SafeCameraForward;
	const FPPGrabTargetData TargetData = FindBestTarget(SafeCameraForward);
	if (TargetData.IsValid())
	{
		StartGrabAuthoritative(TargetData);
	}
	else
	{
		SustainedGrabSeconds = 0.0f;
		SetGrabPresentation(EPPGrabState::Reaching, nullptr);
		SetComponentTickEnabled(true);
	}
}

void UPPGrabberComponent::ReleaseGrab_Implementation()
{
	AActor* OwnerActor = GetOwner();
	if (!OwnerActor)
	{
		return;
	}

	if (!OwnerActor->HasAuthority())
	{
		UE_LOG(LogVNH, Display, TEXT("PairPressureGrab: local LMB release requested. Grabber=%s State=%d Target=%s"),
			*GetNameSafe(OwnerActor),
			static_cast<int32>(GrabState),
			*GetNameSafe(GrabTarget));
		ServerReleaseGrab();
		if (UVNHAlienLocomotionComponent* LocomotionComponent = OwnerActor->FindComponentByClass<UVNHAlienLocomotionComponent>())
		{
			LocomotionComponent->SetGrabMovementMultiplier(1.0f);
		}
		OnGrabStateChanged.Broadcast(EPPGrabState::None, nullptr);
		return;
	}

	UE_LOG(LogVNH, Display, TEXT("PairPressureGrab: authority LMB release requested. Grabber=%s State=%d Target=%s"),
		*GetNameSafe(OwnerActor),
		static_cast<int32>(GrabState),
		*GetNameSafe(GrabTarget));
	ReleaseGrabAuthoritative();
}

void UPPGrabberComponent::RequestHeldItemThrow(const FVector& ThrowDirection)
{
	AActor* OwnerActor = GetOwner();
	if (!OwnerActor || (GrabState != EPPGrabState::HoldingItem && GrabState != EPPGrabState::GrabbingPlayer))
	{
		return;
	}

	const FVector SafeThrowDirection = ThrowDirection.GetSafeNormal();
	if (SafeThrowDirection.IsNearlyZero())
	{
		return;
	}

	if (!OwnerActor->HasAuthority())
	{
		ServerThrowHeldItem(SafeThrowDirection);
		return;
	}
	PerformHeldItemThrow(SafeThrowDirection, 0.0f);
}

void UPPGrabberComponent::RequestChargedThrow(const FVector& ThrowDirection, float ChargeAlpha)
{
	AActor* OwnerActor = GetOwner();
	if (!OwnerActor || !OwnerActor->HasAuthority())
	{
		return;
	}
	PerformHeldItemThrow(ThrowDirection.GetSafeNormal(), FMath::Clamp(ChargeAlpha, 0.0f, 1.0f));
}

void UPPGrabberComponent::RequestDirectionalEscape(const FVector& EscapeDirection)
{
	AActor* OwnerActor = GetOwner();
	if (!OwnerActor || !IncomingGrabber)
	{
		return;
	}

	const FVector SafeEscapeDirection = EscapeDirection.GetSafeNormal2D();
	if (SafeEscapeDirection.IsNearlyZero())
	{
		return;
	}

	if (!OwnerActor->HasAuthority())
	{
		ServerRequestDirectionalEscape(SafeEscapeDirection);
		return;
	}
	PerformDirectionalEscape(SafeEscapeDirection);
}

void UPPGrabberComponent::ServerBeginGrab_Implementation(FVector_NetQuantizeNormal CameraForward)
{
	BeginGrab_Implementation(CameraForward);
}

void UPPGrabberComponent::ServerReleaseGrab_Implementation()
{
	UE_LOG(LogVNH, Display, TEXT("PairPressureGrab: server received LMB release. Grabber=%s State=%d Target=%s"),
		*GetNameSafe(GetOwner()),
		static_cast<int32>(GrabState),
		*GetNameSafe(GrabTarget));
	ReleaseGrabAuthoritative();
}

void UPPGrabberComponent::ServerThrowHeldItem_Implementation(FVector_NetQuantizeNormal ThrowDirection)
{
	PerformHeldItemThrow(ThrowDirection, 0.0f);
}

void UPPGrabberComponent::ServerRequestDirectionalEscape_Implementation(FVector_NetQuantizeNormal EscapeDirection)
{
	PerformDirectionalEscape(EscapeDirection);
}

void UPPGrabberComponent::ClientGrabRejected_Implementation()
{
	if (AActor* OwnerActor = GetOwner())
	{
		if (UVNHAlienLocomotionComponent* LocomotionComponent = OwnerActor->FindComponentByClass<UVNHAlienLocomotionComponent>())
		{
			LocomotionComponent->SetGrabMovementMultiplier(1.0f);
			LocomotionComponent->SetGrabTurnMultiplier(1.0f);
		}
	}
	OnGrabFailed.Broadcast();
	OnGrabStateChanged.Broadcast(EPPGrabState::None, nullptr);
}

void UPPGrabberComponent::OnRep_GrabPresentation()
{
	ApplyIncomingGrabRagdoll(IncomingGrabber != nullptr);
	ApplyMovementPresentation();
	OnGrabStateChanged.Broadcast(GrabState, GrabTarget);
}

FPPGrabTargetData UPPGrabberComponent::FindBestTarget(const FVector& CameraForward) const
{
	FPPGrabTargetData BestTargetData;
	const AActor* OwnerActor = GetOwner();
	UWorld* World = GetWorld();
	if (!OwnerActor || !World)
	{
		return BestTargetData;
	}

	const FVector TraceStart = GetGrabOrigin();
	const FVector TraceEnd = TraceStart + CameraForward * SearchReach;
	FCollisionQueryParams QueryParams(SCENE_QUERY_STAT(PairPressureGrabSearch), false, OwnerActor);
	FCollisionObjectQueryParams ObjectQueryParams;
	ObjectQueryParams.AddObjectTypesToQuery(ECC_PhysicsBody);
	ObjectQueryParams.AddObjectTypesToQuery(ECC_WorldDynamic);
	ObjectQueryParams.AddObjectTypesToQuery(ECC_Pawn);

	TArray<FHitResult> CandidateHits;
	World->SweepMultiByObjectType(
		CandidateHits,
		TraceStart,
		TraceEnd,
		FQuat::Identity,
		ObjectQueryParams,
		FCollisionShape::MakeSphere(SearchRadius),
		QueryParams);

	TArray<AActor*> CandidateActors;
	for (const FHitResult& CandidateHit : CandidateHits)
	{
		CandidateActors.AddUnique(CandidateHit.GetActor());
	}

	// A second proximity volume catches small items directly beside or beneath the
	// mascot. The forward chest trace alone naturally passes over floor pickups.
	const float ClosePickupRadius = FMath::Max(SearchRadius * 2.0f, 145.0f);
	const FVector ClosePickupCenter = OwnerActor->GetActorLocation() + FVector(0.0f, 0.0f, 30.0f);
	TArray<FOverlapResult> CloseOverlaps;
	World->OverlapMultiByObjectType(
		CloseOverlaps,
		ClosePickupCenter,
		FQuat::Identity,
		ObjectQueryParams,
		FCollisionShape::MakeSphere(ClosePickupRadius),
		QueryParams);
	for (const FOverlapResult& CloseOverlap : CloseOverlaps)
	{
		CandidateActors.AddUnique(CloseOverlap.GetActor());
	}

	TSet<AActor*> EvaluatedActors;
	for (AActor* CandidateActor : CandidateActors)
	{
		if (!CandidateActor || EvaluatedActors.Contains(CandidateActor))
		{
			continue;
		}
		EvaluatedActors.Add(CandidateActor);

		FPPGrabTargetData CandidateData;
		const bool bCandidateValid = BuildTargetData(CandidateActor, TraceStart, CameraForward, CandidateData);
		if (bDrawGrabDebug)
		{
			const FVector DebugLocation = bCandidateValid ? FVector(CandidateData.GrabPoint) : CandidateActor->GetActorLocation();
			DrawDebugSphere(World, DebugLocation, 9.0f, 8, bCandidateValid ? FColor::Blue : FColor::Red, false, 1.0f);
			DrawDebugString(
				World,
				DebugLocation + FVector(0.0f, 0.0f, 18.0f),
				bCandidateValid
					? FString::Printf(TEXT("%s score %.1f"), *CandidateActor->GetName(), CandidateData.Score)
					: FString::Printf(TEXT("%s rejected"), *CandidateActor->GetName()),
				nullptr,
				bCandidateValid ? FColor::Cyan : FColor::Red,
				1.0f,
				false);
		}

		if (bCandidateValid && (!BestTargetData.IsValid() || CandidateData.Score > BestTargetData.Score))
		{
			BestTargetData = CandidateData;
		}
	}

	if (bDrawGrabDebug)
	{
		DrawDebugSphere(World, ClosePickupCenter, ClosePickupRadius, 20, FColor::Cyan, false, 1.0f);
		DrawDebugCapsule(
			World,
			(TraceStart + TraceEnd) * 0.5f,
			FVector::Distance(TraceStart, TraceEnd) * 0.5f,
			SearchRadius,
			FQuat::FindBetweenNormals(FVector::UpVector, CameraForward),
			BestTargetData.IsValid() ? FColor::Green : FColor::Red,
			false,
			1.0f);
		if (BestTargetData.IsValid())
		{
			DrawDebugSphere(World, BestTargetData.GrabPoint, 16.0f, 12, FColor::Yellow, false, 1.0f);
		}
	}

	return BestTargetData;
}

UPPGrabbableComponent* UPPGrabberComponent::FindGrabbableComponent(AActor* CandidateActor) const
{
	return CandidateActor ? CandidateActor->FindComponentByClass<UPPGrabbableComponent>() : nullptr;
}

bool UPPGrabberComponent::ImplementsBlueprintGrabbableContract(AActor* CandidateActor) const
{
	if (!CandidateActor)
	{
		return false;
	}

	static UClass* GrabbableContractClass = LoadObject<UClass>(
		nullptr,
		TEXT("/Game/PairPressure/Core/Interfaces/BPI_PP_GrabbableContract.BPI_PP_GrabbableContract_C"));
	static UClass* GrabbableInterfaceClass = LoadObject<UClass>(
		nullptr,
		TEXT("/Game/PairPressure/Core/Interfaces/BPI_PP_Grabbable.BPI_PP_Grabbable_C"));
	return (GrabbableContractClass && CandidateActor->GetClass()->ImplementsInterface(GrabbableContractClass))
		|| (GrabbableInterfaceClass && CandidateActor->GetClass()->ImplementsInterface(GrabbableInterfaceClass));
}

bool UPPGrabberComponent::IsBlueprintGrabEnabled(AActor* CandidateActor) const
{
	if (!CandidateActor)
	{
		return false;
	}

	if (const FBoolProperty* CanGrabProperty = FindFProperty<FBoolProperty>(CandidateActor->GetClass(), TEXT("bCanGrab")))
	{
		return CanGrabProperty->GetPropertyValue_InContainer(CandidateActor);
	}
	if (UFunction* CanGrabFunction = CandidateActor->FindFunction(TEXT("CanGrab")))
	{
		TArray<uint8, TInlineAllocator<128>> ParameterMemory;
		ParameterMemory.SetNumZeroed(CanGrabFunction->ParmsSize);
		for (TFieldIterator<FProperty> PropertyIterator(CanGrabFunction); PropertyIterator; ++PropertyIterator)
		{
			FProperty* ParameterProperty = *PropertyIterator;
			if (!ParameterProperty->HasAnyPropertyFlags(CPF_Parm) || ParameterProperty->HasAnyPropertyFlags(CPF_ReturnParm))
			{
				continue;
			}
			if (FObjectPropertyBase* ObjectParameter = CastField<FObjectPropertyBase>(ParameterProperty))
			{
				ObjectParameter->SetObjectPropertyValue_InContainer(ParameterMemory.GetData(), GetOwner());
			}
		}
		CandidateActor->ProcessEvent(CanGrabFunction, ParameterMemory.GetData());
		for (TFieldIterator<FProperty> PropertyIterator(CanGrabFunction); PropertyIterator; ++PropertyIterator)
		{
			if (const FBoolProperty* ReturnProperty = CastField<FBoolProperty>(*PropertyIterator);
				ReturnProperty && ReturnProperty->HasAnyPropertyFlags(CPF_ReturnParm))
			{
				return ReturnProperty->GetPropertyValue_InContainer(ParameterMemory.GetData());
			}
		}
	}

	return true;
}

FPPGrabProfile UPPGrabberComponent::GetBlueprintGrabProfile(AActor* CandidateActor) const
{
	FPPGrabProfile ResultProfile;
	if (!CandidateActor)
	{
		return ResultProfile;
	}

	bool bHasAuthoredProfile = false;
	if (const FStructProperty* ProfileProperty = FindFProperty<FStructProperty>(CandidateActor->GetClass(), TEXT("GrabProfile"));
		ProfileProperty && ProfileProperty->Struct == FPPGrabProfile::StaticStruct())
	{
		bHasAuthoredProfile = true;
		FPPGrabProfile::StaticStruct()->CopyScriptStruct(
			&ResultProfile,
			ProfileProperty->ContainerPtrToValuePtr<void>(CandidateActor));
	}
	if (CandidateActor->ActorHasTag(TEXT("PP.Grab.Player")))
	{
		ResultProfile.TargetType = EPPGrabTargetType::Player;
	}
	else if (CandidateActor->ActorHasTag(TEXT("PP.Grab.Ledge")) || CandidateActor->ActorHasTag(TEXT("PP.Grab.Handle")))
	{
		ResultProfile.TargetType = EPPGrabTargetType::LedgeOrHandle;
		if (!bHasAuthoredProfile)
		{
			ResultProfile.MaximumRange = 190.0f;
			ResultProfile.BreakForce = 125000.0f;
			ResultProfile.MovementSpeedMultiplier = 0.35f;
		}
	}
	else if (CandidateActor->ActorHasTag(TEXT("PP.Grab.Pushable")))
	{
		ResultProfile.TargetType = EPPGrabTargetType::LargePushable;
		if (!bHasAuthoredProfile)
		{
			ResultProfile.MaximumMass = 500.0f;
			ResultProfile.BreakForce = 165000.0f;
			ResultProfile.MovementSpeedMultiplier = 0.6f;
			ResultProfile.CarryDistance = 90.0f;
		}
	}
	return ResultProfile;
}

void UPPGrabberComponent::NotifyBlueprintGrabEvent(AActor* CandidateActor, bool bGrabStarted) const
{
	if (!CandidateActor || !ImplementsBlueprintGrabbableContract(CandidateActor))
	{
		return;
	}

	const FName PreferredEventName = bGrabStarted ? FName(TEXT("OnGrabbed")) : FName(TEXT("OnGrabReleased"));
	UFunction* GrabEvent = CandidateActor->FindFunction(PreferredEventName);
	if (!GrabEvent && !bGrabStarted)
	{
		GrabEvent = CandidateActor->FindFunction(TEXT("OnReleased"));
	}
	if (!GrabEvent)
	{
		return;
	}

	TArray<uint8, TInlineAllocator<128>> ParameterMemory;
	ParameterMemory.SetNumZeroed(GrabEvent->ParmsSize);
	for (TFieldIterator<FProperty> PropertyIterator(GrabEvent); PropertyIterator; ++PropertyIterator)
	{
		FProperty* ParameterProperty = *PropertyIterator;
		if (!ParameterProperty->HasAnyPropertyFlags(CPF_Parm) || ParameterProperty->HasAnyPropertyFlags(CPF_ReturnParm))
		{
			continue;
		}
		if (FObjectPropertyBase* ObjectParameter = CastField<FObjectPropertyBase>(ParameterProperty))
		{
			ObjectParameter->SetObjectPropertyValue_InContainer(ParameterMemory.GetData(), GetOwner());
		}
	}
	CandidateActor->ProcessEvent(GrabEvent, ParameterMemory.GetData());
}

bool UPPGrabberComponent::BuildTargetData(
	AActor* CandidateActor,
	const FVector& TraceStart,
	const FVector& CameraForward,
	FPPGrabTargetData& OutTargetData) const
{
	AActor* OwnerActor = GetOwner();
	if (IncomingGrabber && CandidateActor != IncomingGrabber)
	{
		return false;
	}
	UPPGrabbableComponent* GrabbableComponent = FindGrabbableComponent(CandidateActor);
	const bool bUsesBlueprintContract = ImplementsBlueprintGrabbableContract(CandidateActor);
	if (!OwnerActor || (!GrabbableComponent && !bUsesBlueprintContract)
		|| (GrabbableComponent && !IPPGrabbable::Execute_CanBeGrabbed(GrabbableComponent, OwnerActor))
		|| (!GrabbableComponent && bUsesBlueprintContract && !IsBlueprintGrabEnabled(CandidateActor)))
	{
		return false;
	}

	const FPPGrabProfile CandidateProfile = GrabbableComponent
		? IPPGrabbable::Execute_GetGrabProfile(GrabbableComponent)
		: GetBlueprintGrabProfile(CandidateActor);
	if (CandidateProfile.TargetType == EPPGrabTargetType::None)
	{
		return false;
	}
	if (CandidateProfile.TargetType == EPPGrabTargetType::Player)
	{
		if (!IsStandingPlayerStateValid(CandidateActor))
		{
			return false;
		}
		if (const UPPGrabberComponent* TargetGrabber = CandidateActor->FindComponentByClass<UPPGrabberComponent>();
			TargetGrabber && TargetGrabber->ImmuneGrabber.Get() == OwnerActor && GetWorld()
			&& GetWorld()->GetTimeSeconds() < TargetGrabber->ImmunityEndTimeSeconds)
		{
			return false;
		}
	}

	UPrimitiveComponent* CandidatePrimitive = GrabbableComponent
		? IPPGrabbable::Execute_GetGrabPrimitive(GrabbableComponent)
		: Cast<UPrimitiveComponent>(CandidateActor->GetRootComponent());
	if (!CandidatePrimitive)
	{
		TInlineComponentArray<UPrimitiveComponent*> CandidatePrimitives;
		CandidateActor->GetComponents(CandidatePrimitives);
		for (UPrimitiveComponent* PrimitiveComponent : CandidatePrimitives)
		{
			if (PrimitiveComponent && PrimitiveComponent->IsSimulatingPhysics())
			{
				CandidatePrimitive = PrimitiveComponent;
				break;
			}
		}
	}
	const bool bRequiresSimulatedPrimitive = CandidateProfile.TargetType == EPPGrabTargetType::GameplayItem
		|| CandidateProfile.TargetType == EPPGrabTargetType::LargePushable;
	if (!CandidatePrimitive
		|| (bRequiresSimulatedPrimitive && !CandidatePrimitive->IsSimulatingPhysics())
		|| (bRequiresSimulatedPrimitive && CandidatePrimitive->GetMass() > CandidateProfile.MaximumMass))
	{
		return false;
	}

	FVector CandidateGrabPoint = CandidatePrimitive->GetCenterOfMass();
	if (GrabbableComponent)
	{
		CandidateGrabPoint = IPPGrabbable::Execute_GetGrabPoint(GrabbableComponent, OwnerActor);
	}
	else
	{
		TInlineComponentArray<USceneComponent*> CandidateSceneComponents;
		CandidateActor->GetComponents(CandidateSceneComponents);
		for (const USceneComponent* SceneComponent : CandidateSceneComponents)
		{
			if (SceneComponent && SceneComponent->ComponentHasTag(TEXT("GripPoint")))
			{
				CandidateGrabPoint = SceneComponent->GetComponentLocation();
				break;
			}
		}
	}
	const FVector ToCandidate = CandidateGrabPoint - TraceStart;
	const float CandidateDistance = ToCandidate.Size();
	if (CandidateDistance > FMath::Min(SearchReach, CandidateProfile.MaximumRange) || CandidateDistance <= UE_KINDA_SMALL_NUMBER)
	{
		return false;
	}

	const FVector DirectionToCandidate = ToCandidate / CandidateDistance;
	const float FacingAlignment = FVector::DotProduct(OwnerActor->GetActorForwardVector(), DirectionToCandidate);
	const float CameraAlignment = FVector::DotProduct(CameraForward, DirectionToCandidate);
	const FVector HorizontalOffset = FVector(ToCandidate.X, ToCandidate.Y, 0.0f);
	const float HorizontalDistance = HorizontalOffset.Size();
	const float ClosePickupRadius = FMath::Max(SearchRadius * 2.0f, 145.0f);
	const bool bCloseGameplayItem = CandidateProfile.TargetType == EPPGrabTargetType::GameplayItem
		&& HorizontalDistance <= ClosePickupRadius
		&& FMath::Abs(ToCandidate.Z) <= ClosePickupRadius;
	const float HorizontalFacingAlignment = HorizontalDistance <= UE_KINDA_SMALL_NUMBER
		? 1.0f
		: FVector::DotProduct(OwnerActor->GetActorForwardVector().GetSafeNormal2D(), HorizontalOffset / HorizontalDistance);
	if ((!bCloseGameplayItem
			&& (FacingAlignment <= 0.0f
				|| CameraAlignment < FMath::Cos(FMath::DegreesToRadians(CandidateProfile.MaximumAngleDegrees))))
		|| (bCloseGameplayItem && HorizontalFacingAlignment < -0.4f))
	{
		return false;
	}

	OutTargetData.TargetActor = CandidateActor;
	OutTargetData.TargetPrimitive = CandidatePrimitive;
	OutTargetData.GrabPoint = CandidateGrabPoint;
	OutTargetData.Profile = CandidateProfile;
	if (!HasClearLineOfSight(TraceStart, OutTargetData))
	{
		OutTargetData = FPPGrabTargetData();
		return false;
	}

	const int32 CandidatePriority = CandidateProfile.PriorityOverride >= 0
		? CandidateProfile.PriorityOverride
		: GetDefaultPriority(CandidateProfile.TargetType);
	const float NormalizedDistance = CandidateDistance / FMath::Max(CandidateProfile.MaximumRange, 1.0f);
	OutTargetData.Score = CameraAlignment * PPGrabAlignmentScoreWeight
		+ static_cast<float>(CandidatePriority) * PPGrabPriorityScoreWeight
		- NormalizedDistance * PPGrabDistanceScoreWeight
		+ PPGrabLineOfSightScore
		+ (bCloseGameplayItem ? 300.0f : 0.0f);
	return true;
}

bool UPPGrabberComponent::HasClearLineOfSight(const FVector& TraceStart, const FPPGrabTargetData& TargetData) const
{
	const UWorld* World = GetWorld();
	if (!World || !TargetData.TargetActor)
	{
		return false;
	}

	FCollisionQueryParams QueryParams(SCENE_QUERY_STAT(PairPressureGrabLineOfSight), true, GetOwner());
	QueryParams.AddIgnoredActor(TargetData.TargetActor);
	FHitResult BlockingHit;
	return !World->LineTraceSingleByChannel(BlockingHit, TraceStart, TargetData.GrabPoint, ECC_Visibility, QueryParams);
}

void UPPGrabberComponent::StartGrabAuthoritative(const FPPGrabTargetData& TargetData)
{
	if (!GetOwner() || !GetOwner()->HasAuthority() || !TargetData.IsValid())
	{
		SetGrabPresentation(EPPGrabState::None, nullptr);
		ClientGrabRejected();
		return;
	}

	ActiveProfile = TargetData.Profile;
	if (TargetData.TargetActor
		&& TargetData.TargetActor->GetClass()->GetPathName().Contains(TEXT("BP_PP_GrabTestItem")))
	{
		// Keep the Lobby proof object deliberately easy to test without changing
		// authored profiles for real gameplay items.
		ActiveProfile.MaximumRange = 250.0f;
		ActiveProfile.MaximumAngleDegrees = 60.0f;
		ActiveProfile.CarryDistance = 120.0f;
		ActiveProfile.LinearStiffness = 1600.0f;
		ActiveProfile.LinearDamping = 320.0f;
		ActiveProfile.AngularStiffness = 550.0f;
		ActiveProfile.AngularDamping = 120.0f;
		ActiveProfile.BreakForce = 300000.0f;
	}
	GrabbedPrimitive = TargetData.TargetPrimitive;
	if (ActiveProfile.TargetType == EPPGrabTargetType::LargePushable && GrabbedPrimitive)
	{
		const float MassMultiplier = FMath::GetMappedRangeValueClamped(
			FVector2D(20.0f, FMath::Max(ActiveProfile.MaximumMass, 21.0f)),
			FVector2D(0.85f, 0.35f),
			GrabbedPrimitive->GetMass());
		ActiveProfile.MovementSpeedMultiplier = FMath::Min(ActiveProfile.MovementSpeedMultiplier, MassMultiplier);
	}
	else if (ActiveProfile.TargetType == EPPGrabTargetType::GameplayItem && ActiveProfile.bRequiresTwoHands)
	{
		ActiveProfile.MovementSpeedMultiplier = FMath::Min(ActiveProfile.MovementSpeedMultiplier, 0.62f);
	}
	PresentationGrabPoint = TargetData.GrabPoint;
	FixedWorldGrabPoint = TargetData.GrabPoint;
	SustainedGrabSeconds = 0.0f;

	if (ActiveProfile.TargetType == EPPGrabTargetType::Player)
	{
		UPPGrabberComponent* TargetGrabber = TargetData.TargetActor->FindComponentByClass<UPPGrabberComponent>();
		const bool bIsReciprocalGrab = TargetGrabber
			&& TargetGrabber->GrabTarget == GetOwner()
			&& TargetGrabber->GrabState == EPPGrabState::GrabbingPlayer;
		if (bIsReciprocalGrab)
		{
			CancelReciprocalGrab(TargetGrabber);
			return;
		}
	}

	if (ActiveProfile.TargetType == EPPGrabTargetType::GameplayItem
		|| ActiveProfile.TargetType == EPPGrabTargetType::LargePushable)
	{
		if (!PhysicsHandle)
		{
			SetGrabPresentation(EPPGrabState::None, nullptr);
			ClientGrabRejected();
			return;
		}
		PhysicsHandle->SetLinearStiffness(ActiveProfile.LinearStiffness);
		PhysicsHandle->SetLinearDamping(ActiveProfile.LinearDamping);
		PhysicsHandle->SetAngularStiffness(ActiveProfile.AngularStiffness);
		PhysicsHandle->SetAngularDamping(ActiveProfile.AngularDamping);
		PhysicsHandle->SetInterpolationSpeed(18.0f);
		if (ActiveProfile.bMaintainRotation)
		{
			PhysicsHandle->GrabComponentAtLocationWithRotation(
				GrabbedPrimitive,
				NAME_None,
				TargetData.GrabPoint,
				GrabbedPrimitive->GetComponentRotation());
		}
		else
		{
			PhysicsHandle->GrabComponentAtLocation(GrabbedPrimitive, NAME_None, TargetData.GrabPoint);
		}

		if (!PhysicsHandle->GetGrabbedComponent())
		{
			GrabbedPrimitive = nullptr;
			SetGrabPresentation(EPPGrabState::None, nullptr);
			ClientGrabRejected();
			return;
		}
		PhysicsHandle->SetTargetLocation(
			GetHandCarryLocation(ActiveProfile.CarryDistance));
	}
	else if (ActiveProfile.TargetType == EPPGrabTargetType::Player)
	{
		ACharacter* TargetCharacter = Cast<ACharacter>(TargetData.TargetActor);
		USkeletalMeshComponent* TargetMesh = TargetCharacter ? TargetCharacter->GetMesh() : nullptr;
		UPPGrabberComponent* TargetGrabber = TargetData.TargetActor->FindComponentByClass<UPPGrabberComponent>();
		if (!PhysicsHandle || !TargetMesh || !TargetGrabber)
		{
			GrabbedPrimitive = nullptr;
			SetGrabPresentation(EPPGrabState::None, nullptr);
			ClientGrabRejected();
			return;
		}

		TargetGrabber->BeginIncomingPlayerGrab(GetOwner());
		ActivePlayerGrabBone = ResolvePlayerGrabBone(TargetMesh);
		const FVector PlayerGrabLocation = ActivePlayerGrabBone.IsNone()
			? TargetMesh->GetComponentLocation()
			: TargetMesh->GetBoneLocation(ActivePlayerGrabBone);
		PhysicsHandle->SetLinearStiffness(ActiveProfile.LinearStiffness);
		PhysicsHandle->SetLinearDamping(ActiveProfile.LinearDamping);
		PhysicsHandle->SetAngularStiffness(ActiveProfile.AngularStiffness);
		PhysicsHandle->SetAngularDamping(ActiveProfile.AngularDamping);
		PhysicsHandle->SetInterpolationSpeed(14.0f);
		PhysicsHandle->GrabComponentAtLocation(TargetMesh, ActivePlayerGrabBone, PlayerGrabLocation);
		if (!PhysicsHandle->GetGrabbedComponent())
		{
			TargetGrabber->EndIncomingPlayerGrab(GetOwner(), false);
			ActivePlayerGrabBone = NAME_None;
			GrabbedPrimitive = nullptr;
			SetGrabPresentation(EPPGrabState::None, nullptr);
			ClientGrabRejected();
			return;
		}
		PhysicsHandle->SetTargetLocation(
			GetHandCarryLocation(PlayerHoldDistance));
		GrabbedPrimitive = TargetMesh;
		PresentationGrabPoint = PlayerGrabLocation;
		UE_LOG(LogVNH, Display, TEXT("PairPressureGrab: player hold started. Grabber=%s Target=%s Bone=%s"),
			*GetNameSafe(GetOwner()),
			*GetNameSafe(TargetCharacter),
			*ActivePlayerGrabBone.ToString());
	}

	if (UPPGrabbableComponent* GrabbableComponent = FindGrabbableComponent(TargetData.TargetActor))
	{
		IPPGrabbable::Execute_OnGrabStarted(GrabbableComponent, GetOwner());
	}
	else
	{
		NotifyBlueprintGrabEvent(TargetData.TargetActor, true);
	}

	switch (ActiveProfile.TargetType)
	{
	case EPPGrabTargetType::GameplayItem:
		SetGrabPresentation(EPPGrabState::HoldingItem, TargetData.TargetActor);
		UE_LOG(LogVNH, Display, TEXT("PairPressureGrab: item hold started. Grabber=%s Target=%s Mass=%.1f"),
			*GetNameSafe(GetOwner()),
			*GetNameSafe(TargetData.TargetActor),
			GrabbedPrimitive ? GrabbedPrimitive->GetMass() : 0.0f);
		break;
	case EPPGrabTargetType::Player:
	{
		ForceDropTargetItem(TargetData.TargetActor);
		SetGrabPresentation(EPPGrabState::GrabbingPlayer, TargetData.TargetActor);
		break;
	}
	case EPPGrabTargetType::LargePushable:
		SetGrabPresentation(EPPGrabState::PushingObject, TargetData.TargetActor);
		break;
	case EPPGrabTargetType::LedgeOrHandle:
		if (ACharacter* OwnerCharacter = Cast<ACharacter>(GetOwner()))
		{
			if (UCharacterMovementComponent* OwnerMovement = OwnerCharacter->GetCharacterMovement())
			{
				OwnerMovement->SetMovementMode(MOVE_Falling);
			}
		}
		SetGrabPresentation(EPPGrabState::HangingFromLedge, TargetData.TargetActor);
		break;
	case EPPGrabTargetType::None:
	default:
		SetGrabPresentation(EPPGrabState::None, nullptr);
		ClientGrabRejected();
		return;
	}
	SetComponentTickEnabled(true);
}

void UPPGrabberComponent::ReleaseGrabAuthoritative(bool bPlayRecoveryAnimation)
{
	if (!GetOwner() || !GetOwner()->HasAuthority())
	{
		return;
	}

	AActor* PreviousGrabTarget = GrabTarget;
	const EPPGrabState PreviousGrabState = GrabState;
	const bool bWasPairedRelease = bReleasingPairedGrab;
	bReleasingPairedGrab = true;
	SetGrabPresentation(EPPGrabState::Releasing, PreviousGrabTarget);
	if (PhysicsHandle)
	{
		PhysicsHandle->ReleaseComponent();
	}
	if (UPPGrabbableComponent* GrabbableComponent = FindGrabbableComponent(PreviousGrabTarget))
	{
		IPPGrabbable::Execute_OnGrabEnded(GrabbableComponent, GetOwner());
	}
	else
	{
		NotifyBlueprintGrabEvent(PreviousGrabTarget, false);
	}
	if ((PreviousGrabState == EPPGrabState::GrabbingPlayer || PreviousGrabState == EPPGrabState::MutualGrab) && PreviousGrabTarget)
	{
		if (UPPGrabberComponent* TargetGrabber = PreviousGrabTarget->FindComponentByClass<UPPGrabberComponent>())
		{
			TargetGrabber->EndIncomingPlayerGrab(GetOwner(), true);
			if (bPlayRecoveryAnimation)
			{
				PlayGetUpFrontAnimation(Cast<ACharacter>(PreviousGrabTarget));
			}
			if (PreviousGrabState == EPPGrabState::MutualGrab && !bWasPairedRelease
				&& TargetGrabber->GrabState == EPPGrabState::MutualGrab && TargetGrabber->GrabTarget == GetOwner())
			{
				TargetGrabber->bReleasingPairedGrab = true;
				TargetGrabber->ReleaseGrabAuthoritative(false);
			}
		}
	}

	GrabbedPrimitive = nullptr;
	PresentationGrabPoint = FVector::ZeroVector;
	FixedWorldGrabPoint = FVector::ZeroVector;
	ActivePlayerGrabBone = NAME_None;
	ConstraintForceEstimate = 0.0f;
	SustainedGrabSeconds = 0.0f;
	SetComponentTickEnabled(false);
	SetGrabPresentation(EPPGrabState::None, nullptr);
	bReleasingPairedGrab = false;
}

void UPPGrabberComponent::PerformHeldItemThrow(const FVector& ThrowDirection, float ChargeAlpha)
{
	AActor* OwnerActor = GetOwner();
	if (!OwnerActor || !OwnerActor->HasAuthority()
		|| (GrabState != EPPGrabState::HoldingItem && GrabState != EPPGrabState::GrabbingPlayer)
		|| !GrabbedPrimitive)
	{
		return;
	}

	FVector ValidatedThrowDirection = ThrowDirection.GetSafeNormal();
	const FVector OwnerForward = OwnerActor->GetActorForwardVector().GetSafeNormal2D();
	if (ValidatedThrowDirection.IsNearlyZero()
		|| FVector::DotProduct(ValidatedThrowDirection.GetSafeNormal2D(), OwnerForward) < 0.2f)
	{
		ValidatedThrowDirection = (OwnerForward + FVector::UpVector * 0.18f).GetSafeNormal();
	}

	const bool bThrowingPlayer = GrabState == EPPGrabState::GrabbingPlayer;
	ACharacter* ThrownCharacter = bThrowingPlayer ? Cast<ACharacter>(GrabTarget) : nullptr;
	USkeletalMeshComponent* ThrownCharacterMesh = ThrownCharacter ? ThrownCharacter->GetMesh() : nullptr;
	UPrimitiveComponent* ThrownPrimitive = GrabbedPrimitive;
	const FVector InheritedVelocity = OwnerActor->GetVelocity();
	const float ThrowSpeed = FMath::Lerp(HeldItemThrowSpeed * 0.55f, HeldItemThrowSpeed * 2.15f, FMath::Clamp(ChargeAlpha, 0.0f, 1.0f));
	ReleaseGrabAuthoritative(false);
	if (ThrownCharacter)
	{
		if (UPPPhysicalStateComponent* PhysicalState = UPPPhysicalStateComponent::FindPhysicalStateComponent(ThrownCharacter))
		{
			IPPPhysicalStateInterface::Execute_RequestRagdoll(PhysicalState, 0.0f);
		}
		const FVector PlayerThrowVelocity = InheritedVelocity + ValidatedThrowDirection * (ThrowSpeed * 0.8f);
		if (ThrownCharacterMesh && ThrownCharacterMesh->IsAnySimulatingPhysics())
		{
			ThrownCharacterMesh->SetAllPhysicsLinearVelocity(PlayerThrowVelocity, false);
			ThrownCharacterMesh->WakeAllRigidBodies();
		}
		else
		{
			ThrownCharacter->LaunchCharacter(PlayerThrowVelocity, true, true);
		}
		return;
	}
	if (ThrownPrimitive && ThrownPrimitive->IsSimulatingPhysics())
	{
		ThrownPrimitive->SetPhysicsLinearVelocity(InheritedVelocity + ValidatedThrowDirection * ThrowSpeed);
		ThrownPrimitive->WakeAllRigidBodies();
	}
}

void UPPGrabberComponent::PerformDirectionalEscape(const FVector& EscapeDirection)
{
	AActor* OwnerActor = GetOwner();
	AActor* CurrentIncomingGrabber = IncomingGrabber;
	if (!OwnerActor || !OwnerActor->HasAuthority() || !CurrentIncomingGrabber)
	{
		return;
	}

	const FVector ValidatedEscapeDirection = EscapeDirection.GetSafeNormal2D();
	FVector EscapingPlayerLocation = OwnerActor->GetActorLocation();
	if (const ACharacter* OwnerCharacter = Cast<ACharacter>(OwnerActor))
	{
		if (const USkeletalMeshComponent* OwnerMesh = OwnerCharacter->GetMesh())
		{
			const FName EscapeAnchorBone = ResolvePlayerGrabBone(OwnerMesh);
			EscapingPlayerLocation = EscapeAnchorBone.IsNone()
				? OwnerMesh->GetComponentLocation()
				: OwnerMesh->GetBoneLocation(EscapeAnchorBone);
		}
	}
	const FVector TowardGrabber = (CurrentIncomingGrabber->GetActorLocation() - EscapingPlayerLocation).GetSafeNormal2D();
	if (ValidatedEscapeDirection.IsNearlyZero() || TowardGrabber.IsNearlyZero()
		|| FVector::DotProduct(ValidatedEscapeDirection, TowardGrabber) > EscapeBlockedTowardDot)
	{
		return;
	}

	UPPGrabberComponent* IncomingGrabberComponent = CurrentIncomingGrabber->FindComponentByClass<UPPGrabberComponent>();
	if (!IncomingGrabberComponent || IncomingGrabberComponent->GrabTarget != OwnerActor
		|| IncomingGrabberComponent->GrabState != EPPGrabState::GrabbingPlayer)
	{
		return;
	}

	IncomingGrabberComponent->ApplyBreakFreePenalty(OwnerActor);
	IncomingGrabberComponent->ReleaseGrabAuthoritative(false);
	if (ACharacter* OwnerCharacter = Cast<ACharacter>(OwnerActor))
	{
		const FVector EscapeVelocity = ValidatedEscapeDirection * EscapeJumpHorizontalSpeed
			+ FVector::UpVector * EscapeJumpVerticalSpeed;
		OwnerCharacter->LaunchCharacter(EscapeVelocity, true, true);
	}
}

void UPPGrabberComponent::UpdateReachSearch(float DeltaTime)
{
	SustainedGrabSeconds += DeltaTime;
	if (SustainedGrabSeconds < 0.08f)
	{
		return;
	}
	SustainedGrabSeconds = 0.0f;

	const FPPGrabTargetData TargetData = FindBestTarget(LastValidatedCameraForward);
	if (TargetData.IsValid())
	{
		StartGrabAuthoritative(TargetData);
	}
}

void UPPGrabberComponent::CancelReciprocalGrab(UPPGrabberComponent* OtherGrabber)
{
	AActor* OwnerActor = GetOwner();
	if (!OwnerActor || !OwnerActor->HasAuthority() || !OtherGrabber || !GetWorld())
	{
		return;
	}

	AActor* OtherOwner = OtherGrabber->GetOwner();
	OtherGrabber->ReleaseGrabAuthoritative(false);
	ImmuneGrabber = OtherOwner;
	ImmunityEndTimeSeconds = GetWorld()->GetTimeSeconds() + SameOpponentImmunitySeconds;
	OtherGrabber->ImmuneGrabber = OwnerActor;
	OtherGrabber->ImmunityEndTimeSeconds = GetWorld()->GetTimeSeconds() + OtherGrabber->SameOpponentImmunitySeconds;
	ActiveProfile = FPPGrabProfile();
	GrabbedPrimitive = nullptr;
	PresentationGrabPoint = FVector::ZeroVector;
	FixedWorldGrabPoint = FVector::ZeroVector;
	ConstraintForceEstimate = 0.0f;
	SustainedGrabSeconds = 0.0f;
	SetComponentTickEnabled(false);
	SetGrabPresentation(EPPGrabState::None, nullptr);
	ClientGrabRejected();
}

void UPPGrabberComponent::UpdateHeldItem(float DeltaTime)
{
	if (!PhysicsHandle || !GrabbedPrimitive || !GrabTarget || PhysicsHandle->GetGrabbedComponent() != GrabbedPrimitive
		|| IncomingGrabber || !IsStandingPlayerStateValid(GetOwner()))
	{
		UE_LOG(LogVNH, Warning, TEXT("PairPressureGrab: item hold released by invalid state. Target=%s Handle=%s Primitive=%s"),
			*GetNameSafe(GrabTarget),
			*GetNameSafe(PhysicsHandle ? PhysicsHandle->GetGrabbedComponent() : nullptr),
			*GetNameSafe(GrabbedPrimitive));
		ReleaseGrabAuthoritative();
		return;
	}
	SustainedGrabSeconds += DeltaTime;
	const bool bConstraintSettled = SustainedGrabSeconds >= 0.35f;

	const FVector DesiredLocation = GetHandCarryLocation(ActiveProfile.CarryDistance);
	const FVector ConstraintError = DesiredLocation - GrabbedPrimitive->GetCenterOfMass();
	FPPGrabTargetData CurrentTargetData;
	CurrentTargetData.TargetActor = GrabTarget;
	CurrentTargetData.TargetPrimitive = GrabbedPrimitive;
	CurrentTargetData.GrabPoint = GrabbedPrimitive->GetCenterOfMass();
	PresentationGrabPoint = CurrentTargetData.GrabPoint;
	if (bConstraintSettled && !HasClearLineOfSight(GetGrabOrigin(), CurrentTargetData))
	{
		UE_LOG(LogVNH, Warning, TEXT("PairPressureGrab: item hold released by line of sight. Target=%s"), *GetNameSafe(GrabTarget));
		ReleaseGrabAuthoritative(false);
		return;
	}
	ConstraintForceEstimate = ConstraintError.Size() * ActiveProfile.LinearStiffness;
	if (bDrawGrabDebug && GetWorld())
	{
		DrawDebugLine(GetWorld(), GrabbedPrimitive->GetCenterOfMass(), DesiredLocation, FColor::Purple, false, 0.05f, 0, 2.0f);
		DrawDebugString(
			GetWorld(),
			DesiredLocation + FVector(0.0f, 0.0f, 25.0f),
			FString::Printf(
				TEXT("State HoldingItem | Force %.0f / %.0f | Immunity 0.00"),
				ConstraintForceEstimate,
				ActiveProfile.BreakForce),
			nullptr,
			FColor::White,
			0.05f,
			false);
	}
	const float BreakDistance = FMath::Max(ActiveProfile.MaximumRange * 1.65f, ActiveProfile.CarryDistance + 150.0f);
	const float EffectiveBreakForce = FMath::Max(ActiveProfile.BreakForce, 250000.0f);
	if (bConstraintSettled && (ConstraintError.Size() > BreakDistance || ConstraintForceEstimate > EffectiveBreakForce))
	{
		UE_LOG(LogVNH, Warning, TEXT("PairPressureGrab: item hold released by constraint. Target=%s Distance=%.1f Force=%.0f/%.0f"),
			*GetNameSafe(GrabTarget), ConstraintError.Size(), ConstraintForceEstimate, EffectiveBreakForce);
		ReleaseGrabAuthoritative(false);
		return;
	}

	PhysicsHandle->SetTargetLocation(DesiredLocation);
	if (ActiveProfile.bMaintainRotation)
	{
		PhysicsHandle->SetTargetRotation(GetOwner()->GetActorRotation());
	}
}

void UPPGrabberComponent::UpdatePlayerGrab(float DeltaTime)
{
	ACharacter* TargetCharacter = Cast<ACharacter>(GrabTarget);
	USkeletalMeshComponent* TargetMesh = TargetCharacter ? TargetCharacter->GetMesh() : nullptr;
	if (!TargetCharacter || !TargetMesh || !PhysicsHandle || PhysicsHandle->GetGrabbedComponent() != TargetMesh
		|| !IsStandingPlayerStateValid(GetOwner())
		|| !IsStandingPlayerStateValid(TargetCharacter))
	{
		UE_LOG(LogVNH, Warning, TEXT("PairPressureGrab: player hold released by invalid state. Grabber=%s Target=%s Handle=%s"),
			*GetNameSafe(GetOwner()),
			*GetNameSafe(TargetCharacter),
			*GetNameSafe(PhysicsHandle ? PhysicsHandle->GetGrabbedComponent() : nullptr));
		ReleaseGrabAuthoritative(false);
		return;
	}
	SustainedGrabSeconds += DeltaTime;
	const bool bConstraintSettled = SustainedGrabSeconds >= 0.30f;

	const FVector CurrentGrabPoint = ActivePlayerGrabBone.IsNone()
		? TargetMesh->GetComponentLocation()
		: TargetMesh->GetBoneLocation(ActivePlayerGrabBone);
	FPPGrabTargetData CurrentTargetData;
	CurrentTargetData.TargetActor = TargetCharacter;
	CurrentTargetData.TargetPrimitive = TargetMesh;
	CurrentTargetData.GrabPoint = CurrentGrabPoint;
	const FVector GrabOrigin = GetGrabOrigin();
	const float CurrentDistance = FVector::Distance(GrabOrigin, CurrentGrabPoint);
	if (bConstraintSettled && CurrentDistance > ActiveProfile.MaximumRange * 1.65f)
	{
		UE_LOG(LogVNH, Warning, TEXT("PairPressureGrab: player hold released by distance %.1f."), CurrentDistance);
		ReleaseGrabAuthoritative(false);
		return;
	}
	if (bConstraintSettled && !HasClearLineOfSight(GrabOrigin, CurrentTargetData))
	{
		UE_LOG(LogVNH, Warning, TEXT("PairPressureGrab: player hold released by line of sight."));
		ReleaseGrabAuthoritative(false);
		return;
	}

	const FVector DesiredTargetLocation = GetHandCarryLocation(PlayerHoldDistance);
	const FVector RawError = DesiredTargetLocation - CurrentGrabPoint;
	PhysicsHandle->SetTargetLocation(DesiredTargetLocation);

	ConstraintForceEstimate = RawError.Size() * ActiveProfile.LinearStiffness;
	PresentationGrabPoint = CurrentTargetData.GrabPoint;
	const float EffectiveBreakForce = FMath::Max(ActiveProfile.BreakForce, 400000.0f)
		* (IsFriendlyPlayer(TargetCharacter) ? TeammateRescueStrengthMultiplier : 1.0f);
	if (bConstraintSettled && ConstraintForceEstimate > EffectiveBreakForce)
	{
		UE_LOG(LogVNH, Warning, TEXT("PairPressureGrab: player hold released by force %.0f / %.0f."),
			ConstraintForceEstimate,
			EffectiveBreakForce);
		ReleaseGrabAuthoritative(false);
		return;
	}

	if (OpponentEscapeSeconds > 0.0f && GrabState == EPPGrabState::GrabbingPlayer && !IsFriendlyPlayer(TargetCharacter)
		&& SustainedGrabSeconds >= OpponentEscapeSeconds)
	{
		ApplyBreakFreePenalty(TargetCharacter);
		ReleaseGrabAuthoritative(false);
		return;
	}

	if (bDrawGrabDebug && GetWorld())
	{
		const float ImmunityRemaining = FMath::Max(0.0, ImmunityEndTimeSeconds - GetWorld()->GetTimeSeconds());
		DrawDebugLine(GetWorld(), GrabOrigin, CurrentTargetData.GrabPoint, FColor::Orange, false, 0.05f, 0, 2.5f);
		DrawDebugString(
			GetWorld(),
			CurrentTargetData.GrabPoint + FVector(0.0f, 0.0f, 30.0f),
			FString::Printf(
				TEXT("State %s | Hold %.2f | Force %.0f / %.0f | Immunity %.2f"),
				GrabState == EPPGrabState::MutualGrab ? TEXT("MutualGrab") : TEXT("GrabbingPlayer"),
				SustainedGrabSeconds,
				ConstraintForceEstimate,
				ActiveProfile.BreakForce,
				ImmunityRemaining),
			nullptr,
			FColor::White,
			0.05f,
			false);
	}
}

void UPPGrabberComponent::UpdatePushable(float /*DeltaTime*/)
{
	if (!PhysicsHandle || !GrabbedPrimitive || !GrabTarget || !PhysicsHandle->GetGrabbedComponent()
		|| IncomingGrabber || !IsStandingPlayerStateValid(GetOwner()))
	{
		ReleaseGrabAuthoritative();
		return;
	}

	FVector DesiredLocation = GetGrabOrigin() + GetOwner()->GetActorForwardVector() * ActiveProfile.CarryDistance;
	DesiredLocation.Z = FixedWorldGrabPoint.Z;
	const FVector ConstraintError = DesiredLocation - GrabbedPrimitive->GetCenterOfMass();
	ConstraintForceEstimate = ConstraintError.Size() * ActiveProfile.LinearStiffness;
	FPPGrabTargetData CurrentTargetData;
	CurrentTargetData.TargetActor = GrabTarget;
	CurrentTargetData.TargetPrimitive = GrabbedPrimitive;
	CurrentTargetData.GrabPoint = GrabbedPrimitive->GetCenterOfMass();
	if (ConstraintError.Size() > ActiveProfile.MaximumRange * 1.5f
		|| ConstraintForceEstimate > ActiveProfile.BreakForce
		|| !HasClearLineOfSight(GetGrabOrigin(), CurrentTargetData))
	{
		ReleaseGrabAuthoritative();
		return;
	}

	PhysicsHandle->SetTargetLocation(DesiredLocation);
	PresentationGrabPoint = GrabbedPrimitive->GetCenterOfMass();
	if (bDrawGrabDebug && GetWorld())
	{
		DrawDebugLine(GetWorld(), PresentationGrabPoint, DesiredLocation, FColor::Silver, false, 0.05f, 0, 2.0f);
	}
}

void UPPGrabberComponent::UpdateLedgeGrab(float /*DeltaTime*/)
{
	ACharacter* OwnerCharacter = Cast<ACharacter>(GetOwner());
	UCharacterMovementComponent* OwnerMovement = OwnerCharacter ? OwnerCharacter->GetCharacterMovement() : nullptr;
	if (!OwnerCharacter || !OwnerMovement || !GrabTarget || !IsStandingPlayerStateValid(OwnerCharacter))
	{
		ReleaseGrabAuthoritative();
		return;
	}

	FPPGrabTargetData CurrentTargetData;
	CurrentTargetData.TargetActor = GrabTarget;
	CurrentTargetData.TargetPrimitive = GrabbedPrimitive;
	CurrentTargetData.GrabPoint = FixedWorldGrabPoint;
	if (!HasClearLineOfSight(GetGrabOrigin(), CurrentTargetData))
	{
		ReleaseGrabAuthoritative();
		return;
	}

	const FVector DesiredActorLocation = FixedWorldGrabPoint - FVector(0.0f, 0.0f, 70.0f);
	const FVector ConstraintError = DesiredActorLocation - OwnerCharacter->GetActorLocation();
	ConstraintForceEstimate = ConstraintError.Size() * ActiveProfile.LinearStiffness;
	if (ConstraintError.Size() > ActiveProfile.MaximumRange * 1.5f || ConstraintForceEstimate > ActiveProfile.BreakForce)
	{
		ReleaseGrabAuthoritative();
		return;
	}

	const FVector SwingFriendlyError(ConstraintError.X * 0.55f, ConstraintError.Y * 0.55f, ConstraintError.Z);
	const FVector Acceleration = SwingFriendlyError.GetClampedToMaxSize(180.0f) * LedgeConstraintAcceleration / 180.0f;
	OwnerMovement->AddForce(Acceleration * OwnerMovement->Mass);
	PresentationGrabPoint = FixedWorldGrabPoint;
	if (bDrawGrabDebug && GetWorld())
	{
		DrawDebugLine(GetWorld(), GetGrabOrigin(), FixedWorldGrabPoint, FColor::Emerald, false, 0.05f, 0, 2.0f);
	}
}

void UPPGrabberComponent::BeginIncomingPlayerGrab(AActor* NewIncomingGrabber)
{
	if (!GetOwner() || !GetOwner()->HasAuthority() || !NewIncomingGrabber)
	{
		return;
	}

	if (GrabTarget && GrabTarget != NewIncomingGrabber && GrabState != EPPGrabState::None)
	{
		ReleaseGrabAuthoritative(false);
	}
	else if (GrabState == EPPGrabState::HoldingItem)
	{
		ReleaseGrabAuthoritative(false);
	}
	IncomingGrabber = NewIncomingGrabber;
	if (ACharacter* OwnerCharacter = Cast<ACharacter>(GetOwner()))
	{
		if (UCharacterMovementComponent* OwnerMovement = OwnerCharacter->GetCharacterMovement();
			OwnerMovement && OwnerMovement->IsFalling())
		{
			FVector ReducedVelocity = OwnerMovement->Velocity;
			ReducedVelocity.X *= AirborneHorizontalMomentumMultiplier;
			ReducedVelocity.Y *= AirborneHorizontalMomentumMultiplier;
			OwnerMovement->Velocity = ReducedVelocity;
		}
	}
	OnRep_GrabPresentation();
	SetComponentTickEnabled(true);
}

void UPPGrabberComponent::EndIncomingPlayerGrab(AActor* PreviousIncomingGrabber, bool bApplyImmunity)
{
	if (!GetOwner() || !GetOwner()->HasAuthority() || IncomingGrabber != PreviousIncomingGrabber)
	{
		return;
	}
	IncomingGrabber = nullptr;
	if (bApplyImmunity && PreviousIncomingGrabber && GetWorld())
	{
		ImmuneGrabber = PreviousIncomingGrabber;
		ImmunityEndTimeSeconds = GetWorld()->GetTimeSeconds() + SameOpponentImmunitySeconds;
	}
	OnRep_GrabPresentation();
	if (GrabState == EPPGrabState::None)
	{
		SetComponentTickEnabled(false);
	}
}

void UPPGrabberComponent::ApplyIncomingGrabRagdoll(bool bEnableRagdoll)
{
	if (bIncomingGrabRagdollApplied == bEnableRagdoll)
	{
		return;
	}

	ACharacter* OwnerCharacter = Cast<ACharacter>(GetOwner());
	USkeletalMeshComponent* OwnerMesh = OwnerCharacter ? OwnerCharacter->GetMesh() : nullptr;
	if (!OwnerCharacter || !OwnerMesh)
	{
		return;
	}

	bIncomingGrabRagdollApplied = bEnableRagdoll;
	if (bEnableRagdoll)
	{
		IncomingGrabInitialMeshRelativeTransform = OwnerMesh->GetRelativeTransform();
		if (UCharacterMovementComponent* OwnerMovement = OwnerCharacter->GetCharacterMovement())
		{
			OwnerMovement->StopMovementImmediately();
			OwnerMovement->DisableMovement();
		}
		if (UCapsuleComponent* OwnerCapsule = OwnerCharacter->GetCapsuleComponent())
		{
			OwnerCapsule->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		}
		OwnerMesh->SetCollisionProfileName(TEXT("Ragdoll"));
		OwnerMesh->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
		OwnerMesh->SetCollisionObjectType(ECC_PhysicsBody);
		OwnerMesh->SetCollisionResponseToAllChannels(ECR_Block);
		OwnerMesh->SetCollisionResponseToChannel(ECC_Pawn, ECR_Ignore);
		OwnerMesh->SetCollisionResponseToChannel(ECC_Camera, ECR_Ignore);
		OwnerMesh->SetAllUseCCD(true);
		OwnerMesh->SetEnableGravity(true);
		OwnerMesh->bPauseAnims = true;
		// Simulate only the authored character body chain. The Penguin has an
		// inversely-scaled Null/petArmat import chain above hips; including those
		// import roots explosively launches the mesh out of world bounds.
		const FName RagdollRootBone = ResolvePlayerRecoveryBone(OwnerMesh);
		if (!RagdollRootBone.IsNone())
		{
			OwnerMesh->SetAllBodiesBelowSimulatePhysics(RagdollRootBone, true, true);
			OwnerMesh->SetAllBodiesBelowPhysicsBlendWeight(RagdollRootBone, 1.0f, false, true);
		}
		OwnerMesh->SetAllPhysicsLinearVelocity(FVector::ZeroVector, false);
		OwnerMesh->SetAllPhysicsAngularVelocityInDegrees(FVector::ZeroVector, false);
		OwnerMesh->WakeAllRigidBodies();
		UE_LOG(LogVNH, Display, TEXT("PairPressureGrab: incoming grab ragdoll enabled. Target=%s PhysicsAsset=%s"),
			*GetNameSafe(OwnerCharacter),
			*GetNameSafe(OwnerMesh->GetPhysicsAsset()));
		return;
	}

	if (const UPPPhysicalStateComponent* PhysicalState = UPPPhysicalStateComponent::FindPhysicalStateComponent(OwnerCharacter);
		PhysicalState && PhysicalState->IsRagdolled())
	{
		return;
	}

	FVector RecoveryLocation = OwnerMesh->GetComponentLocation();
	const FName RecoveryBone = ResolvePlayerRecoveryBone(OwnerMesh);
	if (!RecoveryBone.IsNone())
	{
		RecoveryLocation = OwnerMesh->GetBoneLocation(RecoveryBone);
	}
	if (const UCapsuleComponent* OwnerCapsule = OwnerCharacter->GetCapsuleComponent())
	{
		RecoveryLocation.Z += OwnerCapsule->GetScaledCapsuleHalfHeight();
	}
	if (!RecoveryBone.IsNone())
	{
		OwnerMesh->SetAllBodiesBelowPhysicsBlendWeight(RecoveryBone, 0.0f, false, true);
		OwnerMesh->SetAllBodiesBelowSimulatePhysics(RecoveryBone, false, true);
	}
	OwnerMesh->SetAllBodiesSimulatePhysics(false);
	OwnerMesh->SetAllUseCCD(false);
	OwnerMesh->bPauseAnims = false;
	OwnerMesh->SetCollisionProfileName(TEXT("CharacterMesh"));
	OwnerMesh->SetRelativeTransform(IncomingGrabInitialMeshRelativeTransform);
	OwnerCharacter->SetActorLocation(RecoveryLocation, false, nullptr, ETeleportType::TeleportPhysics);
	if (UCapsuleComponent* OwnerCapsule = OwnerCharacter->GetCapsuleComponent())
	{
		OwnerCapsule->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
	}
	if (UCharacterMovementComponent* OwnerMovement = OwnerCharacter->GetCharacterMovement())
	{
		OwnerMovement->SetMovementMode(MOVE_Walking);
	}
	UE_LOG(LogVNH, Display, TEXT("PairPressureGrab: incoming grab ragdoll restored. Target=%s"), *GetNameSafe(OwnerCharacter));
}

void UPPGrabberComponent::PlayGetUpFrontAnimation(ACharacter* RecoveringCharacter)
{
	if (!RecoveringCharacter || !GetWorld())
	{
		return;
	}

	USkeletalMeshComponent* RecoveringMesh = RecoveringCharacter->GetMesh();
	if (!RecoveringMesh || RecoveringMesh->IsAnySimulatingPhysics())
	{
		return;
	}

	UAnimSequence* GetUpFrontAnimation = nullptr;
	if (UDataTable* MascotAnimationTable = LoadObject<UDataTable>(
		nullptr,
		TEXT("/Game/PairPressure/Data/DT_MascotAnimations.DT_MascotAnimations")))
	{
		if (const FPPMascotAnimationRow* PenguinRow = MascotAnimationTable->FindRow<FPPMascotAnimationRow>(
			FName(TEXT("Penguin")),
			TEXT("Grab release get-up"),
			false))
		{
			GetUpFrontAnimation = PenguinRow->GetUpFront.LoadSynchronous();
		}
	}
	if (!GetUpFrontAnimation)
	{
		GetUpFrontAnimation = LoadObject<UAnimSequence>(
			nullptr,
			TEXT("/Game/CuteChubbyPenguin/Penguin/Animations/AS_Penguin_UE_Anim_wakesup1.AS_Penguin_UE_Anim_wakesup1"));
	}
	if (!GetUpFrontAnimation)
	{
		return;
	}

	UClass* LocomotionAnimClass = RecoveringMesh->GetAnimClass();
	RecoveringMesh->SetAnimationMode(EAnimationMode::AnimationSingleNode);
	RecoveringMesh->SetAnimation(GetUpFrontAnimation);
	RecoveringMesh->Play(false);
	if (UCharacterMovementComponent* RecoveringMovement = RecoveringCharacter->GetCharacterMovement())
	{
		RecoveringMovement->DisableMovement();
	}

	const float RecoveryDuration = FMath::Max(0.1f, GetUpFrontAnimation->GetPlayLength());
	TWeakObjectPtr<ACharacter> WeakRecoveringCharacter(RecoveringCharacter);
	TWeakObjectPtr<USkeletalMeshComponent> WeakRecoveringMesh(RecoveringMesh);
	FTimerHandle GetUpTimerHandle;
	GetWorld()->GetTimerManager().SetTimer(
		GetUpTimerHandle,
		[WeakRecoveringCharacter, WeakRecoveringMesh, LocomotionAnimClass]()
		{
			ACharacter* Character = WeakRecoveringCharacter.Get();
			USkeletalMeshComponent* Mesh = WeakRecoveringMesh.Get();
			if (!Character || !Mesh || Mesh->IsAnySimulatingPhysics())
			{
				return;
			}
			if (const UPPGrabberComponent* CharacterGrabber = Character->FindComponentByClass<UPPGrabberComponent>();
				CharacterGrabber && CharacterGrabber->GetIncomingGrabber())
			{
				return;
			}
			Mesh->SetAnimationMode(EAnimationMode::AnimationBlueprint);
			if (LocomotionAnimClass)
			{
				Mesh->SetAnimInstanceClass(LocomotionAnimClass);
			}
			if (UCharacterMovementComponent* Movement = Character->GetCharacterMovement())
			{
				Movement->SetMovementMode(MOVE_Walking);
			}
		},
		RecoveryDuration,
		false);
}

FName UPPGrabberComponent::ResolvePlayerGrabBone(const USkeletalMeshComponent* TargetMesh) const
{
	if (!TargetMesh)
	{
		return NAME_None;
	}

	for (const FName CandidateBone : {
		FName(TEXT("chest")), FName(TEXT("spine_03")), FName(TEXT("spine_02")),
		FName(TEXT("hips")), FName(TEXT("pelvis"))})
	{
		if (TargetMesh->GetBoneIndex(CandidateBone) != INDEX_NONE)
		{
			return CandidateBone;
		}
	}
	return NAME_None;
}

FName UPPGrabberComponent::ResolvePlayerRecoveryBone(const USkeletalMeshComponent* TargetMesh) const
{
	if (!TargetMesh)
	{
		return NAME_None;
	}

	const UPhysicsAsset* PhysicsAsset = TargetMesh->GetPhysicsAsset();
	for (const FName CandidateBone : {
		FName(TEXT("hips")), FName(TEXT("pelvis")), FName(TEXT("chest")),
		FName(TEXT("spine_03")), FName(TEXT("root")), FName(TEXT("Root"))})
	{
		if (TargetMesh->GetBoneIndex(CandidateBone) != INDEX_NONE
			&& (!PhysicsAsset || PhysicsAsset->FindBodyIndex(CandidateBone) != INDEX_NONE))
		{
			return CandidateBone;
		}
	}
	return NAME_None;
}

void UPPGrabberComponent::ApplyMovementPresentation()
{
	AActor* OwnerActor = GetOwner();
	UVNHAlienLocomotionComponent* LocomotionComponent = OwnerActor
		? OwnerActor->FindComponentByClass<UVNHAlienLocomotionComponent>()
		: nullptr;
	if (!LocomotionComponent)
	{
		return;
	}

	float MovementMultiplier = 1.0f;
	float TurnMultiplier = 1.0f;
	if (GrabState == EPPGrabState::MutualGrab)
	{
		MovementMultiplier = MutualMovementMultiplier;
		TurnMultiplier = GrabberTurnMultiplier;
	}
	else if (IncomingGrabber)
	{
		MovementMultiplier = VictimMovementMultiplier;
		TurnMultiplier = 1.0f;
	}
	else if (GrabState == EPPGrabState::GrabbingPlayer)
	{
		MovementMultiplier = GrabberMovementMultiplier;
		TurnMultiplier = GrabberTurnMultiplier;
	}
	else if (GrabState == EPPGrabState::HoldingItem || GrabState == EPPGrabState::PushingObject)
	{
		MovementMultiplier = ActiveProfile.MovementSpeedMultiplier;
	}
	else if (GrabState == EPPGrabState::HangingFromLedge)
	{
		MovementMultiplier = 0.35f;
		TurnMultiplier = 0.4f;
	}
	else if (GrabState == EPPGrabState::Reaching)
	{
		MovementMultiplier = 0.94f;
	}
	LocomotionComponent->SetGrabMovementMultiplier(MovementMultiplier);
	LocomotionComponent->SetGrabTurnMultiplier(TurnMultiplier);
}

void UPPGrabberComponent::ForceDropTargetItem(AActor* TargetActor) const
{
	AVNHShopperCharacter* TargetShopper = Cast<AVNHShopperCharacter>(TargetActor);
	if (!TargetShopper || !TargetShopper->HasAuthority())
	{
		return;
	}

	int32 SafetyCount = 0;
	while (AActor* HeldProp = TargetShopper->GetHeldProp())
	{
		HeldProp->DetachFromActor(FDetachmentTransformRules::KeepWorldTransform);
		HeldProp->SetOwner(nullptr);
		TInlineComponentArray<UPrimitiveComponent*> PrimitiveComponents;
		HeldProp->GetComponents(PrimitiveComponents);
		for (UPrimitiveComponent* PrimitiveComponent : PrimitiveComponents)
		{
			if (PrimitiveComponent)
			{
				PrimitiveComponent->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
				PrimitiveComponent->SetSimulatePhysics(true);
			}
		}
		TargetShopper->SetHeldProp(nullptr);
		if (++SafetyCount >= 8)
		{
			break;
		}
	}
}

bool UPPGrabberComponent::IsFriendlyPlayer(const AActor* OtherActor) const
{
	const UPPTeamMemberComponent* OwnerTeam = UPPTeamMemberComponent::FindTeamComponent(GetOwner());
	return OwnerTeam && OwnerTeam->IsFriendlyTo_Implementation(OtherActor);
}

bool UPPGrabberComponent::IsStandingPlayerStateValid(const AActor* PlayerActor) const
{
	const UPPPhysicalStateComponent* PhysicalState = UPPPhysicalStateComponent::FindPhysicalStateComponent(PlayerActor);
	return PlayerActor && (!PhysicalState || (!PhysicalState->IsRagdolled() && !PhysicalState->IsUnconscious()));
}

void UPPGrabberComponent::ApplyBreakFreePenalty(AActor* EscapedPlayer)
{
	if (UPPGrabberComponent* EscapedGrabber = EscapedPlayer
		? EscapedPlayer->FindComponentByClass<UPPGrabberComponent>()
		: nullptr)
	{
		EscapedGrabber->ImmuneGrabber = GetOwner();
		EscapedGrabber->ImmunityEndTimeSeconds = GetWorld()
			? GetWorld()->GetTimeSeconds() + SameOpponentImmunitySeconds
			: 0.0;
	}
	if (ACharacter* OwnerCharacter = Cast<ACharacter>(GetOwner()))
	{
		const FVector RecoilVelocity = -OwnerCharacter->GetActorForwardVector() * 260.0f + FVector(0.0f, 0.0f, 95.0f);
		OwnerCharacter->LaunchCharacter(RecoilVelocity, true, false);
	}
}

int32 UPPGrabberComponent::GetDefaultPriority(EPPGrabTargetType TargetType) const
{
	switch (TargetType)
	{
	case EPPGrabTargetType::GameplayItem:
		return 4;
	case EPPGrabTargetType::Player:
		return 3;
	case EPPGrabTargetType::LedgeOrHandle:
		return 2;
	case EPPGrabTargetType::LargePushable:
		return 1;
	case EPPGrabTargetType::None:
	default:
		return 0;
	}
}

FVector UPPGrabberComponent::GetGrabOrigin() const
{
	const AActor* OwnerActor = GetOwner();
	return OwnerActor
		? OwnerActor->GetActorTransform().TransformPositionNoScale(GrabOriginOffset)
		: FVector::ZeroVector;
}

FVector UPPGrabberComponent::GetHandCarryLocation(float ForwardDistance) const
{
	const ACharacter* OwnerCharacter = Cast<ACharacter>(GetOwner());
	const USkeletalMeshComponent* OwnerMesh = OwnerCharacter ? OwnerCharacter->GetMesh() : nullptr;
	FVector CarryLocation = GetGrabOrigin();
	bool bFoundBothHands = false;
	if (OwnerMesh)
	{
		FName LeftHandBone = NAME_None;
		FName RightHandBone = NAME_None;
		for (const FName Candidate : {FName(TEXT("hand_L")), FName(TEXT("hand_l")), FName(TEXT("Hand_L"))})
		{
			if (OwnerMesh->GetBoneIndex(Candidate) != INDEX_NONE)
			{
				LeftHandBone = Candidate;
				break;
			}
		}
		for (const FName Candidate : {FName(TEXT("hand_R")), FName(TEXT("hand_r")), FName(TEXT("Hand_R"))})
		{
			if (OwnerMesh->GetBoneIndex(Candidate) != INDEX_NONE)
			{
				RightHandBone = Candidate;
				break;
			}
		}
		if (!LeftHandBone.IsNone() && !RightHandBone.IsNone())
		{
			CarryLocation = (OwnerMesh->GetBoneLocation(LeftHandBone) + OwnerMesh->GetBoneLocation(RightHandBone)) * 0.5f;
			bFoundBothHands = true;
		}
	}

	const FVector Forward = GetOwner() ? GetOwner()->GetActorForwardVector().GetSafeNormal2D() : FVector::ForwardVector;
	const float HandForwardOffset = bFoundBothHands
		? FMath::Clamp(ForwardDistance * 0.32f, 25.0f, 45.0f)
		: FMath::Max(0.0f, ForwardDistance);
	// Imported character skeletons can put resting hand bones unusually low. Keep
	// the physical carry point at least at the authored chest-height grab origin.
	constexpr float HandCarryVerticalOffset = -45.0f;
	CarryLocation.Z = FMath::Max(
		CarryLocation.Z + HandCarryVerticalOffset,
		GetGrabOrigin().Z + HandCarryVerticalOffset);
	return CarryLocation + Forward * HandForwardOffset;
}

void UPPGrabberComponent::SetGrabPresentation(EPPGrabState NewGrabState, AActor* NewGrabTarget)
{
	const bool bPresentationChanged = GrabState != NewGrabState || GrabTarget != NewGrabTarget;
	GrabState = NewGrabState;
	GrabTarget = NewGrabTarget;
	if (bPresentationChanged)
	{
		OnRep_GrabPresentation();
	}
}
