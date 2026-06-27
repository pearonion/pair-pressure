// Copyright 2026 Betide Studio. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Tools/NeoStackToolBase.h"

class FEditorViewportClient;
class UObject;
class UWidgetBlueprint;

/**
 * Unified screenshot tool that captures images from both the level editor viewport
 * and open asset editor viewports. Provides full camera control so AI agents can
 * view scenes and assets from any angle, zoom level, or perspective.
 *
 * Supports explicit level mode, explicit asset mode, and active mode (default) that captures
 * whichever viewport the user is currently working in.
 */
class NEOSTACKAI_API FScreenshotTool : public FNeoStackToolBase
{
public:
	virtual FString GetName() const override { return TEXT("screenshot"); }

	virtual FString GetDescription() const override
	{
		return TEXT("Captures a screenshot and returns it as a PNG image.\n\n"
			"THREE MODES:\n"
			"1) ACTIVE (default): Captures what the user is currently focused on. "
			"If an asset editor viewport is active, it captures that. Otherwise it captures the active level viewport.\n\n"
			"2) LEVEL VIEWPORT: Captures the editor viewport showing the 3D world. "
			"Control the camera with location/rotation/fov, auto-frame an actor with focus_actor, "
			"or change the render mode with view_mode.\n\n"
			"3) ASSET EDITOR: Captures the viewport of an asset open in its editor "
			"(e.g., StaticMesh, SkeletalMesh, Material, Blueprint, Niagara, Animation, Widget Blueprint). "
			"Shows the asset exactly as you see it in the editor with proper environment lighting. "
			"Control the orbit camera with orbit_yaw/orbit_pitch/orbit_distance. "
			"For Widget Blueprints, captures the clean UMG preview by default; pass widget_capture='designer' "
			"to intentionally capture Designer overlays/rulers/safe-zone chrome/current pan. "
			"If the asset editor is not open, falls back to a basic thumbnail render for meshes.\n\n"
			"SHARED OPTIONS:\n"
			"- hide_overlays: Set to true to hide viewport gizmos, grid, selection outlines, and icons.\n"
			"- widget_capture: For Widget Blueprint assets, use 'preview' for clean UMG output or 'designer' for editor overlays.");
	}

	virtual TSharedPtr<FJsonObject> GetInputSchema() const override;

	// Async: warmup runs on a per-tick ticker so the editor stays interactive. Sync
	// prep (camera setup, viewport find) and post-warmup work (read pixels, encode)
	// remain on the game thread but are bounded; only the warmup loop, which can be
	// up to wait_for_ready_ms (default 1500ms), yields.
	virtual void Execute(const TSharedPtr<FJsonObject>& Args, const FResultCallback& OnComplete) override;
	virtual bool IsSynchronous() const override { return false; }

private:
	enum class EScreenshotMode : uint8
	{
		Active,
		Level,
		Asset
	};

	/** Info about a found editor viewport */
	struct FEditorViewportInfo
	{
		FEditorViewportClient* ViewportClient = nullptr;
		FViewport* Viewport = nullptr;
	};

	/** Capture using "what the user is actively working on" semantics */
	void CaptureActiveViewport(const TSharedPtr<FJsonObject>& Args, int32 MaxDimension, FResultCallback OnComplete);

	/** Capture from a specific asset path */
	void CaptureAssetViewport(const TSharedPtr<FJsonObject>& Args, int32 MaxDimension, const FString& AssetPath, FResultCallback OnComplete);

	/** Find the 3D viewport inside an open asset editor */
	FEditorViewportInfo FindAssetEditorViewport(UObject* Asset);

	/** Find the currently focused editor viewport (asset or level) */
	FEditorViewportInfo FindFocusedEditorViewport() const;

	/** Find the most recently activated edited asset */
	UObject* FindMostRecentlyActivatedEditedAsset() const;

	/** Ensure a viewport has valid dimensions and has been rendered at least once */
	bool EnsureViewportIsReady(FEditorViewportClient* ViewportClient, FViewport* Viewport) const;

	/** Capture from an open editor viewport with optional orbit camera control */
	void CaptureEditorViewport(
		FEditorViewportClient* ViewportClient, FViewport* Viewport,
		int32 MaxDimension, const FString& AssetName,
		bool bHasOrbitYaw, double OrbitYaw,
		bool bHasOrbitPitch, double OrbitPitch,
		bool bHasOrbitDistance, double OrbitDistance,
		bool bHasFOV, float FOV,
		int32 WaitForReadyMs,
		bool bHideOverlays,
		FResultCallback OnComplete);

	/** Capture the level editor viewport with free camera control */
	void CaptureLevelViewport(const TSharedPtr<FJsonObject>& Args,
		int32 ViewportIndex, int32 MaxDimension, FResultCallback OnComplete);

	/** Capture the UMG Widget Blueprint preview, independent of designer pan/zoom */
	FToolResult CaptureWidgetDesigner(UWidgetBlueprint* WidgetBP, int32 MaxDimension);

	/** Capture the UMG Widget Blueprint Designer tab with editor overlays/pan/zoom */
	FToolResult CaptureWidgetDesignerOverlays(UWidgetBlueprint* WidgetBP, int32 MaxDimension);

	/** Thumbnail fallback for assets without an open editor */
	template<typename TScene>
	FToolResult CaptureThumbnailScene(TScene& ThumbnailScene, int32 Resolution,
		const FString& MeshName, const FString& MeshType);

	/** Shared: downscale pixels if needed, PNG encode, base64, return as image result */
	FToolResult EncodeAndReturn(TArray<FColor>& Pixels, int32 Width, int32 Height,
		int32 MaxDimension, const FString& Message);
};
