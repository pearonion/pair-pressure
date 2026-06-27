#include "VNHShopperWaypoint.h"

AVNHShopperWaypoint::AVNHShopperWaypoint()
{
	PrimaryActorTick.bCanEverTick = false;

	SceneRoot = CreateDefaultSubobject<USceneComponent>(TEXT("SceneRoot"));
	SetRootComponent(SceneRoot);
}
