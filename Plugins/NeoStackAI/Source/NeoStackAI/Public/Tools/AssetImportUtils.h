// Copyright 2025 Betide Studio. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

class UTexture2D;
class UStaticMesh;
class USoundWave;

struct FGeneratedImageBackgroundOptions
{
	bool bEnableChromaKey = false;
	FColor ChromaKeyColor = FColor::Green;
	int32 Tolerance = 40;
	bool bForcePng = true;
	int32 Softness = 40;
	int32 SpillCleanup = 80;
	int32 DespillEdgePixels = 2;
	bool bFloodFromEdges = true;
	int32 FloodAlphaThreshold = 64;
	int32 ChokePixels = 0;
};

struct FGeneratedTextureImportOptions
{
	FString Preset;
	FGeneratedImageBackgroundOptions Background;
};

/**
 * Shared utilities for importing generated content as UE assets.
 * Used by image generation and 3D model generation tools.
 */
namespace AssetImportUtils
{
	/**
	 * Decode base64 string and save to a temporary file.
	 * @param Base64Data The base64 encoded data (can include data URL prefix like "data:image/png;base64,")
	 * @param Extension File extension to use (e.g., "png", "glb")
	 * @param OutError Error message if failed
	 * @return Path to the temporary file, or empty string if failed
	 */
	NEOSTACKAI_API FString SaveBase64ToTempFile(const FString& Base64Data, const FString& Extension, FString& OutError);

	/**
	 * Import an image file as UTexture2D.
	 * @param SourceFile Path to the source image file (PNG, JPG, etc.)
	 * @param DestinationPath UE asset path (e.g., "/Game/GeneratedImages")
	 * @param AssetName Name for the texture asset (without path)
	 * @param OutError Error message if failed
	 * @return The imported texture, or nullptr if failed
	 */
	NEOSTACKAI_API UTexture2D* ImportTexture(const FString& SourceFile, const FString& DestinationPath, const FString& AssetName, FString& OutError);

	/**
	 * Import an image file as UTexture2D with optional generated-image cleanup and texture preset.
	 */
	NEOSTACKAI_API UTexture2D* ImportTexture(const FString& SourceFile, const FString& DestinationPath, const FString& AssetName,
		const FGeneratedTextureImportOptions& Options, FString& OutError);

	/**
	 * Import a 3D model file as UStaticMesh.
	 * @param SourceFile Path to the source model file (GLB, FBX, OBJ)
	 * @param DestinationPath UE asset path (e.g., "/Game/Generated3DModels")
	 * @param AssetName Name for the static mesh asset (without path)
	 * @param OutError Error message if failed
	 * @return The imported static mesh, or nullptr if failed
	 */
	NEOSTACKAI_API UStaticMesh* ImportStaticMesh(const FString& SourceFile, const FString& DestinationPath, const FString& AssetName, FString& OutError);

	/**
	 * Import an audio file as USoundWave.
	 * @param SourceFile Path to the source audio file (WAV, OGG)
	 * @param DestinationPath UE asset path (e.g., "/Game/GeneratedAudio")
	 * @param AssetName Name for the sound wave asset (without path)
	 * @param OutError Error message if failed
	 * @return The imported sound wave, or nullptr if failed
	 */
	NEOSTACKAI_API USoundWave* ImportAudio(const FString& SourceFile, const FString& DestinationPath, const FString& AssetName, FString& OutError);

	/**
	 * Generate a unique asset name that doesn't conflict with existing assets.
	 * @param BaseName The preferred base name
	 * @param Path The asset path to check for conflicts
	 * @return A unique asset name (may have numeric suffix if base name exists)
	 */
	NEOSTACKAI_API FString GenerateUniqueAssetName(const FString& BaseName, const FString& Path);

	/**
	 * Download a file from URL to a local path.
	 * This is a blocking operation.
	 * @param Url The URL to download from
	 * @param OutputPath Local file path to save to
	 * @param OutError Error message if failed
	 * @return True if successful
	 */
	NEOSTACKAI_API bool DownloadFile(const FString& Url, const FString& OutputPath, FString& OutError);

	/**
	 * Get the temp directory for generated content.
	 * Creates the directory if it doesn't exist.
	 * @return Path to temp directory
	 */
	NEOSTACKAI_API FString GetGeneratedContentTempDir();

	/**
	 * Sanitize a string to be used as an asset name.
	 * Removes invalid characters and limits length.
	 * @param Input The input string
	 * @return Sanitized asset name
	 */
	NEOSTACKAI_API FString SanitizeAssetName(const FString& Input);
}
