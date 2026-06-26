// Copyright 2025 Betide Studio. All Rights Reserved.

#include "Tools/AssetImportUtils.h"

// Unity builds can enter this file with <winuser.h>'s LoadImage macro already
// defined by an earlier .cpp in the same translation unit. Clear it before
// ImageUtils.h so FImageUtils declares LoadImage, not LoadImageW.
#ifdef LoadImage
#undef LoadImage
#endif
#include "ImageUtils.h"
#include "NeoStackAIModule.h"
#include "Misc/Base64.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "HAL/PlatformFileManager.h"
#include "HAL/FileManager.h"

// Asset import
#include "AssetToolsModule.h"
#include "IAssetTools.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetImportTask.h"
#include "Editor.h"
#include "Subsystems/AssetEditorSubsystem.h"

// Texture import (ImageUtils.h is included at the very top — see note there)
#include "Engine/Texture2D.h"
#include "Factories/TextureFactory.h"
#include "EditorFramework/AssetImportData.h"
#include "ImageCore.h"

// Static mesh import
#include "Engine/StaticMesh.h"

// Audio import
#include "Sound/SoundWave.h"

// HTTP for downloads
#include "HttpModule.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"

// Some editor/Windows includes can define LoadImage after ImageUtils.h. Clear it
// again so the call site remains FImageUtils::LoadImage.
#ifdef LoadImage
#undef LoadImage
#endif

namespace AssetImportUtils
{
	namespace
	{
		FVector3f NormalizedChroma(const FColor& Color)
		{
			FVector3f V(Color.R / 255.0f, Color.G / 255.0f, Color.B / 255.0f);
			const float Length = V.Length();
			if (Length <= UE_SMALL_NUMBER)
			{
				return FVector3f::ZeroVector;
			}
			return V / Length;
		}

		bool NormalizeImportDestinationPath(FString& InOutPath, FString& OutError)
		{
			InOutPath.TrimStartAndEndInline();
			while (InOutPath.Len() > 1 && InOutPath.EndsWith(TEXT("/")))
			{
				InOutPath.LeftChopInline(1);
			}

			const FString ProbePackageName = InOutPath / TEXT("__NeoStackImportValidation__");
			FText Reason;
			if (!FPackageName::IsValidLongPackageName(ProbePackageName, /*bIncludeReadOnlyRoots=*/false, &Reason))
			{
				OutError = FString::Printf(TEXT("Invalid destination path '%s': %s"), *InOutPath, *Reason.ToString());
				return false;
			}

			return true;
		}

		FString GeneratePrefixedUniqueAssetName(const FString& BaseName, const FString& AssetPath, const TCHAR* Prefix)
		{
			FString Name = SanitizeAssetName(BaseName);
			if (!Name.StartsWith(Prefix))
			{
				Name = FString(Prefix) + Name;
			}
			return GenerateUniqueAssetName(Name, AssetPath);
		}

		TArray<UObject*> ImportAssetsWithTask(const FString& SourceFile, const FString& DestinationPath,
			const FString& DestinationName, UFactory* Factory)
		{
			IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools").Get();

			UAssetImportTask* Task = NewObject<UAssetImportTask>();
			Task->Filename = SourceFile;
			Task->DestinationPath = DestinationPath;
			Task->DestinationName = DestinationName;
			Task->bAutomated = true;
			Task->bSave = false;
			Task->bAsync = false;
			Task->Factory = Factory;

			TArray<UAssetImportTask*> Tasks;
			Tasks.Add(Task);
			AssetTools.ImportAssetTasks(Tasks);
			return Task->GetObjects();
		}

		bool EnsureImportedAssetName(UObject* Asset, const FString& AssetPath, const FString& FinalAssetName, FString& OutError)
		{
			if (!Asset)
			{
				OutError = TEXT("Import failed - no asset to rename");
				return false;
			}
			if (Asset->GetName() == FinalAssetName)
			{
				return true;
			}

			IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools").Get();
			TArray<FAssetRenameData> RenameData;
			RenameData.Emplace(Asset, AssetPath, FinalAssetName);
			if (!AssetTools.RenameAssets(RenameData))
			{
				OutError = FString::Printf(TEXT("Import succeeded but failed to rename '%s' to '%s'"),
					*Asset->GetPathName(), *FinalAssetName);
				return false;
			}
			return true;
		}

		float ChromaDistance(const FColor& Color, const FVector3f& KeyChroma)
		{
			return FVector3f::Distance(NormalizedChroma(Color), KeyChroma);
		}

		float BackgroundAmountFromChroma(const FColor& Color, const FVector3f& KeyChroma, float Tolerance, float Softness)
		{
			const float Distance = ChromaDistance(Color, KeyChroma);
			if (Distance <= Tolerance)
			{
				return 1.0f;
			}
			if (Distance >= Tolerance + Softness)
			{
				return 0.0f;
			}
			return 1.0f - FMath::Clamp((Distance - Tolerance) / FMath::Max(Softness, 0.001f), 0.0f, 1.0f);
		}

		void DecontaminateAgainstKey(FColor& Pixel, const FColor& Key, float Alpha)
		{
			if (Alpha <= 0.001f)
			{
				Pixel = FColor(0, 0, 0, 0);
				return;
			}

			const float InvAlpha = 1.0f / Alpha;
			auto RemoveKey = [Alpha, InvAlpha](uint8 Src, uint8 KeyChannel) -> uint8
			{
				const float Foreground = ((Src / 255.0f) - (1.0f - Alpha) * (KeyChannel / 255.0f)) * InvAlpha;
				return static_cast<uint8>(FMath::RoundToInt(FMath::Clamp(Foreground, 0.0f, 1.0f) * 255.0f));
			};

			Pixel.R = RemoveKey(Pixel.R, Key.R);
			Pixel.G = RemoveKey(Pixel.G, Key.G);
			Pixel.B = RemoveKey(Pixel.B, Key.B);
		}

		void DespillGreen(FColor& Pixel, float Amount)
		{
			if (Amount <= 0.0f)
			{
				return;
			}

			const uint8 TargetG = FMath::Max(Pixel.R, Pixel.B);
			if (Pixel.G > TargetG)
			{
				Pixel.G = static_cast<uint8>(FMath::RoundToInt(FMath::Lerp(float(Pixel.G), float(TargetG), Amount)));
			}
		}

		void ChokeAlpha(TArrayView64<FColor> Pixels, int32 Width, int32 Height, int32 Iterations)
		{
			if (Iterations <= 0)
			{
				return;
			}

			TArray<uint8> SourceAlpha;
			TArray<uint8> DestAlpha;
			SourceAlpha.SetNumUninitialized(Width * Height);
			DestAlpha.SetNumUninitialized(Width * Height);

			for (int32 Index = 0; Index < Width * Height; ++Index)
			{
				SourceAlpha[Index] = Pixels[Index].A;
			}

			for (int32 Iteration = 0; Iteration < Iterations; ++Iteration)
			{
				for (int32 Y = 0; Y < Height; ++Y)
				{
					for (int32 X = 0; X < Width; ++X)
					{
						uint8 MinAlpha = SourceAlpha[Y * Width + X];
						for (int32 OffsetY = -1; OffsetY <= 1; ++OffsetY)
						{
							for (int32 OffsetX = -1; OffsetX <= 1; ++OffsetX)
							{
								const int32 SampleX = X + OffsetX;
								const int32 SampleY = Y + OffsetY;
								if (SampleX >= 0 && SampleY >= 0 && SampleX < Width && SampleY < Height)
								{
									MinAlpha = FMath::Min(MinAlpha, SourceAlpha[SampleY * Width + SampleX]);
								}
							}
						}
						DestAlpha[Y * Width + X] = MinAlpha;
					}
				}
				Swap(SourceAlpha, DestAlpha);
			}

			for (int32 Index = 0; Index < Width * Height; ++Index)
			{
				Pixels[Index].A = SourceAlpha[Index];
				if (Pixels[Index].A == 0)
				{
					Pixels[Index] = FColor(0, 0, 0, 0);
				}
			}
		}

		TArray<uint8> DilateMask(const TArray<uint8>& SourceMask, int32 Width, int32 Height, int32 Iterations)
		{
			if (Iterations <= 0)
			{
				return SourceMask;
			}

			TArray<uint8> Current = SourceMask;
			TArray<uint8> Next;
			Next.SetNumZeroed(Width * Height);

			for (int32 Iteration = 0; Iteration < Iterations; ++Iteration)
			{
				Next = Current;
				for (int32 Y = 0; Y < Height; ++Y)
				{
					for (int32 X = 0; X < Width; ++X)
					{
						const int32 Index = Y * Width + X;
						if (!Current[Index])
						{
							continue;
						}

						for (int32 OffsetY = -1; OffsetY <= 1; ++OffsetY)
						{
							for (int32 OffsetX = -1; OffsetX <= 1; ++OffsetX)
							{
								const int32 SampleX = X + OffsetX;
								const int32 SampleY = Y + OffsetY;
								if (SampleX >= 0 && SampleY >= 0 && SampleX < Width && SampleY < Height)
								{
									Next[SampleY * Width + SampleX] = 1;
								}
							}
						}
					}
				}
				Swap(Current, Next);
			}

			return Current;
		}

		FString PrepareImageForImport(const FString& SourceFile, const FGeneratedTextureImportOptions& Options, FString& OutError)
		{
			if (!Options.Background.bEnableChromaKey)
			{
				return SourceFile;
			}

			FImage Image;
			if (!FImageUtils::LoadImage(*SourceFile, Image))
			{
				OutError = FString::Printf(TEXT("Failed to decode image for background cleanup: %s"), *SourceFile);
				return FString();
			}

			Image.ChangeFormat(ERawImageFormat::BGRA8, EGammaSpace::sRGB);
			TArrayView64<FColor> Pixels = Image.AsBGRA8();

			const FColor Key = Options.Background.ChromaKeyColor;
			const FVector3f KeyChroma = NormalizedChroma(Key);
			const float Tolerance = FMath::Clamp(Options.Background.Tolerance / 255.0f, 0.0f, 1.75f);
			const float Softness = FMath::Clamp(Options.Background.Softness / 255.0f, 0.001f, 1.75f);
			const float SpillRange = FMath::Clamp((Options.Background.Tolerance + Options.Background.SpillCleanup) / 255.0f, 0.0f, 1.75f);
			const int32 Width = Image.SizeX;
			const int32 Height = Image.SizeY;
			TArray<float> BackgroundAmounts;
			TArray<uint8> KeyRegion;
			BackgroundAmounts.SetNumUninitialized(Width * Height);
			KeyRegion.SetNumZeroed(Width * Height);
			int64 ChangedPixels = 0;

			for (int32 Index = 0; Index < Width * Height; ++Index)
			{
				BackgroundAmounts[Index] = BackgroundAmountFromChroma(Pixels[Index], KeyChroma, Tolerance, Softness);
			}

			if (Options.Background.bFloodFromEdges)
			{
				TArray<uint8> Visited;
				Visited.SetNumZeroed(Width * Height);
				TArray<int32> Queue;
				Queue.Reserve(Width * 2 + Height * 2);

				auto TryEnqueue = [&](int32 X, int32 Y)
				{
					if (X < 0 || Y < 0 || X >= Width || Y >= Height)
					{
						return;
					}
					const int32 Index = Y * Width + X;
					const uint8 MatteAlpha = static_cast<uint8>(FMath::RoundToInt((1.0f - BackgroundAmounts[Index]) * Pixels[Index].A));
					if (Visited[Index] || MatteAlpha > Options.Background.FloodAlphaThreshold)
					{
						return;
					}
					Visited[Index] = 1;
					KeyRegion[Index] = 1;
					Queue.Add(Index);
				};

				for (int32 X = 0; X < Width; ++X)
				{
					TryEnqueue(X, 0);
					TryEnqueue(X, Height - 1);
				}
				for (int32 Y = 0; Y < Height; ++Y)
				{
					TryEnqueue(0, Y);
					TryEnqueue(Width - 1, Y);
				}

				for (int32 Cursor = 0; Cursor < Queue.Num(); ++Cursor)
				{
					const int32 Index = Queue[Cursor];
					const int32 X = Index % Width;
					const int32 Y = Index / Width;
					TryEnqueue(X + 1, Y);
					TryEnqueue(X - 1, Y);
					TryEnqueue(X, Y + 1);
					TryEnqueue(X, Y - 1);
				}
			}
			else
			{
				for (uint8& Value : KeyRegion)
				{
					Value = 1;
				}
			}

			const TArray<uint8> DespillRegion = DilateMask(KeyRegion, Width, Height, Options.Background.DespillEdgePixels);

			for (int32 Index = 0; Index < Width * Height; ++Index)
			{
				FColor& Pixel = Pixels[Index];
				const float BackgroundAmount = BackgroundAmounts[Index];
				if (KeyRegion[Index] && BackgroundAmount > 0.0f)
				{
					const float Alpha = FMath::Clamp(1.0f - BackgroundAmount, 0.0f, 1.0f);
					Pixel.A = static_cast<uint8>(FMath::RoundToInt(Pixel.A * Alpha));
					if (Pixel.A > 0)
					{
						DecontaminateAgainstKey(Pixel, Key, Alpha);
					}
					++ChangedPixels;
				}

				const float Distance = ChromaDistance(Pixel, KeyChroma);
				if (Pixel.A > 0 && DespillRegion[Index] && Distance <= SpillRange)
				{
					const float SpillAmount = 1.0f - FMath::Clamp((Distance - Tolerance) / FMath::Max(SpillRange - Tolerance, 0.001f), 0.0f, 1.0f);
					DespillGreen(Pixel, SpillAmount);
				}
			}

			ChokeAlpha(Pixels, Image.SizeX, Image.SizeY, Options.Background.ChokePixels);

			if (Options.Background.bFloodFromEdges)
			{
				for (int32 Index = 0; Index < Width * Height; ++Index)
				{
					if (KeyRegion[Index] && Pixels[Index].A <= Options.Background.FloodAlphaThreshold)
					{
						Pixels[Index] = FColor(0, 0, 0, 0);
					}
				}
			}

			if (ChangedPixels == 0)
			{
				UE_LOG(LogNeoStackAI, Verbose, TEXT("[AIK] Chroma key did not match any pixels in %s"), *SourceFile);
			}

			const FString TempDir = GetGeneratedContentTempDir();
			const FString ProcessedFile = TempDir / FString::Printf(TEXT("Processed_%s.png"), *FGuid::NewGuid().ToString());
			if (!FImageUtils::SaveImageByExtension(*ProcessedFile, Image))
			{
				OutError = FString::Printf(TEXT("Failed to save processed transparent PNG: %s"), *ProcessedFile);
				return FString();
			}

			return ProcessedFile;
		}

		void ApplyTexturePreset(UTexture2D* Texture, const FString& Preset)
		{
			if (!Texture || Preset.IsEmpty())
			{
				return;
			}

			const FString Normalized = Preset.ToLower();
			Texture->Modify();

			if (Normalized == TEXT("ui_icon") || Normalized == TEXT("icon") || Normalized == TEXT("ui"))
			{
				Texture->LODGroup = TEXTUREGROUP_UI;
				Texture->MipGenSettings = TMGS_NoMipmaps;
				Texture->CompressionSettings = TC_EditorIcon;
				Texture->SRGB = true;
				Texture->NeverStream = true;
			}
			else if (Normalized == TEXT("pixel_art") || Normalized == TEXT("pixel"))
			{
				Texture->LODGroup = TEXTUREGROUP_Pixels2D;
				Texture->MipGenSettings = TMGS_NoMipmaps;
				Texture->CompressionSettings = TC_EditorIcon;
				Texture->Filter = TF_Nearest;
				Texture->SRGB = true;
				Texture->NeverStream = true;
			}
			else if (Normalized == TEXT("normal_map") || Normalized == TEXT("normal"))
			{
				Texture->CompressionSettings = TC_Normalmap;
				Texture->SRGB = false;
			}
			else if (Normalized == TEXT("decal"))
			{
				Texture->LODGroup = TEXTUREGROUP_World;
				Texture->SRGB = true;
			}
			else if (Normalized == TEXT("texture") || Normalized == TEXT("default"))
			{
				Texture->LODGroup = TEXTUREGROUP_World;
				Texture->SRGB = true;
			}

			Texture->PostEditChange();
			Texture->MarkPackageDirty();
		}
	}

	FString GetGeneratedContentTempDir()
	{
		FString TempDir = FPaths::ProjectSavedDir() / TEXT("GeneratedContent");

		// Create directory if it doesn't exist
		IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
		if (!PlatformFile.DirectoryExists(*TempDir))
		{
			PlatformFile.CreateDirectoryTree(*TempDir);
		}

		return TempDir;
	}

	FString SanitizeAssetName(const FString& Input)
	{
		FString Sanitized = Input;

		// Remove invalid characters for asset names
		FString InvalidChars = TEXT(" .,:;'\"\\/?!@#$%^&*()[]{}|<>~`");
		for (int32 i = 0; i < InvalidChars.Len(); ++i)
		{
			Sanitized = Sanitized.Replace(&InvalidChars[i], TEXT("_"));
		}

		// Remove consecutive underscores
		while (Sanitized.Contains(TEXT("__")))
		{
			Sanitized = Sanitized.Replace(TEXT("__"), TEXT("_"));
		}

		// Trim underscores from start and end
		Sanitized.TrimStartAndEndInline();
		while (Sanitized.StartsWith(TEXT("_")))
		{
			Sanitized = Sanitized.RightChop(1);
		}
		while (Sanitized.EndsWith(TEXT("_")))
		{
			Sanitized = Sanitized.LeftChop(1);
		}

		// Limit length
		if (Sanitized.Len() > 64)
		{
			Sanitized = Sanitized.Left(64);
		}

		// Ensure it starts with a letter or underscore
		if (Sanitized.Len() > 0 && FChar::IsDigit(Sanitized[0]))
		{
			Sanitized = TEXT("Asset_") + Sanitized;
		}

		// Default name if empty
		if (Sanitized.IsEmpty())
		{
			Sanitized = TEXT("GeneratedAsset");
		}

		return Sanitized;
	}

	FString SaveBase64ToTempFile(const FString& Base64Data, const FString& Extension, FString& OutError)
	{
		FString DataToDecode = Base64Data;

		// Strip data URL prefix if present (e.g., "data:image/png;base64,")
		int32 CommaIndex;
		if (DataToDecode.FindChar(TEXT(','), CommaIndex))
		{
			if (DataToDecode.Left(CommaIndex).Contains(TEXT("base64")))
			{
				DataToDecode = DataToDecode.RightChop(CommaIndex + 1);
			}
		}

		// Decode base64
		TArray<uint8> DecodedData;
		if (!FBase64::Decode(DataToDecode, DecodedData))
		{
			OutError = TEXT("Failed to decode base64 data");
			return FString();
		}

		if (DecodedData.Num() == 0)
		{
			OutError = TEXT("Decoded data is empty");
			return FString();
		}

		// Generate unique filename
		FString TempDir = GetGeneratedContentTempDir();
		FString FileName = FString::Printf(TEXT("Generated_%s.%s"), *FGuid::NewGuid().ToString(), *Extension);
		FString FilePath = TempDir / FileName;

		// Write to file
		if (!FFileHelper::SaveArrayToFile(DecodedData, *FilePath))
		{
			OutError = FString::Printf(TEXT("Failed to write temp file: %s"), *FilePath);
			return FString();
		}

		UE_LOG(LogNeoStackAI, Verbose, TEXT("[AIK] Saved base64 data to temp file: %s (%d bytes)"), *FilePath, DecodedData.Num());
		return FilePath;
	}

	FString GenerateUniqueAssetName(const FString& BaseName, const FString& Path)
	{
		FString SanitizedName = SanitizeAssetName(BaseName);
		FString AssetPath = Path;

		// Check if asset exists
		FString PackageName = AssetPath / SanitizedName;
		if (!FPackageName::DoesPackageExist(PackageName))
		{
			return SanitizedName;
		}

		// Try with numeric suffix
		for (int32 i = 1; i < 1000; ++i)
		{
			FString NewName = FString::Printf(TEXT("%s_%d"), *SanitizedName, i);
			PackageName = AssetPath / NewName;
			if (!FPackageName::DoesPackageExist(PackageName))
			{
				return NewName;
			}
		}

		// Fallback with GUID
		return FString::Printf(TEXT("%s_%s"), *SanitizedName, *FGuid::NewGuid().ToString().Left(8));
	}

	UTexture2D* ImportTexture(const FString& SourceFile, const FString& DestinationPath, const FString& AssetName, FString& OutError)
	{
		FGeneratedTextureImportOptions Options;
		return ImportTexture(SourceFile, DestinationPath, AssetName, Options, OutError);
	}

	UTexture2D* ImportTexture(const FString& SourceFile, const FString& DestinationPath, const FString& AssetName,
		const FGeneratedTextureImportOptions& Options, FString& OutError)
	{
		// Validate source file exists
		if (!FPaths::FileExists(SourceFile))
		{
			OutError = FString::Printf(TEXT("Source file not found: %s"), *SourceFile);
			return nullptr;
		}

		const FString ImportSourceFile = PrepareImageForImport(SourceFile, Options, OutError);
		if (ImportSourceFile.IsEmpty())
		{
			return nullptr;
		}

		FString AssetPath = DestinationPath;
		if (!NormalizeImportDestinationPath(AssetPath, OutError))
		{
			return nullptr;
		}

		const FString FinalAssetName = GeneratePrefixedUniqueAssetName(AssetName, AssetPath, TEXT("T_"));

		// Create texture factory
		UTextureFactory* TextureFactory = NewObject<UTextureFactory>();
		TextureFactory->AddToRoot(); // Prevent GC during import
		TextureFactory->SuppressImportOverwriteDialog();

		TArray<UObject*> ImportedAssets = ImportAssetsWithTask(ImportSourceFile, AssetPath, FinalAssetName, TextureFactory);

		TextureFactory->RemoveFromRoot();

		if (ImportedAssets.Num() == 0)
		{
			OutError = TEXT("Import failed - no assets created");
			return nullptr;
		}

		UTexture2D* ImportedTexture = Cast<UTexture2D>(ImportedAssets[0]);
		if (!ImportedTexture)
		{
			OutError = TEXT("Import succeeded but result is not a Texture2D");
			return nullptr;
		}

		if (!EnsureImportedAssetName(ImportedTexture, AssetPath, FinalAssetName, OutError))
		{
			return nullptr;
		}

		// Mark dirty
		ApplyTexturePreset(ImportedTexture, Options.Preset);

		// Mark dirty
		ImportedTexture->GetPackage()->MarkPackageDirty();

		// Notify asset registry
		FAssetRegistryModule::AssetCreated(ImportedTexture);

		UE_LOG(LogNeoStackAI, Verbose, TEXT("[AIK] Successfully imported texture: %s"), *ImportedTexture->GetPathName());
		return ImportedTexture;
	}

	UStaticMesh* ImportStaticMesh(const FString& SourceFile, const FString& DestinationPath, const FString& AssetName, FString& OutError)
	{
		// Validate source file exists
		if (!FPaths::FileExists(SourceFile))
		{
			OutError = FString::Printf(TEXT("Source file not found: %s"), *SourceFile);
			return nullptr;
		}

		FString AssetPath = DestinationPath;
		if (!NormalizeImportDestinationPath(AssetPath, OutError))
		{
			return nullptr;
		}

		const FString FinalAssetName = GeneratePrefixedUniqueAssetName(AssetName, AssetPath, TEXT("SM_"));

		// Import with default settings (will use Interchange for GLB if available)
		TArray<UObject*> ImportedAssets = ImportAssetsWithTask(SourceFile, AssetPath, FinalAssetName, nullptr);

		if (ImportedAssets.Num() == 0)
		{
			OutError = TEXT("Import failed - no assets created. Ensure the file format is supported.");
			return nullptr;
		}

		// Find the static mesh in imported assets (may import multiple assets like materials)
		UStaticMesh* ImportedMesh = nullptr;
		for (UObject* Asset : ImportedAssets)
		{
			ImportedMesh = Cast<UStaticMesh>(Asset);
			if (ImportedMesh)
			{
				break;
			}
		}

		if (!ImportedMesh)
		{
			OutError = TEXT("Import succeeded but no StaticMesh found in imported assets");
			return nullptr;
		}
		if (!EnsureImportedAssetName(ImportedMesh, AssetPath, FinalAssetName, OutError))
		{
			return nullptr;
		}

		// Mark dirty
		ImportedMesh->GetPackage()->MarkPackageDirty();

		// Notify asset registry
		FAssetRegistryModule::AssetCreated(ImportedMesh);

		UE_LOG(LogNeoStackAI, Verbose, TEXT("[AIK] Successfully imported static mesh: %s"), *ImportedMesh->GetPathName());
		return ImportedMesh;
	}

	USoundWave* ImportAudio(const FString& SourceFile, const FString& DestinationPath, const FString& AssetName, FString& OutError)
	{
		if (!FPaths::FileExists(SourceFile))
		{
			OutError = FString::Printf(TEXT("Source file not found: %s"), *SourceFile);
			return nullptr;
		}

		FString AssetPath = DestinationPath;
		if (!NormalizeImportDestinationPath(AssetPath, OutError))
		{
			return nullptr;
		}

		const FString FinalAssetName = GeneratePrefixedUniqueAssetName(AssetName, AssetPath, TEXT("S_"));

		// Import with auto-detection (UE handles WAV/OGG via USoundFactory)
		TArray<UObject*> ImportedAssets = ImportAssetsWithTask(SourceFile, AssetPath, FinalAssetName, nullptr);

		if (ImportedAssets.Num() == 0)
		{
			OutError = TEXT("Import failed - no assets created. Ensure the file format is supported (WAV, OGG).");
			return nullptr;
		}

		USoundWave* ImportedSound = nullptr;
		for (UObject* Asset : ImportedAssets)
		{
			ImportedSound = Cast<USoundWave>(Asset);
			if (ImportedSound) break;
		}

		if (!ImportedSound)
		{
			OutError = TEXT("Import succeeded but no SoundWave found in imported assets");
			return nullptr;
		}
		if (!EnsureImportedAssetName(ImportedSound, AssetPath, FinalAssetName, OutError))
		{
			return nullptr;
		}

		ImportedSound->GetPackage()->MarkPackageDirty();
		FAssetRegistryModule::AssetCreated(ImportedSound);

		UE_LOG(LogNeoStackAI, Verbose, TEXT("[AIK] Successfully imported audio: %s"), *ImportedSound->GetPathName());
		return ImportedSound;
	}

	bool DownloadFile(const FString& Url, const FString& OutputPath, FString& OutError)
	{
		TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = FHttpModule::Get().CreateRequest();
		Request->SetURL(Url);
		Request->SetVerb(TEXT("GET"));
		Request->SetTimeout(300.0f); // 5 minute timeout for large files

		// Synchronous HTTP — safe from game thread (uses CompleteOnHttpThread internally)
		Request->ProcessRequestUntilComplete();

		FHttpResponsePtr Response = Request->GetResponse();
		if (Request->GetStatus() != EHttpRequestStatus::Succeeded || !Response.IsValid())
		{
			OutError = TEXT("Connection failed");
			return false;
		}

		int32 ResponseCode = Response->GetResponseCode();
		if (ResponseCode < 200 || ResponseCode >= 300)
		{
			OutError = FString::Printf(TEXT("HTTP %d: %s"), ResponseCode, *Response->GetContentAsString().Left(200));
			return false;
		}

		const TArray<uint8>& ResponseData = Response->GetContent();
		if (ResponseData.Num() == 0)
		{
			OutError = TEXT("Downloaded file is empty");
			return false;
		}

		// Ensure directory exists
		FString Directory = FPaths::GetPath(OutputPath);
		IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
		if (!PlatformFile.DirectoryExists(*Directory))
		{
			PlatformFile.CreateDirectoryTree(*Directory);
		}

		// Write to file
		if (!FFileHelper::SaveArrayToFile(ResponseData, *OutputPath))
		{
			OutError = FString::Printf(TEXT("Failed to write file: %s"), *OutputPath);
			return false;
		}

		UE_LOG(LogNeoStackAI, Verbose, TEXT("[AIK] Downloaded file: %s (%d bytes)"), *OutputPath, ResponseData.Num());
		return true;
	}
}
