#include "PairPressure/PPGrabberComponent.h"

#include "Components/CapsuleComponent.h"
#include "Components/PrimitiveComponent.h"
#include "Components/SceneComponent.h"
#include "DrawDebugHelpers.h"
#include "Engine/EngineTypes.h"
#include "Engine/OverlapResult.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "GameFramework/Character.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Animation/AnimInstance.h"
#include "Animation/AnimSequence.h"
#include "Engine/DataTable.h"
#include "Net/UnrealNetwork.h"
#include "PhysicsEngine/PhysicsAsset.h"
#include "PhysicsEngine/BodyInstance.h"
#include "PhysicsEngine/PhysicsHandleComponent.h"
#include "PairPressure/PPGrabbableComponent.h"
#include "PairPressure/PPPhysicalStateComponent.h"
#include "PairPressure/PPPlayerActionRouterComponent.h"
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

FVector GetPPBridgePerimeterPoint(const FBox& BridgeBounds, const FVector& QueryLocation)
{
	FVector PerimeterPoint = BridgeBounds.GetClosestPointTo(QueryLocation);
	const bool bInsideHorizontalBounds = QueryLocation.X >= BridgeBounds.Min.X && QueryLocation.X <= BridgeBounds.Max.X
		&& QueryLocation.Y >= BridgeBounds.Min.Y && QueryLocation.Y <= BridgeBounds.Max.Y;
	if (bInsideHorizontalBounds)
	{
		const float DistanceToMinX = QueryLocation.X - BridgeBounds.Min.X;
		const float DistanceToMaxX = BridgeBounds.Max.X - QueryLocation.X;
		const float DistanceToMinY = QueryLocation.Y - BridgeBounds.Min.Y;
		const float DistanceToMaxY = BridgeBounds.Max.Y - QueryLocation.Y;
		const float NearestSideDistance = FMath::Min(
			FMath::Min(DistanceToMinX, DistanceToMaxX),
			FMath::Min(DistanceToMinY, DistanceToMaxY));
		if (NearestSideDistance == DistanceToMinX)
		{
			PerimeterPoint.X = BridgeBounds.Min.X;
		}
		else if (NearestSideDistance == DistanceToMaxX)
		{
			PerimeterPoint.X = BridgeBounds.Max.X;
		}
		else if (NearestSideDistance == DistanceToMinY)
		{
			PerimeterPoint.Y = BridgeBounds.Min.Y;
		}
		else
		{
			PerimeterPoint.Y = BridgeBounds.Max.Y;
		}
	}
	PerimeterPoint.Z = BridgeBounds.Max.Z - 8.0f;
	return PerimeterPoint;
}

bool IsPPBridgeLedge(const AActor* CandidateActor)
{
	return CandidateActor && CandidateActor->GetName().Contains(TEXT("Bridge_V1"), ESearchCase::IgnoreCase);
}

bool UsesPPGrabberStablePenguinRagdoll(const USkeletalMeshComponent* CharacterMesh)
{
	const UPhysicsAsset* PhysicsAsset = CharacterMesh ? CharacterMesh->GetPhysicsAsset() : nullptr;
	return PhysicsAsset
		&& PhysicsAsset->SkeletalBodySetups.Num() == 2
		&& PhysicsAsset->FindBodyIndex(FName(TEXT("hips"))) != INDEX_NONE
		&& PhysicsAsset->FindBodyIndex(FName(TEXT("chest"))) != INDEX_NONE;
}
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
	// Blueprint-authored Lobby fixtures can otherwise retain overlap-only defaults.
	// Normalize every tagged pushable as solid world geometry before the player
	// reaches it, not only after a grab has already started.
	if (UWorld* World = GetWorld())
	{
		for (TActorIterator<AActor> ActorIterator(World); ActorIterator; ++ActorIterator)
		{
			AActor* CandidatePushable = *ActorIterator;
			if (!CandidatePushable || !CandidatePushable->ActorHasTag(TEXT("PP.Grab.Pushable")))
			{
				continue;
			}
			TInlineComponentArray<UPrimitiveComponent*> PrimitiveComponents;
			CandidatePushable->GetComponents(PrimitiveComponents);
			for (UPrimitiveComponent* PrimitiveComponent : PrimitiveComponents)
			{
				if (!PrimitiveComponent || !PrimitiveComponent->IsSimulatingPhysics())
				{
					continue;
				}
				PrimitiveComponent->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
				PrimitiveComponent->SetCollisionResponseToChannel(ECC_Pawn, ECR_Block);
				PrimitiveComponent->SetCollisionResponseToChannel(ECC_WorldStatic, ECR_Block);
				PrimitiveComponent->SetCollisionResponseToChannel(ECC_WorldDynamic, ECR_Block);
			}
		}
	}
}

void UPPGrabberComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

	if (!GetOwner())
	{
		return;
	}
	if (IncomingGrabber)
	{
		UpdateIncomingCarryPresentation();
	}
	if (!GetOwner()->HasAuthority())
	{
		UpdateRemoteGrabPresentation(DeltaTime);
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
	if (const UPPPlayerActionRouterComponent* ActionRouter = OwnerActor->FindComponentByClass<UPPPlayerActionRouterComponent>();
		ActionRouter && ActionRouter->IsAirDiveActive())
	{
		// Dives can hand off directly to an authored ledge, but must not turn into
		// a general mid-air pickup or player-grab action.
		const FPPGrabTargetData DiveTarget = FindBestTarget(SafeCameraForward);
		if (!DiveTarget.IsValid() || DiveTarget.Profile.TargetType != EPPGrabTargetType::LedgeOrHandle)
		{
			return;
		}
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
		if (GrabState == EPPGrabState::HoldingItem
			|| GrabState == EPPGrabState::GrabbingPlayer
			|| GrabState == EPPGrabState::MutualGrab)
		{
			bPredictedReleasePresentationPending = true;
			OnGrabReleasedPresentation.Broadcast(GrabState == EPPGrabState::HoldingItem, false);
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
		bPredictedThrowPresentationPending = true;
		OnGrabThrowPresentation.Broadcast(false);
		ServerThrowHeldItem(SafeThrowDirection);
		return;
	}
	PerformHeldItemThrow(SafeThrowDirection, 0.0f);
}

void UPPGrabberComponent::RequestChargedThrow(const FVector& ThrowDirection, float ChargeAlpha)
{
	AActor* OwnerActor = GetOwner();
	if (!OwnerActor)
	{
		return;
	}

	const FVector SafeThrowDirection = ThrowDirection.GetSafeNormal();
	if (SafeThrowDirection.IsNearlyZero())
	{
		return;
	}

	const float ClampedChargeAlpha = FMath::Clamp(ChargeAlpha, 0.0f, 1.0f);
	if (!OwnerActor->HasAuthority())
	{
		const bool bConfirmedLocalHold = GrabState == EPPGrabState::HoldingItem
			|| GrabState == EPPGrabState::GrabbingPlayer;
		bPredictedThrowPresentationPending = bConfirmedLocalHold;
		if (bConfirmedLocalHold)
		{
			OnGrabThrowPresentation.Broadcast(ClampedChargeAlpha >= 0.45f);
		}
		ServerThrowHeldItemCharged(SafeThrowDirection, ClampedChargeAlpha);
		return;
	}
	PerformHeldItemThrowWithReachGrace(SafeThrowDirection, ClampedChargeAlpha);
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

void UPPGrabberComponent::RequestLedgeClimb()
{
	AActor* OwnerActor = GetOwner();
	if (!OwnerActor || GrabState != EPPGrabState::HangingFromLedge)
	{
		return;
	}

	if (!OwnerActor->HasAuthority())
	{
		ServerRequestLedgeClimb();
		return;
	}
	PerformLedgeClimb();
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
	PerformHeldItemThrowWithReachGrace(ThrowDirection, 0.0f);
}

void UPPGrabberComponent::ServerThrowHeldItemCharged_Implementation(FVector_NetQuantizeNormal ThrowDirection, float ChargeAlpha)
{
	PerformHeldItemThrowWithReachGrace(ThrowDirection, FMath::Clamp(ChargeAlpha, 0.0f, 1.0f));
}

void UPPGrabberComponent::ServerRequestDirectionalEscape_Implementation(FVector_NetQuantizeNormal EscapeDirection)
{
	PerformDirectionalEscape(EscapeDirection);
}

void UPPGrabberComponent::ServerRequestLedgeClimb_Implementation()
{
	PerformLedgeClimb();
}

void UPPGrabberComponent::MulticastGrabRejected_Implementation()
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

void UPPGrabberComponent::MulticastGrabReleasedPresentation_Implementation(bool bDroppedItem, bool bLedgeClimb)
{
	if (GetOwner() && GetOwner()->GetLocalRole() == ROLE_AutonomousProxy && bPredictedReleasePresentationPending)
	{
		bPredictedReleasePresentationPending = false;
		return;
	}
	OnGrabReleasedPresentation.Broadcast(bDroppedItem, bLedgeClimb);
}

void UPPGrabberComponent::MulticastGrabThrowPresentation_Implementation(bool bChargedThrow)
{
	if (GetOwner() && GetOwner()->GetLocalRole() == ROLE_AutonomousProxy && bPredictedThrowPresentationPending)
	{
		bPredictedThrowPresentationPending = false;
		return;
	}
	OnGrabThrowPresentation.Broadcast(bChargedThrow);
}

void UPPGrabberComponent::MulticastPlayGrabbedPlayerGetUp_Implementation(ACharacter* RecoveringCharacter)
{
	PlayGetUpFrontAnimation(RecoveringCharacter);
}

void UPPGrabberComponent::OnRep_GrabPresentation()
{
	if (IncomingGrabber)
	{
		// The carrier and target replicate on separate actor channels. Validate the
		// carrier's outgoing state before enabling a hold so a stale target pointer
		// cannot re-freeze a throw that has already begun.
		UpdateIncomingCarryPresentation();
	}
	else
	{
		ApplyIncomingGrabRagdoll(false);
	}
	ApplyMovementPresentation();
	OnGrabStateChanged.Broadcast(GrabState, GrabTarget);
	const bool bNeedsGrabTick = IncomingGrabber != nullptr
		|| (GetOwner() && GetOwner()->HasAuthority() && GrabState != EPPGrabState::None);
	SetComponentTickEnabled(bNeedsGrabTick);
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
	ObjectQueryParams.AddObjectTypesToQuery(ECC_WorldStatic);
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

	// Push/pull blocks are intentionally context-free: a player can take hold of
	// any reachable face, not just the face currently in the camera sweep.
	const float PushableSearchRadius = FMath::Max(SearchRadius * 3.0f, 225.0f);
	TArray<FOverlapResult> PushableOverlaps;
	World->OverlapMultiByObjectType(
		PushableOverlaps,
		ClosePickupCenter,
		FQuat::Identity,
		ObjectQueryParams,
		FCollisionShape::MakeSphere(PushableSearchRadius),
		QueryParams);
	for (const FOverlapResult& PushableOverlap : PushableOverlaps)
	{
		AActor* PushableActor = PushableOverlap.GetActor();
		if (PushableActor && PushableActor->ActorHasTag(TEXT("PP.Grab.Pushable")))
		{
			CandidateActors.AddUnique(PushableActor);
		}
	}

	// Ledge grips are intentionally forgiving: their authored grip point may sit
	// above the player's chest sweep, so collect nearby tagged/contract targets
	// before the target-specific range and facing checks below decide eligibility.
	TArray<FOverlapResult> LedgeOverlaps;
	World->OverlapMultiByObjectType(
		LedgeOverlaps,
		TraceStart + CameraForward * (SearchReach * 0.5f),
		FQuat::Identity,
		ObjectQueryParams,
		FCollisionShape::MakeSphere(FMath::Max(SearchRadius * 2.5f, 175.0f)),
		QueryParams);
	for (const FOverlapResult& LedgeOverlap : LedgeOverlaps)
	{
		AActor* LedgeActor = LedgeOverlap.GetActor();
		if (LedgeActor && (LedgeActor->ActorHasTag(TEXT("PP.Grab.Ledge")) || LedgeActor->ActorHasTag(TEXT("PP.Grab.Handle")) || IsPPBridgeLedge(LedgeActor)))
		{
			CandidateActors.AddUnique(LedgeActor);
		}
	}
	// Bridge V1 is used by both Lobby bridge instances (including the actor
	// labelled Bridge_V2). Its broad platform bounds can leave a side edge out
	// of the chest sphere, so add it by nearest perimeter distance instead.
	for (TActorIterator<AActor> ActorIterator(World); ActorIterator; ++ActorIterator)
	{
		AActor* BridgeActor = *ActorIterator;
		if (!IsPPBridgeLedge(BridgeActor))
		{
			continue;
		}
		FVector BridgeBoundsOrigin;
		FVector BridgeBoundsExtent;
		BridgeActor->GetActorBounds(true, BridgeBoundsOrigin, BridgeBoundsExtent);
		const FVector NearestBridgePoint = GetPPBridgePerimeterPoint(
			FBox::BuildAABB(BridgeBoundsOrigin, BridgeBoundsExtent),
			TraceStart);
		if (FVector::DistSquared(TraceStart, NearestBridgePoint) <= FMath::Square(340.0f))
		{
			CandidateActors.AddUnique(BridgeActor);
		}
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
	else if (CandidateActor->ActorHasTag(TEXT("PP.Grab.Ledge")) || CandidateActor->ActorHasTag(TEXT("PP.Grab.Handle")) || IsPPBridgeLedge(CandidateActor))
	{
		ResultProfile.TargetType = EPPGrabTargetType::LedgeOrHandle;
		if (!bHasAuthoredProfile)
		{
			ResultProfile.MaximumRange = IsPPBridgeLedge(CandidateActor) ? 340.0f : 190.0f;
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
	const bool bBridgeLedge = IsPPBridgeLedge(CandidateActor);
	if (!OwnerActor || (!GrabbableComponent && !bUsesBlueprintContract && !bBridgeLedge)
		|| (GrabbableComponent && !IPPGrabbable::Execute_CanBeGrabbed(GrabbableComponent, OwnerActor))
		|| (!GrabbableComponent && bUsesBlueprintContract && !IsBlueprintGrabEnabled(CandidateActor)))
	{
		return false;
	}

	const FPPGrabProfile CandidateProfile = GrabbableComponent
		? IPPGrabbable::Execute_GetGrabProfile(GrabbableComponent)
		: GetBlueprintGrabProfile(CandidateActor);
	const bool bLedgeTarget = CandidateProfile.TargetType == EPPGrabTargetType::LedgeOrHandle;
	const bool bPushableTarget = CandidateProfile.TargetType == EPPGrabTargetType::LargePushable;
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
	if (bBridgeLedge)
	{
		TInlineComponentArray<UPrimitiveComponent*> CandidatePrimitives;
		CandidateActor->GetComponents(CandidatePrimitives);
		float NearestBridgeEdgeDistanceSquared = TNumericLimits<float>::Max();
		for (UPrimitiveComponent* PrimitiveComponent : CandidatePrimitives)
		{
			if (!PrimitiveComponent || PrimitiveComponent->GetCollisionEnabled() == ECollisionEnabled::NoCollision)
			{
				continue;
			}
			const FVector NearestEdgePoint = GetPPBridgePerimeterPoint(PrimitiveComponent->Bounds.GetBox(), TraceStart);
			const float EdgeDistanceSquared = FVector::DistSquared(TraceStart, NearestEdgePoint);
			if (EdgeDistanceSquared < NearestBridgeEdgeDistanceSquared)
			{
				NearestBridgeEdgeDistanceSquared = EdgeDistanceSquared;
				CandidatePrimitive = PrimitiveComponent;
			}
		}
	}
	else if (!CandidatePrimitive)
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
	if (bBridgeLedge)
	{
		const FBox BridgeBounds = CandidatePrimitive->Bounds.GetBox();
		CandidateGrabPoint = GetPPBridgePerimeterPoint(BridgeBounds, TraceStart);
	}
	const FVector ToCandidate = CandidateGrabPoint - TraceStart;
	const float CandidateDistance = ToCandidate.Size();
	const float EffectiveMaximumRange = bBridgeLedge
		? FMath::Max(CandidateProfile.MaximumRange, 340.0f)
		: bLedgeTarget
		? FMath::Min(SearchReach, FMath::Max(CandidateProfile.MaximumRange, 250.0f))
		: FMath::Min(SearchReach, CandidateProfile.MaximumRange);
	if (CandidateDistance > EffectiveMaximumRange || CandidateDistance <= UE_KINDA_SMALL_NUMBER)
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
	const bool bClosePlayer = CandidateProfile.TargetType == EPPGrabTargetType::Player
		&& HorizontalDistance <= 155.0f
		&& FMath::Abs(ToCandidate.Z) <= 145.0f;
	const float HorizontalFacingAlignment = HorizontalDistance <= UE_KINDA_SMALL_NUMBER
		? 1.0f
		: FVector::DotProduct(OwnerActor->GetActorForwardVector().GetSafeNormal2D(), HorizontalOffset / HorizontalDistance);
	const bool bLedgeFacingValid = bLedgeTarget
		&& HorizontalFacingAlignment >= (bBridgeLedge ? -0.55f : -0.35f)
		&& CameraAlignment >= (bBridgeLedge ? -0.4f : -0.2f);
	if ((!bLedgeTarget && !bPushableTarget && !bCloseGameplayItem && !bClosePlayer
			&& (FacingAlignment <= 0.0f
				|| CameraAlignment < FMath::Cos(FMath::DegreesToRadians(CandidateProfile.MaximumAngleDegrees))))
		|| (bCloseGameplayItem && HorizontalFacingAlignment < -0.4f)
		|| (bClosePlayer && HorizontalFacingAlignment < -0.05f)
		|| (bLedgeTarget && !bLedgeFacingValid))
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
		+ (bCloseGameplayItem ? 300.0f : 0.0f)
		+ (bClosePlayer ? 220.0f : 0.0f);
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
		MulticastGrabRejected();
		return;
	}

	ActiveProfile = TargetData.Profile;
	if (ActiveProfile.TargetType == EPPGrabTargetType::Player)
	{
		ActiveProfile.MaximumRange = FMath::Max(ActiveProfile.MaximumRange, 260.0f);
		ActiveProfile.MaximumAngleDegrees = FMath::Max(ActiveProfile.MaximumAngleDegrees, 65.0f);
		ActiveProfile.LinearStiffness = FMath::Max(ActiveProfile.LinearStiffness, 5200.0f);
		ActiveProfile.LinearDamping = FMath::Max(ActiveProfile.LinearDamping, 900.0f);
		ActiveProfile.BreakForce = FMath::Max(ActiveProfile.BreakForce, 1250000.0f);
	}
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
		ActiveProfile.AngularStiffness = FMath::Max(ActiveProfile.AngularStiffness, 12000.0f);
		ActiveProfile.AngularDamping = FMath::Max(ActiveProfile.AngularDamping, 2200.0f);
	}
	else if (ActiveProfile.TargetType == EPPGrabTargetType::GameplayItem && ActiveProfile.bRequiresTwoHands)
	{
		ActiveProfile.MovementSpeedMultiplier = FMath::Min(ActiveProfile.MovementSpeedMultiplier, 0.62f);
	}
	PresentationGrabPoint = TargetData.GrabPoint;
	FixedWorldGrabPoint = TargetData.GrabPoint;
	LockedOwnerRotation = GetOwner()->GetActorRotation();
	LockedPushAxis = GetOwner()->GetActorForwardVector().GetSafeNormal2D();
	LockedGrabbedRotation = GrabbedPrimitive ? GrabbedPrimitive->GetComponentRotation() : FRotator::ZeroRotator;
	SustainedGrabSeconds = 0.0f;
	ActivePlayerGrabBoneActorHeightOffset = 0.0f;
	bMatchGrabDummyCarryHeight = false;

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
			MulticastGrabRejected();
			return;
		}
		PhysicsHandle->SetLinearStiffness(ActiveProfile.LinearStiffness);
		PhysicsHandle->SetLinearDamping(ActiveProfile.LinearDamping);
		PhysicsHandle->SetAngularStiffness(ActiveProfile.AngularStiffness);
		PhysicsHandle->SetAngularDamping(ActiveProfile.AngularDamping);
		PhysicsHandle->SetInterpolationSpeed(18.0f);
		if (ActiveProfile.TargetType == EPPGrabTargetType::LargePushable)
		{
			GrabbedPrimitive->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
			GrabbedPrimitive->SetCollisionResponseToAllChannels(ECR_Block);
			GrabbedPrimitive->SetCollisionResponseToChannel(ECC_Camera, ECR_Ignore);
			GrabbedPrimitive->SetNotifyRigidBodyCollision(true);
			if (ACharacter* OwnerCharacter = Cast<ACharacter>(GetOwner()))
			{
				if (UCapsuleComponent* OwnerCapsule = OwnerCharacter->GetCapsuleComponent())
				{
					OwnerCapsule->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
					OwnerCapsule->SetCollisionResponseToChannel(ECC_PhysicsBody, ECR_Block);
					OwnerCapsule->SetCollisionResponseToChannel(ECC_WorldDynamic, ECR_Block);
				}
			}
		}
		if (ActiveProfile.bMaintainRotation || ActiveProfile.TargetType == EPPGrabTargetType::LargePushable)
		{
			PhysicsHandle->GrabComponentAtLocationWithRotation(
				GrabbedPrimitive,
				NAME_None,
				TargetData.GrabPoint,
				LockedGrabbedRotation);
		}
		else
		{
			PhysicsHandle->GrabComponentAtLocation(GrabbedPrimitive, NAME_None, TargetData.GrabPoint);
		}

		if (!PhysicsHandle->GetGrabbedComponent())
		{
			GrabbedPrimitive = nullptr;
			SetGrabPresentation(EPPGrabState::None, nullptr);
			MulticastGrabRejected();
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
		if (!TargetCharacter || !TargetMesh || !TargetGrabber)
		{
			GrabbedPrimitive = nullptr;
			SetGrabPresentation(EPPGrabState::None, nullptr);
			MulticastGrabRejected();
			return;
		}

		// A held character is a deterministic kinematic carry on every peer. Chaos
		// starts only for the eventual throw/impact ragdoll; otherwise the server's
		// Physics Handle rotation and each client's independent simulation fight and
		// the grabbed owner spins while other peers see a different pose.
		TargetGrabber->BeginIncomingPlayerGrab(GetOwner());
		if (TargetGrabber->IncomingGrabber != GetOwner())
		{
			GrabbedPrimitive = nullptr;
			SetGrabPresentation(EPPGrabState::None, nullptr);
			MulticastGrabRejected();
			return;
		}

		ActivePlayerGrabBone = ResolvePlayerGrabBone(TargetMesh);
		GrabbedPrimitive = TargetMesh;
		PresentationGrabPoint = GetGrabDummyCarryLocation();
		LockedGrabbedRotation = TargetCharacter->GetActorRotation();
		if (IsGrabDummyPenguin(TargetCharacter))
		{
			CarriedGrabDummy = TargetCharacter;
			LockedGrabDummyCarryRotation = LockedGrabbedRotation;
		}
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
		if (UPPGrabberComponent* TargetGrabber =
			TargetData.TargetActor->FindComponentByClass<UPPGrabberComponent>())
		{
			// BeginIncomingPlayerGrab intentionally validates against this outgoing
			// state. Complete the authority handoff in the same frame now that it has
			// been published instead of waiting for component tick order.
			TargetGrabber->UpdateIncomingCarryPresentation();
		}
		break;
	}
	case EPPGrabTargetType::LargePushable:
		SetGrabPresentation(EPPGrabState::PushingObject, TargetData.TargetActor);
		break;
	case EPPGrabTargetType::LedgeOrHandle:
		// Publish the handoff first so the dive-cancel presentation can see that
		// this is a ledge grab, rather than restoring a landing/get-up montage.
		SetGrabPresentation(EPPGrabState::HangingFromLedge, TargetData.TargetActor);
		if (UPPPlayerActionRouterComponent* ActionRouter = GetOwner()->FindComponentByClass<UPPPlayerActionRouterComponent>())
		{
			ActionRouter->CancelAirDiveForLedgeGrab();
		}
		if (ACharacter* OwnerCharacter = Cast<ACharacter>(GetOwner()))
		{
			if (UCharacterMovementComponent* OwnerMovement = OwnerCharacter->GetCharacterMovement())
			{
				OwnerMovement->StopMovementImmediately();
				OwnerMovement->SetMovementMode(MOVE_Flying);
			}
			OwnerCharacter->SetActorLocationAndRotation(
				FixedWorldGrabPoint - FVector(0.0f, 0.0f, 70.0f),
				LockedOwnerRotation,
				true,
				nullptr,
				ETeleportType::TeleportPhysics);
		}
		break;
	case EPPGrabTargetType::None:
	default:
		SetGrabPresentation(EPPGrabState::None, nullptr);
		MulticastGrabRejected();
		return;
	}
	SetComponentTickEnabled(true);
}

void UPPGrabberComponent::ReleaseGrabAuthoritative(
	bool bPlayRecoveryAnimation,
	bool bPlayLedgeClimbAnimation,
	bool bTransitionToRagdoll)
{
	if (!GetOwner() || !GetOwner()->HasAuthority())
	{
		return;
	}

	AActor* PreviousGrabTarget = GrabTarget;
	const EPPGrabState PreviousGrabState = GrabState;
	const bool bWasPairedRelease = bReleasingPairedGrab;
	bReleasingPairedGrab = true;
	if ((PreviousGrabState != EPPGrabState::HangingFromLedge || bPlayLedgeClimbAnimation)
		&& PreviousGrabState != EPPGrabState::Reaching)
	{
		SetGrabPresentation(EPPGrabState::Releasing, PreviousGrabTarget);
	}
	if (PhysicsHandle)
	{
		PhysicsHandle->ReleaseComponent();
	}
	if (CarriedGrabDummy.IsValid())
	{
		EndGrabDummyCarry();
	}
	if (PreviousGrabState == EPPGrabState::HangingFromLedge)
	{
		if (ACharacter* OwnerCharacter = Cast<ACharacter>(GetOwner()))
		{
			if (UCharacterMovementComponent* OwnerMovement = OwnerCharacter->GetCharacterMovement())
			{
				OwnerMovement->StopMovementImmediately();
				OwnerMovement->SetMovementMode(MOVE_Falling);
			}
		}
	}
	if (UPPGrabbableComponent* GrabbableComponent = FindGrabbableComponent(PreviousGrabTarget))
	{
		IPPGrabbable::Execute_OnGrabEnded(GrabbableComponent, GetOwner());
	}
	else
	{
		NotifyBlueprintGrabEvent(PreviousGrabTarget, false);
	}
	if ((PreviousGrabState == EPPGrabState::GrabbingPlayer || PreviousGrabState == EPPGrabState::MutualGrab)
		&& PreviousGrabTarget)
	{
		if (UPPGrabberComponent* TargetGrabber = PreviousGrabTarget->FindComponentByClass<UPPGrabberComponent>())
		{
			TargetGrabber->EndIncomingPlayerGrab(GetOwner(), true, bTransitionToRagdoll);
			if (!bTransitionToRagdoll
				&& PreviousGrabState == EPPGrabState::MutualGrab && !bWasPairedRelease
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
	ActivePlayerGrabBoneActorHeightOffset = 0.0f;
	bMatchGrabDummyCarryHeight = false;
	ConstraintForceEstimate = 0.0f;
	SustainedGrabSeconds = 0.0f;
	SetComponentTickEnabled(false);
	SetGrabPresentation(EPPGrabState::None, nullptr);
	bReleasingPairedGrab = false;
	if (!bTransitionToRagdoll && bPlayRecoveryAnimation && PreviousGrabState != EPPGrabState::None
		&& PreviousGrabState != EPPGrabState::PushingObject
		&& PreviousGrabState != EPPGrabState::Reaching
		&& (PreviousGrabState != EPPGrabState::HangingFromLedge || bPlayLedgeClimbAnimation))
	{
		MulticastGrabReleasedPresentation(
			PreviousGrabState == EPPGrabState::HoldingItem,
			PreviousGrabState == EPPGrabState::HangingFromLedge && bPlayLedgeClimbAnimation);
	}
	ActiveProfile = FPPGrabProfile();
}

void UPPGrabberComponent::PerformHeldItemThrowWithReachGrace(
	const FVector& ThrowDirection,
	float ChargeAlpha)
{
	if (GrabState != EPPGrabState::Reaching || !GetWorld())
	{
		PerformHeldItemThrow(ThrowDirection, ChargeAlpha);
		return;
	}

	// ServerBeginGrab and a following throw share an ordered actor channel, but
	// authority reach acquisition intentionally runs on a short cadence. Use the
	// same one-window grace for listen-server hosts and joining clients, then call
	// the non-deferring implementation exactly once.
	TWeakObjectPtr<UPPGrabberComponent> WeakGrabber(this);
	const FVector DeferredThrowDirection = ThrowDirection;
	const float DeferredChargeAlpha = FMath::Clamp(ChargeAlpha, 0.0f, 1.0f);
	FTimerHandle DeferredThrowTimerHandle;
	GetWorld()->GetTimerManager().SetTimer(
		DeferredThrowTimerHandle,
		[WeakGrabber, DeferredThrowDirection, DeferredChargeAlpha]()
		{
			if (UPPGrabberComponent* GrabberComponent = WeakGrabber.Get())
			{
				GrabberComponent->PerformHeldItemThrow(DeferredThrowDirection, DeferredChargeAlpha);
			}
		},
		0.10f,
		false);
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
	const bool bThrowingGrabDummy = ThrownCharacter && ThrownCharacter == CarriedGrabDummy.Get();
	UPrimitiveComponent* ThrownPrimitive = GrabbedPrimitive;
	const FVector InheritedVelocity = OwnerActor->GetVelocity();
	const float ThrowSpeed = FMath::Lerp(HeldItemThrowSpeed * 0.55f, HeldItemThrowSpeed * 2.15f, FMath::Clamp(ChargeAlpha, 0.0f, 1.0f));

	// Players and the dedicated dummy now share one carry-to-ragdoll transaction.
	// The target is detached in a throw handoff that deliberately leaves movement,
	// capsule collision, and mesh collision disabled. The physical-state component
	// then publishes its first snapshot only after the authored launch exists.
	if (ThrownCharacter)
	{
		if (UPPPhysicalStateComponent* ThrownPhysicalState =
			UPPPhysicalStateComponent::FindPhysicalStateComponent(ThrownCharacter))
		{
			ReleaseGrabAuthoritative(false, false, true);
			ThrownPhysicalState->RequestDummyThrowProfileRagdoll(
				ValidatedThrowDirection,
				ChargeAlpha,
				InheritedVelocity,
				HeldItemThrowSpeed,
				true);
			MulticastGrabThrowPresentation(ChargeAlpha >= 0.45f);
			return;
		}
	}

	ReleaseGrabAuthoritative(false);
	MulticastGrabThrowPresentation(ChargeAlpha >= 0.45f);
	if (bThrowingGrabDummy)
	{
		if (UPPPhysicalStateComponent* DummyPhysicalState = UPPPhysicalStateComponent::FindPhysicalStateComponent(ThrownCharacter))
		{
			DummyPhysicalState->RequestDummyThrowProfileRagdoll(
				ValidatedThrowDirection,
				ChargeAlpha,
				InheritedVelocity,
				HeldItemThrowSpeed);
		}
		else
		{
			// Preserve the old non-physical fallback while the normal dummy path uses
			// the same shared profile as authored course-obstacle impacts.
			const FVector DummyInheritedVelocity(
				InheritedVelocity.X,
				InheritedVelocity.Y,
				FMath::Clamp(InheritedVelocity.Z, -120.0f, 120.0f));
			const FVector DummyThrowVelocity = DummyInheritedVelocity
				+ ValidatedThrowDirection * (ThrowSpeed * 0.70f);
			ThrownCharacter->LaunchCharacter(DummyThrowVelocity, true, true);
		}
		return;
	}
	if (ThrownCharacter)
	{
		const FVector PlayerThrowVelocity = InheritedVelocity + ValidatedThrowDirection * (ThrowSpeed * 0.8f);
		ThrownCharacter->LaunchCharacter(PlayerThrowVelocity, true, true);
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

void UPPGrabberComponent::PerformLedgeClimb()
{
	ACharacter* OwnerCharacter = Cast<ACharacter>(GetOwner());
	if (!OwnerCharacter || !OwnerCharacter->HasAuthority() || GrabState != EPPGrabState::HangingFromLedge)
	{
		return;
	}

	const FVector TowardLedge = (FixedWorldGrabPoint - OwnerCharacter->GetActorLocation()).GetSafeNormal2D();
	const UCapsuleComponent* OwnerCapsule = OwnerCharacter->GetCapsuleComponent();
	const float ForwardClearance = OwnerCapsule ? OwnerCapsule->GetScaledCapsuleRadius() + 22.0f : 56.0f;
	const float VerticalClearance = OwnerCapsule ? OwnerCapsule->GetScaledCapsuleHalfHeight() + 12.0f : 100.0f;
	const FVector ClimbDirection = TowardLedge.IsNearlyZero() ? LockedPushAxis : TowardLedge;
	const FVector ClimbLocation = FixedWorldGrabPoint
		+ ClimbDirection * ForwardClearance
		+ FVector::UpVector * VerticalClearance;

	ReleaseGrabAuthoritative(true, true);
	OwnerCharacter->SetActorLocationAndRotation(
		ClimbLocation,
		LockedOwnerRotation,
		false,
		nullptr,
		ETeleportType::TeleportPhysics);
	if (UCharacterMovementComponent* OwnerMovement = OwnerCharacter->GetCharacterMovement())
	{
		OwnerMovement->StopMovementImmediately();
		OwnerMovement->SetMovementMode(MOVE_Walking);
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
	MulticastGrabRejected();
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
	UPPGrabberComponent* TargetGrabber = TargetCharacter
		? TargetCharacter->FindComponentByClass<UPPGrabberComponent>()
		: nullptr;
	if (!TargetCharacter || !TargetMesh || !TargetGrabber
		|| TargetGrabber->IncomingGrabber != GetOwner()
		|| !IsStandingPlayerStateValid(GetOwner())
		|| !IsStandingPlayerStateValid(TargetCharacter))
	{
		UE_LOG(LogVNH, Warning, TEXT("PairPressureGrab: player hold released by invalid state. Grabber=%s Target=%s Mode=%s"),
			*GetNameSafe(GetOwner()),
			*GetNameSafe(TargetCharacter),
			TEXT("KinematicCarry"));
		ReleaseGrabAuthoritative(false);
		return;
	}
	SustainedGrabSeconds += DeltaTime;

	const FVector CurrentGrabPoint = TargetCharacter->GetActorLocation();
	FPPGrabTargetData CurrentTargetData;
	CurrentTargetData.TargetActor = TargetCharacter;
	CurrentTargetData.TargetPrimitive = TargetMesh;
	CurrentTargetData.GrabPoint = CurrentGrabPoint;
	const FVector GrabOrigin = GetGrabOrigin();
	const FVector DesiredTargetLocation = GetGrabDummyCarryLocation();
	const FVector RawError = DesiredTargetLocation - CurrentGrabPoint;

	ConstraintForceEstimate = RawError.Size() * ActiveProfile.LinearStiffness;
	PresentationGrabPoint = DesiredTargetLocation;

	if (!CarriedGrabDummy.IsValid()
		&& OpponentEscapeSeconds > 0.0f && GrabState == EPPGrabState::GrabbingPlayer && !IsFriendlyPlayer(TargetCharacter)
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

bool UPPGrabberComponent::IsGrabDummyPenguin(const ACharacter* TargetCharacter) const
{
	return TargetCharacter && TargetCharacter->GetClass()
		&& TargetCharacter->GetClass()->GetName().Contains(TEXT("BP_PP_GrabDummyPenguin"), ESearchCase::IgnoreCase);
}

void UPPGrabberComponent::BeginGrabDummyCarry(ACharacter* TargetCharacter)
{
	if (!GetOwner() || !TargetCharacter)
	{
		return;
	}
	CarriedGrabDummy = TargetCharacter;
	LockedGrabDummyCarryRotation = TargetCharacter->GetActorRotation();
	if (UPPGrabberComponent* TargetGrabber = TargetCharacter->FindComponentByClass<UPPGrabberComponent>())
	{
		TargetGrabber->BeginIncomingPlayerGrab(GetOwner());
	}
}

void UPPGrabberComponent::EndGrabDummyCarry()
{
	ACharacter* DummyCharacter = CarriedGrabDummy.Get();
	CarriedGrabDummy.Reset();
	LockedGrabDummyCarryRotation = FRotator::ZeroRotator;
	if (DummyCharacter)
	{
		DummyCharacter->ForceNetUpdate();
	}
}

void UPPGrabberComponent::ApplyGrabDummyCarryPresentation(ACharacter* TargetCharacter, bool bIsCarried)
{
	if (!GetOwner() || !TargetCharacter)
	{
		return;
	}
	if (UPPGrabberComponent* TargetGrabber = TargetCharacter->FindComponentByClass<UPPGrabberComponent>())
	{
		TargetGrabber->ApplyIncomingGrabRagdoll(bIsCarried);
	}
}

void UPPGrabberComponent::UpdateRemoteGrabPresentation(float DeltaTime)
{
	(void)DeltaTime;
}

void UPPGrabberComponent::UpdatePushable(float /*DeltaTime*/)
{
	if (!PhysicsHandle || !GrabbedPrimitive || !GrabTarget || !PhysicsHandle->GetGrabbedComponent()
		|| IncomingGrabber || !IsStandingPlayerStateValid(GetOwner()))
	{
		ReleaseGrabAuthoritative();
		return;
	}

	GetOwner()->SetActorRotation(LockedOwnerRotation, ETeleportType::None);
	GrabbedPrimitive->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
	GrabbedPrimitive->SetCollisionResponseToChannel(ECC_Pawn, ECR_Block);
	GrabbedPrimitive->SetPhysicsAngularVelocityInDegrees(FVector::ZeroVector, false);
	GrabbedPrimitive->SetWorldRotation(LockedGrabbedRotation, false, nullptr, ETeleportType::TeleportPhysics);
	if (ACharacter* OwnerCharacter = Cast<ACharacter>(GetOwner()))
	{
		if (UCapsuleComponent* OwnerCapsule = OwnerCharacter->GetCapsuleComponent())
		{
			OwnerCapsule->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
			OwnerCapsule->SetCollisionResponseToChannel(ECC_PhysicsBody, ECR_Block);
			OwnerCapsule->SetCollisionResponseToChannel(ECC_WorldDynamic, ECR_Block);
			const FVector BlockCenter = GrabbedPrimitive->Bounds.Origin;
			const FVector BlockExtent = GrabbedPrimitive->Bounds.BoxExtent;
			const float ProjectedBlockExtent = FMath::Abs(LockedPushAxis.X) * BlockExtent.X
				+ FMath::Abs(LockedPushAxis.Y) * BlockExtent.Y;
			const float MinimumCenterSeparation = ProjectedBlockExtent
				+ OwnerCapsule->GetScaledCapsuleRadius()
				+ 4.0f;
			const float CurrentForwardSeparation = FVector::DotProduct(
				BlockCenter - OwnerCharacter->GetActorLocation(),
				LockedPushAxis);
			if (CurrentForwardSeparation < MinimumCenterSeparation)
			{
				const FVector CorrectedOwnerLocation = OwnerCharacter->GetActorLocation()
					- LockedPushAxis * (MinimumCenterSeparation - CurrentForwardSeparation);
				OwnerCharacter->SetActorLocation(
					CorrectedOwnerLocation,
					true,
					nullptr,
					ETeleportType::None);
			}
		}
	}
	FVector DesiredLocation = GetGrabOrigin() + LockedPushAxis * ActiveProfile.CarryDistance;
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
	PhysicsHandle->SetTargetRotation(LockedGrabbedRotation);
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

	OwnerMovement->StopMovementImmediately();
	OwnerMovement->SetMovementMode(MOVE_Flying);
	OwnerCharacter->SetActorLocationAndRotation(
		DesiredActorLocation,
		LockedOwnerRotation,
		true,
		nullptr,
		ETeleportType::TeleportPhysics);
	PresentationGrabPoint = FixedWorldGrabPoint;
	if (bDrawGrabDebug && GetWorld())
	{
		DrawDebugLine(GetWorld(), GetGrabOrigin(), FixedWorldGrabPoint, FColor::Emerald, false, 0.05f, 0, 2.0f);
	}
}

void UPPGrabberComponent::UpdateIncomingCarryPresentation()
{
	ACharacter* OwnerCharacter = Cast<ACharacter>(GetOwner());
	ACharacter* CarrierCharacter = Cast<ACharacter>(IncomingGrabber);
	UPPGrabberComponent* CarrierGrabber = CarrierCharacter
		? CarrierCharacter->FindComponentByClass<UPPGrabberComponent>()
		: nullptr;
	const UPPPhysicalStateComponent* PhysicalState =
		UPPPhysicalStateComponent::FindPhysicalStateComponent(OwnerCharacter);
	if (!OwnerCharacter || !CarrierCharacter || !CarrierGrabber
		|| (PhysicalState && PhysicalState->IsRagdolled()))
	{
		if (PhysicalState && PhysicalState->IsRagdolled())
		{
			PrepareIncomingCarryForRagdoll();
		}
		return;
	}

	// During a thrown ragdoll the incoming pointer intentionally remains set until
	// recovery, making a late pointer-clear harmless. The carrier's outgoing state
	// is the order-independent guard that prevents this stale Held presentation
	// from ever reattaching after the throw transaction has begun.
	if (CarrierGrabber->GrabTarget != OwnerCharacter
		|| (CarrierGrabber->GrabState != EPPGrabState::GrabbingPlayer
			&& CarrierGrabber->GrabState != EPPGrabState::MutualGrab))
	{
		return;
	}

	if (!bIncomingGrabRagdollApplied)
	{
		ApplyIncomingGrabRagdoll(true);
	}
	if (!bIncomingGrabRagdollApplied)
	{
		return;
	}

	USceneComponent* OwnerRoot = OwnerCharacter->GetRootComponent();
	USceneComponent* CarrierRoot = CarrierCharacter->GetRootComponent();
	if (!OwnerRoot || !CarrierRoot)
	{
		return;
	}
	if (OwnerCharacter->GetAttachParentActor() != CarrierCharacter)
	{
		OwnerCharacter->AttachToComponent(CarrierRoot, FAttachmentTransformRules::KeepWorldTransform);
		OwnerRoot->SetAbsolute(false, true, false);
	}

	const FVector DesiredCarryLocation = CarrierGrabber->GetGrabDummyCarryLocation();
	OwnerRoot->SetRelativeLocation(
		CarrierRoot->GetComponentTransform().InverseTransformPositionNoScale(DesiredCarryLocation),
		false,
		nullptr,
		ETeleportType::TeleportPhysics);
	if (OwnerCharacter->HasAuthority())
	{
		// AttachmentReplication carries this authoritative absolute rotation to
		// proxies. Re-locking an independently sampled client rotation every frame
		// makes the same held penguin face differently on host and owning client.
		OwnerRoot->SetWorldRotation(LockedGrabDummyCarryRotation, false, nullptr, ETeleportType::TeleportPhysics);
	}
}

void UPPGrabberComponent::BeginIncomingPlayerGrab(AActor* NewIncomingGrabber)
{
	if (!GetOwner() || !GetOwner()->HasAuthority() || !NewIncomingGrabber)
	{
		return;
	}

	if (GrabState != EPPGrabState::None)
	{
		// Reaching has no GrabTarget yet, but its periodic search is still active.
		// Cancel every outgoing action before this target becomes carried.
		ReleaseGrabAuthoritative(true);
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
	GetOwner()->ForceNetUpdate();
}

void UPPGrabberComponent::EndIncomingPlayerGrab(
	AActor* PreviousIncomingGrabber,
	bool bApplyImmunity,
	bool bTransitionToRagdoll)
{
	if (!GetOwner() || !GetOwner()->HasAuthority() || IncomingGrabber != PreviousIncomingGrabber)
	{
		return;
	}
	if (bApplyImmunity && PreviousIncomingGrabber && GetWorld())
	{
		ImmuneGrabber = PreviousIncomingGrabber;
		ImmunityEndTimeSeconds = GetWorld()->GetTimeSeconds() + SameOpponentImmunitySeconds;
	}
	if (bTransitionToRagdoll)
	{
		// Keep the single replicated target-side owner pointer alive throughout the
		// physical-state transition. It no longer presents Held because the carrier
		// has cleared its outgoing state, but a delayed pointer notification also can
		// no longer restore walking before the ragdoll snapshot arrives.
		PrepareIncomingCarryForRagdoll();
		GetOwner()->ForceNetUpdate();
		return;
	}

	IncomingGrabber = nullptr;
	OnRep_GrabPresentation();
	if (GrabState == EPPGrabState::None)
	{
		SetComponentTickEnabled(false);
	}
	GetOwner()->ForceNetUpdate();
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

	if (bEnableRagdoll)
	{
		if (const UPPPhysicalStateComponent* PhysicalState =
			UPPPhysicalStateComponent::FindPhysicalStateComponent(OwnerCharacter);
			PhysicalState && PhysicalState->IsRagdolled())
		{
			return;
		}

		bIncomingGrabRagdollApplied = true;
		IncomingGrabInitialMeshRelativeTransform = OwnerMesh->GetRelativeTransform();
		LockedGrabDummyCarryRotation = OwnerCharacter->GetActorRotation();
		if (UCharacterMovementComponent* OwnerMovement = OwnerCharacter->GetCharacterMovement())
		{
			OwnerMovement->StopMovementImmediately();
			OwnerMovement->DisableMovement();
		}
		if (UCapsuleComponent* OwnerCapsule = OwnerCharacter->GetCapsuleComponent())
		{
			OwnerCapsule->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		}
		OwnerMesh->SetAllPhysicsLinearVelocity(FVector::ZeroVector, false);
		OwnerMesh->SetAllPhysicsAngularVelocityInDegrees(FVector::ZeroVector, false);
		OwnerMesh->SetAllBodiesSimulatePhysics(false);
		OwnerMesh->SetAllBodiesPhysicsBlendWeight(0.0f, false);
		OwnerMesh->SetEnablePhysicsBlending(false);
		OwnerMesh->SetEnableGravity(false);
		OwnerMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		OwnerMesh->SetRelativeTransform(IncomingGrabInitialMeshRelativeTransform);
		OwnerMesh->bPauseAnims = true;
		OwnerMesh->GlobalAnimRateScale = 1.0f;

		if (ACharacter* CarrierCharacter = Cast<ACharacter>(IncomingGrabber))
		{
			if (USceneComponent* OwnerRoot = OwnerCharacter->GetRootComponent())
			{
				if (USceneComponent* CarrierRoot = CarrierCharacter->GetRootComponent())
				{
					OwnerCharacter->AttachToComponent(CarrierRoot, FAttachmentTransformRules::KeepWorldTransform);
					OwnerRoot->SetAbsolute(false, true, false);
				}
			}
		}
		SetComponentTickEnabled(true);
		UpdateIncomingCarryPresentation();
		UE_LOG(LogVNH, Display, TEXT("PairPressureGrab: incoming kinematic carry enabled. Target=%s PhysicsAsset=%s"),
			*GetNameSafe(OwnerCharacter),
			*GetNameSafe(OwnerMesh->GetPhysicsAsset()));
		return;
	}

	bIncomingGrabRagdollApplied = false;
	if (OwnerCharacter->GetAttachParentActor())
	{
		OwnerCharacter->DetachFromActor(FDetachmentTransformRules::KeepWorldTransform);
	}
	if (USceneComponent* OwnerRoot = OwnerCharacter->GetRootComponent())
	{
		OwnerRoot->SetAbsolute(false, false, false);
	}
	LockedGrabDummyCarryRotation = FRotator::ZeroRotator;

	if (const UPPPhysicalStateComponent* PhysicalState = UPPPhysicalStateComponent::FindPhysicalStateComponent(OwnerCharacter);
		PhysicalState && PhysicalState->IsRagdolled())
	{
		return;
	}

	OwnerMesh->SetAllBodiesSimulatePhysics(false);
	OwnerMesh->SetAllBodiesPhysicsBlendWeight(0.0f, false);
	OwnerMesh->SetAllUseCCD(false);
	OwnerMesh->SetEnablePhysicsBlending(false);
	OwnerMesh->SetEnableGravity(true);
	OwnerMesh->bPauseAnims = false;
	OwnerMesh->GlobalAnimRateScale = 1.0f;
	OwnerMesh->SetCollisionProfileName(TEXT("CharacterMesh"));
	OwnerMesh->SetRelativeTransform(IncomingGrabInitialMeshRelativeTransform);
	if (UCapsuleComponent* OwnerCapsule = OwnerCharacter->GetCapsuleComponent())
	{
		OwnerCapsule->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
	}
	if (UCharacterMovementComponent* OwnerMovement = OwnerCharacter->GetCharacterMovement())
	{
		OwnerMovement->SetMovementMode(IsGrabDummyPenguin(OwnerCharacter) ? MOVE_Falling : MOVE_Walking);
	}
	UE_LOG(LogVNH, Display, TEXT("PairPressureGrab: incoming kinematic carry restored. Target=%s"), *GetNameSafe(OwnerCharacter));
}

void UPPGrabberComponent::PrepareIncomingCarryForRagdoll()
{
	ACharacter* OwnerCharacter = Cast<ACharacter>(GetOwner());
	if (!OwnerCharacter)
	{
		return;
	}

	const bool bAttachedToIncomingGrabber = IncomingGrabber
		&& OwnerCharacter->GetAttachParentActor() == IncomingGrabber;
	if (bIncomingGrabRagdollApplied || bAttachedToIncomingGrabber)
	{
		// AttachmentReplication can arrive after the physical-state notification.
		// Detach idempotently even after the local hold flag was already cleared so
		// a late network attachment cannot make a thrown body orbit its carrier.
		if (bAttachedToIncomingGrabber)
		{
			OwnerCharacter->DetachFromActor(FDetachmentTransformRules::KeepWorldTransform);
		}
		if (USceneComponent* OwnerRoot = OwnerCharacter->GetRootComponent())
		{
			OwnerRoot->SetAbsolute(false, false, false);
		}
		LockedGrabDummyCarryRotation = FRotator::ZeroRotator;
	}
	bIncomingGrabRagdollApplied = false;

	// An unrelated physical-state entry while held must release the carrier too.
	// During the normal throw path the carrier has already entered Releasing, so
	// this guard is non-recursive.
	if (OwnerCharacter->HasAuthority() && IncomingGrabber)
	{
		if (UPPGrabberComponent* CarrierGrabber = IncomingGrabber->FindComponentByClass<UPPGrabberComponent>();
			CarrierGrabber && CarrierGrabber->GrabTarget == OwnerCharacter
			&& (CarrierGrabber->GrabState == EPPGrabState::GrabbingPlayer
				|| CarrierGrabber->GrabState == EPPGrabState::MutualGrab))
		{
			CarrierGrabber->ReleaseGrabAuthoritative(false, false, true);
		}
	}
}

void UPPGrabberComponent::FinishIncomingCarryRagdoll()
{
	if (!GetOwner() || !GetOwner()->HasAuthority() || !IncomingGrabber)
	{
		return;
	}

	IncomingGrabber = nullptr;
	OnRep_GrabPresentation();
	GetOwner()->ForceNetUpdate();
	if (GrabState == EPPGrabState::None)
	{
		SetComponentTickEnabled(false);
	}
}

void UPPGrabberComponent::PlayGetUpFrontAnimation(ACharacter* RecoveringCharacter)
{
	if (!RecoveringCharacter || !GetWorld())
	{
		return;
	}

	USkeletalMeshComponent* RecoveringMesh = RecoveringCharacter->GetMesh();
	if (!RecoveringMesh)
	{
		return;
	}
	const UPPPhysicalStateComponent* PhysicalState =
		UPPPhysicalStateComponent::FindPhysicalStateComponent(RecoveringCharacter);
	const UPPGrabberComponent* RecoveringGrabber =
		RecoveringCharacter->FindComponentByClass<UPPGrabberComponent>();
	if (PhysicalState && PhysicalState->IsRagdolled())
	{
		return;
	}
	if (RecoveringGrabber && RecoveringGrabber->GetIncomingGrabber())
	{
		// The reliable multicast can arrive just before the replicated incoming-
		// grab pointer clears. A kinematic hold has no simulating body, so this guard
		// must not be nested under IsAnySimulatingPhysics().
		TWeakObjectPtr<UPPGrabberComponent> WeakGrabber(this);
		TWeakObjectPtr<ACharacter> WeakRecoveringCharacter(RecoveringCharacter);
		FTimerHandle DeferredGetUpTimerHandle;
		GetWorld()->GetTimerManager().SetTimer(
			DeferredGetUpTimerHandle,
			[WeakGrabber, WeakRecoveringCharacter]()
			{
				if (UPPGrabberComponent* GrabberComponent = WeakGrabber.Get())
				{
					GrabberComponent->PlayGetUpFrontAnimation(WeakRecoveringCharacter.Get());
				}
			},
			0.05f,
			false);
		return;
	}
	if (RecoveringMesh->IsAnySimulatingPhysics())
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
	if (UsesPPGrabberStablePenguinRagdoll(TargetMesh))
	{
		return FName(TEXT("hips"));
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
		TurnMultiplier = GrabState == EPPGrabState::PushingObject ? 0.05f : 1.0f;
	}
	else if (GrabState == EPPGrabState::HangingFromLedge)
	{
		MovementMultiplier = 0.0f;
		TurnMultiplier = 0.05f;
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

FVector UPPGrabberComponent::GetPlayerCarryLocation() const
{
	const AActor* OwnerActor = GetOwner();
	if (!OwnerActor)
	{
		return FVector::ZeroVector;
	}
	if (bMatchGrabDummyCarryHeight)
	{
		return GetGrabDummyCarryLocation()
			+ FVector::UpVector * ActivePlayerGrabBoneActorHeightOffset;
	}

	const FVector Forward = OwnerActor->GetActorForwardVector().GetSafeNormal2D();
	return OwnerActor->GetActorLocation()
		+ Forward * PlayerHoldDistance
		+ FVector::UpVector * PlayerHoldHeight;
}

FVector UPPGrabberComponent::GetGrabDummyCarryLocation() const
{
	const ACharacter* OwnerCharacter = Cast<ACharacter>(GetOwner());
	if (!OwnerCharacter)
	{
		return FVector::ZeroVector;
	}
	const UCapsuleComponent* OwnerCapsule = OwnerCharacter->GetCapsuleComponent();
	const float CarryDistance = (OwnerCapsule ? OwnerCapsule->GetScaledCapsuleRadius() : 42.0f) + 58.0f;
	return OwnerCharacter->GetActorLocation()
		+ OwnerCharacter->GetActorForwardVector().GetSafeNormal2D() * CarryDistance
		+ FVector::UpVector * 8.0f;
}

void UPPGrabberComponent::SetGrabPresentation(EPPGrabState NewGrabState, AActor* NewGrabTarget)
{
	const bool bPresentationChanged = GrabState != NewGrabState || GrabTarget != NewGrabTarget;
	GrabState = NewGrabState;
	GrabTarget = NewGrabTarget;
	if (bPresentationChanged)
	{
		OnRep_GrabPresentation();
		if (GetOwner() && GetOwner()->HasAuthority())
		{
			GetOwner()->ForceNetUpdate();
		}
	}
}
