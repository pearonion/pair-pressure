#include "VNHLog.h"

#if WITH_EDITOR

#include "Animation/AnimSequence.h"
#include "Animation/Skeleton.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Engine/SkeletalMesh.h"
#include "HAL/IConsoleManager.h"
#include "Misc/PackageName.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"

namespace
{
struct FPPMascotSkeletonRepairSpec
{
	const TCHAR* Species;
	const TCHAR* SkeletonAssetPath;
	const TCHAR* MeshAssetPath;
	const TCHAR* AnimationPackagePath;
};

bool SavePPMascotRepairedAsset(UObject* Asset)
{
	if (!Asset)
	{
		return false;
	}

	UPackage* Package = Asset->GetOutermost();
	if (!Package)
	{
		return false;
	}

	const FString PackageFilename = FPackageName::LongPackageNameToFilename(
		Package->GetName(),
		FPackageName::GetAssetPackageExtension());
	FSavePackageArgs SaveArguments;
	SaveArguments.TopLevelFlags = RF_Public | RF_Standalone;
	SaveArguments.SaveFlags = SAVE_NoError;
	return UPackage::SavePackage(Package, Asset, *PackageFilename, SaveArguments);
}

void RepairPPMascotSkeletonBindings()
{
	static constexpr FPPMascotSkeletonRepairSpec RepairSpecs[] =
	{
		{
			TEXT("Penguin"),
			TEXT("/Game/CuteChubbyPenguin/Penguin/Meshes/SKEL_Penguin_UE_Skeleton.SKEL_Penguin_UE_Skeleton"),
			TEXT("/Game/CuteChubbyPenguin/Penguin/Meshes/SK_Penguin_UE.SK_Penguin_UE"),
			TEXT("/Game/CuteChubbyPenguin/Penguin/Animations")
		},
		{
			TEXT("Dog"),
			TEXT("/Game/CuteChubbyDog/Dog/Meshes/SKEL_Dog_UE_Skeleton.SKEL_Dog_UE_Skeleton"),
			TEXT("/Game/CuteChubbyDog/Dog/Meshes/SK_Dog_UE.SK_Dog_UE"),
			TEXT("/Game/CuteChubbyDog/Dog/Animations")
		},
		{
			TEXT("Cat"),
			TEXT("/Game/CuteChubbyCat/Cat/Meshes/SKEL_cat_UE_Skeleton.SKEL_cat_UE_Skeleton"),
			TEXT("/Game/CuteChubbyCat/Cat/Meshes/SK_cat_UE.SK_cat_UE"),
			TEXT("/Game/CuteChubbyCat/Cat/Animations")
		},
		{
			TEXT("Fox"),
			TEXT("/Game/CuteChubbyFox/Fox/Meshes/SKEL_Fox_UE_Skeleton.SKEL_Fox_UE_Skeleton"),
			TEXT("/Game/CuteChubbyFox/Fox/Meshes/SK_Fox_UE.SK_Fox_UE"),
			TEXT("/Game/CuteChubbyFox/Fox/Animations")
		},
		{
			TEXT("Panda"),
			TEXT("/Game/CuteChubbyPanda/Panda/Meshes/SKEL_Panda_UE_Skeleton.SKEL_Panda_UE_Skeleton"),
			TEXT("/Game/CuteChubbyPanda/Panda/Meshes/SK_Panda_UE.SK_Panda_UE"),
			TEXT("/Game/CuteChubbyPanda/Panda/Animations")
		},
		{
			TEXT("Pig"),
			TEXT("/Game/CuteChubbyPig/Pig/Meshes/SKEL_Pig_UE_Skeleton.SKEL_Pig_UE_Skeleton"),
			TEXT("/Game/CuteChubbyPig/Pig/Meshes/SK_Pig_UE.SK_Pig_UE"),
			TEXT("/Game/CuteChubbyPig/Pig/Animations")
		},
		{
			TEXT("Raccoon"),
			TEXT("/Game/CuteChubbyRacoon/Raccoon/Meshes/SKEL_Raccoon_Skeleton.SKEL_Raccoon_Skeleton"),
			TEXT("/Game/CuteChubbyRacoon/Raccoon/Meshes/SK_Raccoon.SK_Raccoon"),
			TEXT("/Game/CuteChubbyRacoon/Raccoon/Animations")
		},
		{
			TEXT("Bird"),
			TEXT("/Game/CuteChubbyBird/Bird/Meshes/SKEL_Bird_Skeleton.SKEL_Bird_Skeleton"),
			TEXT("/Game/CuteChubbyBird/Bird/Meshes/SK_Bird.SK_Bird"),
			TEXT("/Game/CuteChubbyBird/Bird/Animations")
		},
		{
			TEXT("Bunny"),
			TEXT("/Game/CuteChubbyBunny/Bunny/Meshes/SKEL_Bunny_Skeleton.SKEL_Bunny_Skeleton"),
			TEXT("/Game/CuteChubbyBunny/Bunny/Meshes/SK_Bunny.SK_Bunny"),
			TEXT("/Game/CuteChubbyBunny/Bunny/Animations")
		},
		{
			TEXT("Cow"),
			TEXT("/Game/CuteChubbyCow/Cow/Meshes/SKEL_Cow_Skeleton.SKEL_Cow_Skeleton"),
			TEXT("/Game/CuteChubbyCow/Cow/Meshes/SK_Cow.SK_Cow"),
			TEXT("/Game/CuteChubbyCow/Cow/Animations")
		},
		{
			TEXT("Frog"),
			TEXT("/Game/CuteChubbyFrog/Frog/Meshes/SKEL_Frog_Skeleton.SKEL_Frog_Skeleton"),
			TEXT("/Game/CuteChubbyFrog/Frog/Meshes/SK_Frog.SK_Frog"),
			TEXT("/Game/CuteChubbyFrog/Frog/Animations")
		},
		{
			TEXT("Koala"),
			TEXT("/Game/CuteChubbyKoala/Koala/Meshes/SKEL_Koala_Skeleton.SKEL_Koala_Skeleton"),
			TEXT("/Game/CuteChubbyKoala/Koala/Meshes/SK_Koala.SK_Koala"),
			TEXT("/Game/CuteChubbyKoala/Koala/Animations")
		},
		{
			TEXT("Shihtzu"),
			TEXT("/Game/CuteChubbyShihtzu/Shihtzu/Meshes/SKEL_Shihtzu_Skeleton.SKEL_Shihtzu_Skeleton"),
			TEXT("/Game/CuteChubbyShihtzu/Shihtzu/Meshes/SK_Shihtzu.SK_Shihtzu"),
			TEXT("/Game/CuteChubbyShihtzu/Shihtzu/Animations")
		}
	};

	FAssetRegistryModule& AssetRegistryModule =
		FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
	int32 RepairedMeshCount = 0;
	int32 RepairedAnimationCount = 0;
	int32 FailedSaveCount = 0;

	for (const FPPMascotSkeletonRepairSpec& RepairSpec : RepairSpecs)
	{
		USkeleton* MascotSkeleton = LoadObject<USkeleton>(nullptr, RepairSpec.SkeletonAssetPath);
		USkeletalMesh* MascotMesh = LoadObject<USkeletalMesh>(nullptr, RepairSpec.MeshAssetPath);
		if (!MascotSkeleton || !MascotMesh)
		{
			UE_LOG(
				LogVNH,
				Error,
				TEXT("MascotSkeletonRepair: missing skeleton or mesh for %s. Skeleton=%s Mesh=%s"),
				RepairSpec.Species,
				MascotSkeleton ? TEXT("Valid") : TEXT("Missing"),
				MascotMesh ? TEXT("Valid") : TEXT("Missing"));
			continue;
		}

		if (MascotMesh->GetSkeleton() != MascotSkeleton)
		{
			MascotSkeleton->Modify();
			MascotMesh->Modify();
			MascotSkeleton->MergeAllBonesToBoneTree(MascotMesh);
			MascotMesh->SetSkeleton(MascotSkeleton);
			MascotSkeleton->MarkPackageDirty();
			MascotMesh->MarkPackageDirty();
			FailedSaveCount += SavePPMascotRepairedAsset(MascotSkeleton) ? 0 : 1;
			FailedSaveCount += SavePPMascotRepairedAsset(MascotMesh) ? 0 : 1;
			++RepairedMeshCount;
		}

		FARFilter AnimationFilter;
		AnimationFilter.PackagePaths.Add(FName(RepairSpec.AnimationPackagePath));
		AnimationFilter.ClassPaths.Add(UAnimSequence::StaticClass()->GetClassPathName());
		AnimationFilter.bRecursivePaths = true;
		TArray<FAssetData> AnimationAssets;
		AssetRegistryModule.Get().GetAssets(AnimationFilter, AnimationAssets);
		for (const FAssetData& AnimationAssetData : AnimationAssets)
		{
			UAnimSequence* AnimationSequence = Cast<UAnimSequence>(AnimationAssetData.GetAsset());
			if (!AnimationSequence || AnimationSequence->GetSkeleton() == MascotSkeleton)
			{
				continue;
			}

			AnimationSequence->Modify();
			AnimationSequence->SetSkeleton(MascotSkeleton);
			AnimationSequence->MarkPackageDirty();
			FailedSaveCount += SavePPMascotRepairedAsset(AnimationSequence) ? 0 : 1;
			++RepairedAnimationCount;
		}
	}

	UE_LOG(
		LogVNH,
		Display,
		TEXT("MascotSkeletonRepair: complete. Meshes=%d Animations=%d FailedSaves=%d"),
		RepairedMeshCount,
		RepairedAnimationCount,
		FailedSaveCount);
}

FAutoConsoleCommand RepairPPMascotSkeletonBindingsCommand(
	TEXT("vnh.RepairMascotSkeletonBindings"),
	TEXT("Bind every mascot mesh and animation to the skeleton used by its active runtime mesh and save them."),
	FConsoleCommandDelegate::CreateStatic(&RepairPPMascotSkeletonBindings));
}

#endif
