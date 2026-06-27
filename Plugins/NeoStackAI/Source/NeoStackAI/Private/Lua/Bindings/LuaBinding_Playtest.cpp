// Copyright 2026 Betide Studio. All Rights Reserved.

#include "Lua/LuaBindingRegistry.h"
#include "Lua/LuaCoroutineAwait.h"
#include "Lua/LuaStr.h"
#include "Lua/NeoLuaState.h"
#include "Tools/NeoStackToolBase.h"

#include "Editor.h"
#include "Editor/EditorEngine.h"
#include "Editor/UnrealEdEngine.h"
#include "Containers/Ticker.h"
#include "GameFramework/PlayerController.h"
#include "Engine/World.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformTime.h"
#include "IAssetViewport.h"
#include "ImageUtils.h"
#include "InputKeyEventArgs.h"
#include "LevelEditor.h"
#include "Misc/Base64.h"
#include "Misc/Crc.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "PlayInEditorDataTypes.h"
#include "UnrealClient.h"
#include "UnrealEdGlobals.h"
#include "ViewportClient.h"

#define SOL_ALL_SAFETIES_ON 1
#include <sol/sol.hpp>

namespace
{
	constexpr int32 DefaultObserveMaxDimension = 1024;
	constexpr int32 MaxObserveDimension = 2048;
	constexpr int32 DefaultObserveWaitMs = 150;
	constexpr int32 MaxObserveWaitMs = 5000;
	constexpr int32 MaxWaitFrames = 6000;

	struct FPlaytestInputTarget
	{
		FViewport* Viewport = nullptr;
		FViewportClient* Client = nullptr;
		UWorld* World = nullptr;
	};

	FString GetPlaytestMode()
	{
		if (!GUnrealEd || !GUnrealEd->IsPlayingSessionInEditor())
		{
			return TEXT("none");
		}

		return GUnrealEd->IsSimulatingInEditor() ? TEXT("simulate") : TEXT("pie");
	}

	UWorld* GetPIEWorld()
	{
		if (!GUnrealEd)
		{
			return nullptr;
		}

		if (FWorldContext* PIEContext = GUnrealEd->GetPIEWorldContext())
		{
			return PIEContext->World();
		}

		return nullptr;
	}

	sol::table MakeStatusTable(sol::this_state S, bool bOk = true, const FString& Message = FString())
	{
		sol::state_view Lua(S);
		sol::table Result = Lua.create_table();

		const bool bQueued = GUnrealEd && GUnrealEd->IsPlaySessionRequestQueued();
		const bool bPlaying = GUnrealEd && GUnrealEd->IsPlayingSessionInEditor();
		const bool bInProgress = GUnrealEd && GUnrealEd->IsPlaySessionInProgress();
		const bool bSimulating = GUnrealEd && GUnrealEd->IsSimulatingInEditor();
		const bool bEnding = GUnrealEd && GUnrealEd->ShouldEndPlayMap();
		UWorld* PIEWorld = GetPIEWorld();

		Result["ok"] = bOk;
		Result["message"] = TCHAR_TO_UTF8(*Message);
		Result["queued"] = bQueued;
		Result["playing"] = bPlaying;
		Result["in_progress"] = bInProgress;
		Result["simulating"] = bSimulating;
		Result["ending"] = bEnding;
		Result["mode"] = TCHAR_TO_UTF8(*GetPlaytestMode());
		Result["has_pie_world"] = PIEWorld != nullptr;
		Result["map"] = PIEWorld ? TCHAR_TO_UTF8(*PIEWorld->GetMapName()) : "";

		return Result;
	}

	sol::table MakeActionTable(sol::this_state S, bool bOk, bool bConsumed, const FString& Message)
	{
		sol::state_view Lua(S);
		sol::table Result = Lua.create_table();
		Result["ok"] = bOk;
		Result["consumed"] = bConsumed;
		Result["message"] = TCHAR_TO_UTF8(*Message);
		Result["mode"] = TCHAR_TO_UTF8(*GetPlaytestMode());
		return Result;
	}

	sol::table MakeCheckTable(sol::this_state S, bool bOk, bool bPassed, const FString& Message)
	{
		sol::state_view Lua(S);
		sol::table Result = Lua.create_table();
		Result["ok"] = bOk;
		Result["passed"] = bPassed;
		Result["message"] = TCHAR_TO_UTF8(*Message);
		Result["mode"] = TCHAR_TO_UTF8(*GetPlaytestMode());
		return Result;
	}

	FPlaytestInputTarget GetInputTarget()
	{
		FPlaytestInputTarget Target;
		Target.World = GetPIEWorld();

		if (GUnrealEd)
		{
			Target.Viewport = GUnrealEd->GetPIEViewport();
		}

		if (Target.Viewport)
		{
			Target.Client = Target.Viewport->GetClient();
		}

		return Target;
	}

	bool RequireInputTarget(FPlaytestInputTarget& OutTarget, FString& OutError)
	{
		if (!GUnrealEd || !GUnrealEd->IsPlayingSessionInEditor())
		{
			OutError = TEXT("No active Play In Editor session. Call playtest_start() first.");
			return false;
		}

		OutTarget = GetInputTarget();
		if (!OutTarget.Viewport)
		{
			OutError = TEXT("No PIE viewport found for input.");
			return false;
		}

		if (!OutTarget.Client)
		{
			OutError = TEXT("PIE viewport has no input client.");
			return false;
		}

		return true;
	}

	FKey ParseInputKey(const FString& KeyName)
	{
		if (KeyName.Len() == 1)
		{
			const TCHAR Upper = FChar::ToUpper(KeyName[0]);
			if ((Upper >= TEXT('A') && Upper <= TEXT('Z')) || (Upper >= TEXT('0') && Upper <= TEXT('9')))
			{
				return FKey(FName(FString::Chr(Upper)));
			}
		}

		return FKey(FName(*KeyName));
	}

	bool ParseInputEvent(const FString& EventName, EInputEvent& OutEvent)
	{
		const FString Lower = EventName.ToLower();
		if (Lower == TEXT("pressed") || Lower == TEXT("press") || Lower == TEXT("down"))
		{
			OutEvent = IE_Pressed;
			return true;
		}
		if (Lower == TEXT("released") || Lower == TEXT("release") || Lower == TEXT("up"))
		{
			OutEvent = IE_Released;
			return true;
		}
		if (Lower == TEXT("repeat") || Lower == TEXT("repeated"))
		{
			OutEvent = IE_Repeat;
			return true;
		}
		if (Lower == TEXT("double_click") || Lower == TEXT("doubleclick"))
		{
			OutEvent = IE_DoubleClick;
			return true;
		}

		return false;
	}

	bool SendPlaytestKey(const FKey& Key, EInputEvent Event, float Amount, FPlaytestInputTarget& Target)
	{
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 7
		FInputKeyEventArgs Args = FInputKeyEventArgs::CreateSimulated(
			Key,
			Event,
			Amount,
			Key.IsAnalog() ? 1 : 0,
			INPUTDEVICEID_NONE,
			false,
			Target.Viewport);

		return Target.Client->InputKey(Args);
#else
		// Pre-5.7: InputKey signature varies across 5.4/5.5/5.6 (FInputKeyEventArgs lacks
		// CreateSimulated; the older positional overload was removed in 5.6). Stub out
		// rather than try to match every minor-version signature.
		(void)Key; (void)Event; (void)Amount; (void)Target;
		return false;
#endif
	}

	bool SendPlaytestAxis(const FKey& Key, float Value, float DeltaTime, int32 NumSamples, FPlaytestInputTarget& Target)
	{
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 7
		FInputKeyEventArgs Args = FInputKeyEventArgs::CreateSimulated(
			Key,
			IE_Axis,
			Value,
			FMath::Max(1, NumSamples),
			INPUTDEVICEID_NONE,
			false,
			Target.Viewport);
		Args.DeltaTime = FMath::Max(0.0f, DeltaTime);

		return Target.Client->InputAxis(Args);
#else
		// Pre-5.7: InputAxis signature varies (5.6 uses FInputKeyEventArgs sans
		// CreateSimulated; 5.4/5.5 took positional args). Stub out.
		(void)Key; (void)Value; (void)DeltaTime; (void)NumSamples; (void)Target;
		return false;
#endif
	}

	bool ReadViewportPixels(FViewport* Viewport, TArray<FColor>& OutPixels, int32& OutWidth, int32& OutHeight)
	{
		if (!Viewport)
		{
			return false;
		}

		FIntPoint Size = Viewport->GetRenderTargetTextureSizeXY();
		if (Size.X <= 0 || Size.Y <= 0)
		{
			Size = Viewport->GetSizeXY();
		}

		if (Size.X <= 0 || Size.Y <= 0)
		{
			return false;
		}

		const FIntRect CaptureRect(0, 0, Size.X, Size.Y);
		OutPixels.Reset();
		OutPixels.SetNumUninitialized(CaptureRect.Area());

		if (GetViewportScreenShot(Viewport, OutPixels, CaptureRect))
		{
			OutWidth = Size.X;
			OutHeight = Size.Y;
			return true;
		}

		if (Viewport->GetSceneHDREnabled())
		{
			TArray<FLinearColor> LinearPixels;
			LinearPixels.SetNumUninitialized(CaptureRect.Area());
			if (GetViewportScreenShotHDR(Viewport, LinearPixels, CaptureRect))
			{
				OutPixels.SetNumUninitialized(LinearPixels.Num());
				ConvertFLinearColorsToFColorSRGB(LinearPixels.GetData(), OutPixels.GetData(), LinearPixels.Num());
				OutWidth = Size.X;
				OutHeight = Size.Y;
				return true;
			}
		}

		return false;
	}

	FString GetOutputLogPath()
	{
		FString LogDir = FPaths::ProjectLogDir();
		TArray<FString> LogFiles;
		IFileManager::Get().FindFiles(LogFiles, *LogDir, TEXT("*.log"));

		FString MostRecentLog;
		FDateTime MostRecentTime = FDateTime::MinValue();
		for (const FString& LogFile : LogFiles)
		{
			const FString FullPath = LogDir / LogFile;
			const FDateTime ModTime = IFileManager::Get().GetTimeStamp(*FullPath);
			if (ModTime > MostRecentTime)
			{
				MostRecentTime = ModTime;
				MostRecentLog = FullPath;
			}
		}

		return MostRecentLog;
	}

	bool ReadOutputLogLines(TArray<FString>& OutLines, FString& OutPath, FString& OutError)
	{
		if (GLog)
		{
			GLog->Flush();
		}

		OutPath = GetOutputLogPath();
		if (OutPath.IsEmpty())
		{
			OutError = TEXT("Could not find output log file in Saved/Logs/.");
			return false;
		}

		const int64 FileSize = IFileManager::Get().FileSize(*OutPath);
		if (FileSize > 100 * 1024 * 1024)
		{
			OutError = TEXT("Output log exceeds 100MB, too large to read.");
			return false;
		}

		TUniquePtr<FArchive> Reader(IFileManager::Get().CreateFileReader(*OutPath, FILEREAD_AllowWrite));
		if (!Reader)
		{
			OutError = TEXT("Could not open output log file for shared read.");
			return false;
		}

		TArray<uint8> Bytes;
		Bytes.SetNumUninitialized(static_cast<int32>(FileSize));
		if (Bytes.Num() > 0)
		{
			Reader->Serialize(Bytes.GetData(), Bytes.Num());
			if (Reader->IsError())
			{
				OutError = TEXT("Could not read output log file.");
				return false;
			}
		}

		FString Contents;
		FFileHelper::BufferToString(Contents, Bytes.GetData(), Bytes.Num());
		Contents.ParseIntoArrayLines(OutLines, false);
		return true;
	}

	bool TryGetLineFromMarker(const sol::object& SinceObj, int32& OutLine)
	{
		OutLine = 0;
		if (!SinceObj.valid() || SinceObj == sol::lua_nil)
		{
			return false;
		}

		if (SinceObj.is<int>())
		{
			OutLine = FMath::Max(0, SinceObj.as<int>());
			return true;
		}

		if (SinceObj.is<double>())
		{
			OutLine = FMath::Max(0, static_cast<int32>(SinceObj.as<double>()));
			return true;
		}

		if (SinceObj.is<sol::table>())
		{
			sol::table Marker = SinceObj.as<sol::table>();
			OutLine = FMath::Max(0, Marker.get_or("line", Marker.get_or("total_lines", 0)));
			return true;
		}

		return false;
	}

	sol::table MakeLogMarker(sol::this_state S)
	{
		sol::state_view Lua(S);
		sol::table Result = Lua.create_table();

		TArray<FString> Lines;
		FString Path;
		FString Error;
		if (!ReadOutputLogLines(Lines, Path, Error))
		{
			Result["ok"] = false;
			Result["message"] = TCHAR_TO_UTF8(*Error);
			Result["line"] = 0;
			Result["total_lines"] = 0;
			return Result;
		}

		Result["ok"] = true;
		Result["message"] = "Log marker captured.";
		Result["line"] = Lines.Num();
		Result["total_lines"] = Lines.Num();
		Result["path"] = TCHAR_TO_UTF8(*Path);
		return Result;
	}

	sol::table FindLogText(sol::this_state S, const FString& Needle, sol::optional<sol::table> OptsOpt)
	{
		sol::state_view Lua(S);
		sol::table Result = Lua.create_table();

		if (Needle.IsEmpty())
		{
			Result["ok"] = false;
			Result["found"] = false;
			Result["message"] = "playtest_log_contains requires non-empty text.";
			return Result;
		}

		int32 SinceLine = 0;
		bool bCaseSensitive = false;
		if (OptsOpt.has_value())
		{
			sol::table Opts = OptsOpt.value();
			bCaseSensitive = Opts.get_or("case_sensitive", false);
			sol::object SinceObj = Opts["since"];
			TryGetLineFromMarker(SinceObj, SinceLine);
		}

		TArray<FString> Lines;
		FString Path;
		FString Error;
		if (!ReadOutputLogLines(Lines, Path, Error))
		{
			Result["ok"] = false;
			Result["found"] = false;
			Result["message"] = TCHAR_TO_UTF8(*Error);
			return Result;
		}

		const ESearchCase::Type SearchCase = bCaseSensitive ? ESearchCase::CaseSensitive : ESearchCase::IgnoreCase;
		for (int32 Index = FMath::Clamp(SinceLine, 0, Lines.Num()); Index < Lines.Num(); ++Index)
		{
			if (Lines[Index].Contains(Needle, SearchCase))
			{
				Result["ok"] = true;
				Result["found"] = true;
				Result["message"] = TCHAR_TO_UTF8(*FString::Printf(TEXT("Found log text: %s"), *Needle));
				Result["line"] = Index + 1;
				Result["total_lines"] = Lines.Num();
				Result["path"] = TCHAR_TO_UTF8(*Path);
				Result["text"] = TCHAR_TO_UTF8(*Lines[Index]);
				return Result;
			}
		}

		Result["ok"] = true;
		Result["found"] = false;
		Result["message"] = TCHAR_TO_UTF8(*FString::Printf(TEXT("Log text not found: %s"), *Needle));
		Result["line"] = 0;
		Result["total_lines"] = Lines.Num();
		Result["path"] = TCHAR_TO_UTF8(*Path);
		return Result;
	}

	FToolResult EncodeViewportCapture(TArray<FColor>& Pixels, int32 Width, int32 Height, int32 MaxDimension)
	{
		if (Pixels.Num() != Width * Height || Width <= 0 || Height <= 0)
		{
			return FToolResult::Fail(TEXT("Invalid PIE viewport pixel data"));
		}

		for (FColor& Pixel : Pixels)
		{
			Pixel.A = 255;
		}

		int32 OutWidth = Width;
		int32 OutHeight = Height;
		TArray<FColor>* PixelsToEncode = &Pixels;
		TArray<FColor> DownscaledPixels;

		const int32 MaxDim = FMath::Max(Width, Height);
		if (MaxDim > MaxDimension)
		{
			const float Scale = static_cast<float>(MaxDimension) / static_cast<float>(MaxDim);
			OutWidth = FMath::Max(1, static_cast<int32>(Width * Scale));
			OutHeight = FMath::Max(1, static_cast<int32>(Height * Scale));
			DownscaledPixels.SetNum(OutWidth * OutHeight);

			for (int32 Y = 0; Y < OutHeight; ++Y)
			{
				for (int32 X = 0; X < OutWidth; ++X)
				{
					const int32 SrcX = FMath::Min(static_cast<int32>(X / Scale), Width - 1);
					const int32 SrcY = FMath::Min(static_cast<int32>(Y / Scale), Height - 1);
					DownscaledPixels[Y * OutWidth + X] = Pixels[SrcY * Width + SrcX];
				}
			}

			PixelsToEncode = &DownscaledPixels;
		}

		TArray64<uint8> PNGData;
		FImageUtils::PNGCompressImageArray(OutWidth, OutHeight, TArrayView64<const FColor>(*PixelsToEncode), PNGData);
		if (PNGData.Num() == 0)
		{
			return FToolResult::Fail(TEXT("Failed to encode PIE viewport screenshot"));
		}

		const uint32 PNGSize = static_cast<uint32>(FMath::Min(PNGData.Num(), static_cast<int64>(MAX_uint32)));
		const FString Base64 = FBase64::Encode(PNGData.GetData(), PNGSize);
		const FString Message = FString::Printf(TEXT("PIE viewport screenshot (%dx%d)"), OutWidth, OutHeight);
		return FToolResult::OkWithImage(Message, Base64, TEXT("image/png"), OutWidth, OutHeight);
	}

	FToolResult CapturePIEViewport(int32 MaxDimension)
	{
		if (!GUnrealEd || !GUnrealEd->IsPlayingSessionInEditor())
		{
			return FToolResult::Fail(TEXT("No active Play In Editor session. Call playtest_start() first."));
		}

		FViewport* Viewport = GUnrealEd->GetPIEViewport();
		if (!Viewport)
		{
			return FToolResult::Fail(TEXT("No PIE viewport found for observation."));
		}

		Viewport->Draw();
		FlushRenderingCommands();

		TArray<FColor> Pixels;
		int32 Width = 0;
		int32 Height = 0;
		if (!ReadViewportPixels(Viewport, Pixels, Width, Height))
		{
			return FToolResult::Fail(TEXT("Failed to read pixels from PIE viewport."));
		}

		return EncodeViewportCapture(Pixels, Width, Height, MaxDimension);
	}

	sol::table CaptureScreenshotMarker(sol::this_state S)
	{
		sol::state_view Lua(S);
		sol::table Result = Lua.create_table();

		if (!GUnrealEd || !GUnrealEd->IsPlayingSessionInEditor())
		{
			Result["ok"] = false;
			Result["message"] = "No active Play In Editor session. Call playtest_start() first.";
			return Result;
		}

		FViewport* Viewport = GUnrealEd->GetPIEViewport();
		if (!Viewport)
		{
			Result["ok"] = false;
			Result["message"] = "No PIE viewport found for screenshot marker.";
			return Result;
		}

		Viewport->Draw();
		FlushRenderingCommands();

		TArray<FColor> Pixels;
		int32 Width = 0;
		int32 Height = 0;
		if (!ReadViewportPixels(Viewport, Pixels, Width, Height))
		{
			Result["ok"] = false;
			Result["message"] = "Failed to read pixels from PIE viewport.";
			return Result;
		}

		for (FColor& Pixel : Pixels)
		{
			Pixel.A = 255;
		}

		const int32 PixelBytes = static_cast<int32>(Pixels.Num() * sizeof(FColor));
		const uint32 Hash = FCrc::MemCrc32(Pixels.GetData(), PixelBytes);
		Result["ok"] = true;
		Result["message"] = "Screenshot marker captured.";
		Result["width"] = Width;
		Result["height"] = Height;
		Result["hash"] = TCHAR_TO_UTF8(*FString::Printf(TEXT("%08x"), Hash));
		Result["pixel_count"] = Pixels.Num();
		return Result;
	}

	bool TryGetScreenshotHash(const sol::object& Obj, FString& OutHash, FString& OutError)
	{
		if (!Obj.valid() || Obj == sol::lua_nil || !Obj.is<sol::table>())
		{
			OutError = TEXT("Expected a screenshot marker table.");
			return false;
		}

		sol::table Marker = Obj.as<sol::table>();
		if (!Marker.get_or("ok", false))
		{
			OutError = NeoLuaStr::ToFString(Marker.get_or<std::string>("message", "Screenshot marker is not ok."));
			return false;
		}

		OutHash = NeoLuaStr::ToFString(Marker.get_or<std::string>("hash", ""));
		if (OutHash.IsEmpty())
		{
			OutError = TEXT("Screenshot marker has no hash.");
			return false;
		}

		return true;
	}

	void SessionLog(const TWeakPtr<FNeoLuaState>& WeakRunner, const FString& Msg)
	{
		if (TSharedPtr<FNeoLuaState> Runner = WeakRunner.Pin())
		{
			if (FLuaSessionData* S = Runner->GetSession())
			{
				S->Log(Msg);
			}
		}
	}

	void SessionAddImages(const TWeakPtr<FNeoLuaState>& WeakRunner, const TArray<FToolResultImage>& Images)
	{
		if (TSharedPtr<FNeoLuaState> Runner = WeakRunner.Pin())
		{
			if (FLuaSessionData* S = Runner->GetSession())
			{
				S->AddImages(Images);
			}
		}
	}

	int PlaytestWaitLuaCFunc(lua_State* L)
	{
		float Seconds = 0.0f;
		if (lua_gettop(L) >= 1 && lua_isnumber(L, 1))
		{
			Seconds = static_cast<float>(lua_tonumber(L, 1));
		}
		Seconds = FMath::Clamp(Seconds, 0.0f, 120.0f);

		return UE::NeoStack::Lua::AwaitAsync(L,
			[Seconds](UE::NeoStack::Lua::FAwaitContinuation Continuation)
			{
				const TSharedRef<UE::NeoStack::Lua::FAwaitContinuation> SharedContinuation =
					MakeShared<UE::NeoStack::Lua::FAwaitContinuation>(MoveTemp(Continuation));

				FTSTicker::GetCoreTicker().AddTicker(
					FTickerDelegate::CreateLambda([SharedContinuation](float) -> bool
					{
						(*SharedContinuation)([](lua_State* CoroL) -> int
						{
							lua_pushboolean(CoroL, 1);
							return 1;
						});
						return false;
					}),
					Seconds);
			});
	}

	int PlaytestWaitFramesLuaCFunc(lua_State* L)
	{
		int32 Frames = 1;
		if (lua_gettop(L) >= 1 && lua_isnumber(L, 1))
		{
			Frames = static_cast<int32>(lua_tointeger(L, 1));
		}
		Frames = FMath::Clamp(Frames, 1, MaxWaitFrames);

		return UE::NeoStack::Lua::AwaitAsync(L,
			[Frames](UE::NeoStack::Lua::FAwaitContinuation Continuation)
			{
				const TSharedRef<UE::NeoStack::Lua::FAwaitContinuation> SharedContinuation =
					MakeShared<UE::NeoStack::Lua::FAwaitContinuation>(MoveTemp(Continuation));
				const TSharedRef<int32> RemainingFrames = MakeShared<int32>(Frames);

				FTSTicker::GetCoreTicker().AddTicker(
					FTickerDelegate::CreateLambda([SharedContinuation, RemainingFrames, Frames](float) -> bool
					{
						--(*RemainingFrames);
						if (*RemainingFrames > 0)
						{
							return true;
						}

						(*SharedContinuation)([Frames](lua_State* CoroL) -> int
						{
							lua_newtable(CoroL);
							lua_pushboolean(CoroL, 1);
							lua_setfield(CoroL, -2, "ok");
							lua_pushinteger(CoroL, Frames);
							lua_setfield(CoroL, -2, "frames");
							lua_pushliteral(CoroL, "Frame wait complete.");
							lua_setfield(CoroL, -2, "message");
							return 1;
						});
						return false;
					}),
					0.0f);
			});
	}

	int PlaytestObserveLuaCFunc(lua_State* L)
	{
		int32 MaxDimension = DefaultObserveMaxDimension;
		int32 WaitForReadyMs = DefaultObserveWaitMs;

		if (lua_gettop(L) >= 1 && lua_istable(L, 1))
		{
			sol::table T(L, 1);
			MaxDimension = FMath::Clamp(static_cast<int32>(T.get_or("max_dimension", DefaultObserveMaxDimension)), 64, MaxObserveDimension);
			WaitForReadyMs = FMath::Clamp(static_cast<int32>(T.get_or("wait_for_ready_ms", DefaultObserveWaitMs)), 0, MaxObserveWaitMs);
		}

		TWeakPtr<FNeoLuaState> WeakRunner = FNeoLuaState::FindForState(L);

		return UE::NeoStack::Lua::AwaitAsync(L,
			[MaxDimension, WaitForReadyMs, WeakRunner](UE::NeoStack::Lua::FAwaitContinuation Continuation)
			{
				const TSharedRef<UE::NeoStack::Lua::FAwaitContinuation> SharedContinuation =
					MakeShared<UE::NeoStack::Lua::FAwaitContinuation>(MoveTemp(Continuation));

				FTSTicker::GetCoreTicker().AddTicker(
					FTickerDelegate::CreateLambda([MaxDimension, WeakRunner, SharedContinuation](float) -> bool
					{
						const FToolResult Result = CapturePIEViewport(MaxDimension);
						SessionAddImages(WeakRunner, Result.Images);
						SessionLog(WeakRunner, Result.Output);

						const FString Output = Result.Output;
						(*SharedContinuation)([Output](lua_State* CoroL) -> int
						{
							lua_pushstring(CoroL, TCHAR_TO_UTF8(*Output));
							return 1;
						});
						return false;
					}),
					static_cast<float>(WaitForReadyMs) / 1000.0f);
			});
	}
}

static TArray<FLuaFunctionDoc> PlaytestDocs = {
	{ TEXT("playtest_start(opts?)"),
	  TEXT("Start an in-process PIE/SIE session. opts: {mode='pie'|'simulate', map='/Game/Maps/Map', viewport='active'}. Returns status table."),
	  TEXT("table") },
	{ TEXT("playtest_stop()"),
	  TEXT("Cancel a queued play request or request the active PIE/SIE session to end. Returns status table."),
	  TEXT("table") },
	{ TEXT("playtest_status()"),
	  TEXT("Return current PIE/SIE state: ok, queued, playing, in_progress, simulating, ending, mode, has_pie_world, map."),
	  TEXT("table") },
	{ TEXT("playtest_time()"),
	  TEXT("Return current high-resolution editor time in seconds for custom playtest timing logic."),
	  TEXT("number") },
	{ TEXT("playtest_wait(seconds)"),
	  TEXT("Yield the Lua coroutine while the editor continues ticking, then return true."),
	  TEXT("boolean") },
	{ TEXT("playtest_wait_frames(frames)"),
	  TEXT("Yield until a fixed number of editor/game ticks have passed. Returns {ok, frames, message}."),
	  TEXT("table") },
	{ TEXT("playtest_wait_for_pie(opts?)"),
	  TEXT("Wait until PIE is active and has a PIE world. opts: {timeout=5.0, interval=0.05}. Returns assertion-style table."),
	  TEXT("table") },
	{ TEXT("playtest_wait_until(condition, opts?)"),
	  TEXT("Poll a Lua function or {lua='...'} condition until it returns truthy or times out. opts: {timeout=3.0, interval=0.05}."),
	  TEXT("table") },
	{ TEXT("playtest_assert(name, fn, opts?)"),
	  TEXT("Run a Lua assertion body and convert truthy/table results or errors into {ok, passed, name, message}."),
	  TEXT("table") },
	{ TEXT("playtest_observe(opts?)"),
	  TEXT("Capture the current PIE viewport and return a description string; image is returned via the session image pipeline. opts: {max_dimension=1024, wait_for_ready_ms=150}."),
	  TEXT("string") },
	{ TEXT("playtest_screenshot_marker()"),
	  TEXT("Capture the current PIE viewport as a lightweight hash marker for later comparison. Returns {ok, width, height, hash}."),
	  TEXT("table") },
	{ TEXT("playtest_assert_screenshot_changed(before, after?)"),
	  TEXT("Compare two screenshot markers, or compare before against a fresh capture when after is omitted. Returns {ok, passed, message}."),
	  TEXT("table") },
	{ TEXT("playtest_log_marker()"),
	  TEXT("Capture the current output log line count for scoped later assertions. Returns {ok, line, total_lines, path}."),
	  TEXT("table") },
	{ TEXT("playtest_log_contains(text, opts?)"),
	  TEXT("Search the output log for text, optionally since a marker. opts: {since=marker, case_sensitive=false}."),
	  TEXT("table") },
	{ TEXT("playtest_assert_log_contains(text, opts?)"),
	  TEXT("Wait for output log text to appear. opts: {since=marker, timeout=3.0, interval=0.05, case_sensitive=false}."),
	  TEXT("table") },
	{ TEXT("playtest_key(opts)"),
	  TEXT("Send a simulated key/button event to the active PIE viewport. opts: {key='W', event='pressed'|'released'|'repeat'|'double_click', amount=1.0}."),
	  TEXT("table") },
	{ TEXT("playtest_axis(opts)"),
	  TEXT("Send a simulated analog axis event to the active PIE viewport. opts: {key='Gamepad_LeftX', value=1.0, delta_time=0.016, samples=1}."),
	  TEXT("table") },
	{ TEXT("playtest_text(text)"),
	  TEXT("Send character input to the active PIE viewport, useful for text boxes and console-like input fields."),
	  TEXT("table") },
	{ TEXT("playtest_click(opts?)"),
	  TEXT("Set the PIE viewport mouse position and send a mouse button press/release. opts: {x=0.5, y=0.5, normalized=true, button='LeftMouseButton'}."),
	  TEXT("table") },
	{ TEXT("playtest_console(command)"),
	  TEXT("Execute a console command through the first PIE player controller. Returns output when the command emits a direct return string; many stat/show commands are fire-and-forget."),
	  TEXT("table") }
};

REGISTER_LUA_BINDING(Playtest, PlaytestDocs, [](sol::state& Lua, FLuaSessionData& Session)
{
	Lua.set_function("playtest_status", [](sol::this_state S) -> sol::table
	{
		return MakeStatusTable(S);
	});

	Lua.set_function("playtest_time", []() -> double
	{
		return FPlatformTime::Seconds();
	});

	Lua.set_function("playtest_log_marker", [](sol::this_state S) -> sol::table
	{
		return MakeLogMarker(S);
	});

	Lua.set_function("playtest_log_contains", [](const std::string& Text, sol::optional<sol::table> Opts, sol::this_state S) -> sol::table
	{
		return FindLogText(S, NeoLuaStr::ToFString(Text), Opts);
	});

	Lua.set_function("playtest_screenshot_marker", [](sol::this_state S) -> sol::table
	{
		return CaptureScreenshotMarker(S);
	});

	Lua.set_function("playtest_assert_screenshot_changed", [](sol::object BeforeObj, sol::object AfterObj, sol::this_state S) -> sol::table
	{
		FString BeforeHash;
		FString Error;
		if (!TryGetScreenshotHash(BeforeObj, BeforeHash, Error))
		{
			return MakeCheckTable(S, false, false, Error);
		}

		FString AfterHash;
		if (AfterObj.valid() && AfterObj != sol::lua_nil)
		{
			if (!TryGetScreenshotHash(AfterObj, AfterHash, Error))
			{
				return MakeCheckTable(S, false, false, Error);
			}
		}
		else
		{
			sol::table Captured = CaptureScreenshotMarker(S);
			if (!Captured.get_or("ok", false))
			{
				Error = NeoLuaStr::ToFString(Captured.get_or<std::string>("message", "Screenshot marker capture failed."));
				return MakeCheckTable(S, false, false, Error);
			}

			AfterHash = NeoLuaStr::ToFString(Captured.get_or<std::string>("hash", ""));
			if (AfterHash.IsEmpty())
			{
				return MakeCheckTable(S, false, false, TEXT("Screenshot marker has no hash."));
			}
		}

		const bool bChanged = !BeforeHash.Equals(AfterHash, ESearchCase::CaseSensitive);
		sol::table Result = MakeCheckTable(S, true, bChanged,
			bChanged ? TEXT("Screenshot changed.") : TEXT("Screenshot did not change."));
		Result["before_hash"] = TCHAR_TO_UTF8(*BeforeHash);
		Result["after_hash"] = TCHAR_TO_UTF8(*AfterHash);
		return Result;
	});

	Lua.set_function("playtest_start", [&Session](sol::optional<sol::table> OptsOpt, sol::this_state S) -> sol::table
	{
		if (!GUnrealEd)
		{
			return MakeStatusTable(S, false, TEXT("UnrealEd engine is not available."));
		}

		if (GUnrealEd->IsPlaySessionInProgress())
		{
			return MakeStatusTable(S, true, TEXT("Play session is already active or queued."));
		}

		FRequestPlaySessionParams Params;
		Params.SessionDestination = EPlaySessionDestinationType::InProcess;
		Params.WorldType = EPlaySessionWorldType::PlayInEditor;

		if (OptsOpt.has_value())
		{
			sol::table Opts = OptsOpt.value();

			const FString Mode = NeoLuaStr::ToFString(Opts.get_or<std::string>("mode", "pie")).ToLower();
			if (Mode == TEXT("simulate") || Mode == TEXT("sie"))
			{
				Params.WorldType = EPlaySessionWorldType::SimulateInEditor;
			}
			else if (Mode != TEXT("pie") && Mode != TEXT("play"))
			{
				return MakeStatusTable(S, false, FString::Printf(TEXT("Unsupported playtest mode: %s"), *Mode));
			}

			const FString Map = NeoLuaStr::ToFString(Opts.get_or<std::string>("map", ""));
			if (!Map.IsEmpty())
			{
				Params.GlobalMapOverride = Map;
			}
		}

		if (FLevelEditorModule* LevelEditorModule = FModuleManager::GetModulePtr<FLevelEditorModule>("LevelEditor"))
		{
			TSharedPtr<IAssetViewport> ActiveLevelViewport = LevelEditorModule->GetFirstActiveViewport();
			if (ActiveLevelViewport.IsValid())
			{
				Params.DestinationSlateViewport = ActiveLevelViewport;

				if (Params.WorldType == EPlaySessionWorldType::PlayInEditor && GUnrealEd->CheckForPlayerStart() == nullptr)
				{
					Params.StartLocation = ActiveLevelViewport->GetAssetViewportClient().GetViewLocation();
					Params.StartRotation = ActiveLevelViewport->GetAssetViewportClient().GetViewRotation();
				}
			}
		}

		GUnrealEd->RequestPlaySession(Params);
		Session.Log(TEXT("[OK] playtest_start -> requested Play In Editor session"));
		return MakeStatusTable(S, true, TEXT("Play session requested."));
	});

	Lua.set_function("playtest_stop", [&Session](sol::this_state S) -> sol::table
	{
		if (!GUnrealEd)
		{
			return MakeStatusTable(S, false, TEXT("UnrealEd engine is not available."));
		}

		if (GUnrealEd->IsPlaySessionRequestQueued() && !GUnrealEd->IsPlayingSessionInEditor())
		{
			GUnrealEd->CancelRequestPlaySession();
			Session.Log(TEXT("[OK] playtest_stop -> canceled queued play request"));
			return MakeStatusTable(S, true, TEXT("Queued play session canceled."));
		}

		if (!GUnrealEd->IsPlayingSessionInEditor())
		{
			return MakeStatusTable(S, true, TEXT("No active play session."));
		}

		GUnrealEd->RequestEndPlayMap();
		Session.Log(TEXT("[OK] playtest_stop -> requested end play"));
		return MakeStatusTable(S, true, TEXT("End play requested."));
	});

	Lua.set_function("playtest_key", [](sol::table Opts, sol::this_state S) -> sol::table
	{
		FPlaytestInputTarget Target;
		FString Error;
		if (!RequireInputTarget(Target, Error))
		{
			return MakeActionTable(S, false, false, Error);
		}

		const FString KeyName = NeoLuaStr::ToFString(Opts.get_or<std::string>("key", ""));
		if (KeyName.IsEmpty())
		{
			return MakeActionTable(S, false, false, TEXT("playtest_key requires opts.key."));
		}

		const FKey Key = ParseInputKey(KeyName);
		if (!Key.IsValid())
		{
			return MakeActionTable(S, false, false, FString::Printf(TEXT("Unknown input key: %s"), *KeyName));
		}

		EInputEvent Event = IE_Pressed;
		const FString EventName = NeoLuaStr::ToFString(Opts.get_or<std::string>("event", "pressed"));
		if (!ParseInputEvent(EventName, Event))
		{
			return MakeActionTable(S, false, false, FString::Printf(TEXT("Unsupported input event: %s"), *EventName));
		}

		const float Amount = static_cast<float>(Opts.get_or("amount", 1.0));
		const bool bConsumed = SendPlaytestKey(Key, Event, Amount, Target);
		return MakeActionTable(S, true, bConsumed, FString::Printf(TEXT("Sent key %s %s"), *Key.ToString(), *EventName));
	});

	Lua.set_function("playtest_axis", [](sol::table Opts, sol::this_state S) -> sol::table
	{
		FPlaytestInputTarget Target;
		FString Error;
		if (!RequireInputTarget(Target, Error))
		{
			return MakeActionTable(S, false, false, Error);
		}

		const FString KeyName = NeoLuaStr::ToFString(Opts.get_or<std::string>("key", ""));
		if (KeyName.IsEmpty())
		{
			return MakeActionTable(S, false, false, TEXT("playtest_axis requires opts.key."));
		}

		const FKey Key = ParseInputKey(KeyName);
		if (!Key.IsValid())
		{
			return MakeActionTable(S, false, false, FString::Printf(TEXT("Unknown input key: %s"), *KeyName));
		}

		const float Value = static_cast<float>(Opts.get_or("value", 0.0));
		const float DeltaTime = static_cast<float>(Opts.get_or("delta_time", 1.0 / 60.0));
		const int32 NumSamples = static_cast<int32>(Opts.get_or("samples", 1));
		const bool bConsumed = SendPlaytestAxis(Key, Value, DeltaTime, NumSamples, Target);
		return MakeActionTable(S, true, bConsumed, FString::Printf(TEXT("Sent axis %s=%0.3f"), *Key.ToString(), Value));
	});

	Lua.set_function("playtest_text", [](const std::string& Text, sol::this_state S) -> sol::table
	{
		FPlaytestInputTarget Target;
		FString Error;
		if (!RequireInputTarget(Target, Error))
		{
			return MakeActionTable(S, false, false, Error);
		}

		const FString UEText = NeoLuaStr::ToFString(Text);
		bool bAnyConsumed = false;
		for (const TCHAR Character : UEText)
		{
			bAnyConsumed |= Target.Client->InputChar(Target.Viewport, 0, Character);
		}

		return MakeActionTable(S, true, bAnyConsumed, FString::Printf(TEXT("Sent %d text character(s)"), UEText.Len()));
	});

	Lua.set_function("playtest_click", [](sol::optional<sol::table> OptsOpt, sol::this_state S) -> sol::table
	{
		FPlaytestInputTarget Target;
		FString Error;
		if (!RequireInputTarget(Target, Error))
		{
			return MakeActionTable(S, false, false, Error);
		}

		FString ButtonName = TEXT("LeftMouseButton");
		double X = 0.5;
		double Y = 0.5;
		bool bNormalized = true;

		if (OptsOpt.has_value())
		{
			sol::table Opts = OptsOpt.value();
			ButtonName = NeoLuaStr::ToFString(Opts.get_or<std::string>("button", "LeftMouseButton"));
			X = Opts.get_or("x", 0.5);
			Y = Opts.get_or("y", 0.5);
			bNormalized = Opts.get_or("normalized", true);
		}

		const FKey Button = ParseInputKey(ButtonName);
		if (!Button.IsValid())
		{
			return MakeActionTable(S, false, false, FString::Printf(TEXT("Unknown mouse button key: %s"), *ButtonName));
		}

		const FIntPoint Size = Target.Viewport->GetSizeXY();
		int32 MouseX = static_cast<int32>(X);
		int32 MouseY = static_cast<int32>(Y);
		if (bNormalized)
		{
			MouseX = FMath::RoundToInt(FMath::Clamp(X, 0.0, 1.0) * FMath::Max(0, Size.X - 1));
			MouseY = FMath::RoundToInt(FMath::Clamp(Y, 0.0, 1.0) * FMath::Max(0, Size.Y - 1));
		}
		Target.Viewport->SetMouse(MouseX, MouseY);

		const bool bPressed = SendPlaytestKey(Button, IE_Pressed, 1.0f, Target);
		const bool bReleased = SendPlaytestKey(Button, IE_Released, 1.0f, Target);
		return MakeActionTable(S, true, bPressed || bReleased,
			FString::Printf(TEXT("Clicked %s at %d,%d"), *Button.ToString(), MouseX, MouseY));
	});

	Lua.set_function("playtest_console", [](const std::string& Command, sol::this_state S) -> sol::table
	{
		FPlaytestInputTarget Target;
		FString Error;
		if (!RequireInputTarget(Target, Error))
		{
			return MakeActionTable(S, false, false, Error);
		}

		if (!Target.World)
		{
			return MakeActionTable(S, false, false, TEXT("No PIE world found for console command."));
		}

		APlayerController* PC = Target.World->GetFirstPlayerController();
		if (!PC)
		{
			return MakeActionTable(S, false, false, TEXT("No PIE player controller found for console command."));
		}

		const FString UECommand = NeoLuaStr::ToFString(Command);
		const FString Output = PC->ConsoleCommand(UECommand, true);

		sol::table Result = MakeActionTable(S, true, true, FString::Printf(TEXT("Executed console command: %s"), *UECommand));
		Result["output"] = TCHAR_TO_UTF8(*Output);
		return Result;
	});

	lua_State* L = Lua.lua_state();
	lua_pushcclosure(L, &PlaytestWaitLuaCFunc, 0);
	lua_setglobal(L, "playtest_wait");

	lua_pushcclosure(L, &PlaytestWaitFramesLuaCFunc, 0);
	lua_setglobal(L, "playtest_wait_frames");

	lua_pushcclosure(L, &PlaytestObserveLuaCFunc, 0);
	lua_setglobal(L, "playtest_observe");

	Lua.script(R"NSAI_PLAYTEST(
function playtest_wait_until(condition, opts)
    opts = opts or {}
    local timeout = math.max(0, math.min(tonumber(opts.timeout or 3.0) or 3.0, 120.0))
    local interval = math.max(0.001, math.min(tonumber(opts.interval or 0.05) or 0.05, 5.0))
    local started = playtest_time()
    local deadline = started + timeout
    local attempts = 0
    local last_value = nil

    local fn = condition
    if type(condition) == "table" then
        local code = condition.lua or condition[1]
        if type(code) ~= "string" then
            return { ok = false, passed = false, message = "playtest_wait_until table condition requires lua string.", attempts = 0 }
        end
        local chunk, err = load("return (" .. code .. ")")
        if not chunk then
            chunk, err = load(code)
        end
        if not chunk then
            return { ok = false, passed = false, message = err or "Could not compile wait condition.", attempts = 0 }
        end
        fn = function()
            return chunk()
        end
    end

    if type(fn) ~= "function" then
        return { ok = false, passed = false, message = "playtest_wait_until requires a function or {lua='...'} condition.", attempts = 0 }
    end

    while playtest_time() <= deadline do
        attempts = attempts + 1
        local ok, value = pcall(fn)
        if not ok then
            return {
                ok = false,
                passed = false,
                message = tostring(value),
                attempts = attempts,
                elapsed = playtest_time() - started,
            }
        end
        last_value = value
        if value then
            return {
                ok = true,
                passed = true,
                message = "Condition passed.",
                attempts = attempts,
                elapsed = playtest_time() - started,
                value = value,
            }
        end
        playtest_wait(interval)
    end

    return {
        ok = false,
        passed = false,
        message = "Timed out waiting for condition.",
        attempts = attempts,
        elapsed = playtest_time() - started,
        value = last_value,
    }
end

function playtest_wait_for_pie(opts)
    opts = opts or {}
    return playtest_wait_until(function()
        local status = playtest_status()
        return status.playing and status.has_pie_world
    end, {
        timeout = opts.timeout or 5.0,
        interval = opts.interval or 0.05,
    })
end

function playtest_assert(name, fn, opts)
    opts = opts or {}
    if type(name) == "function" and fn == nil then
        fn = name
        name = "playtest assertion"
    end
    if type(fn) ~= "function" then
        return { ok = false, passed = false, name = tostring(name), message = "playtest_assert requires a function." }
    end

    local started = playtest_time()
    local ok, value, reason = pcall(fn)
    local result = {
        ok = false,
        passed = false,
        name = tostring(name or "playtest assertion"),
        elapsed = playtest_time() - started,
    }

    if not ok then
        result.message = tostring(value)
        return result
    end

    if type(value) == "table" then
        local passed = value.passed
        if passed == nil then
            passed = value.ok
        end
        result.ok = passed == true
        result.passed = passed == true
        result.message = value.message or (result.passed and "Assertion passed." or "Assertion failed.")
        result.value = value
        return result
    end

    result.ok = value and true or false
    result.passed = result.ok
    result.message = tostring(reason or (result.passed and "Assertion passed." or "Assertion failed."))
    result.value = value
    result.reason = reason
    return result
end

function playtest_assert_log_contains(text, opts)
    opts = opts or {}
    local timeout = opts.timeout or 3.0
    local interval = opts.interval or 0.05
    local found_result = nil
    local waited = playtest_wait_until(function()
        found_result = playtest_log_contains(text, {
            since = opts.since,
            case_sensitive = opts.case_sensitive or false,
        })
        return found_result.ok and found_result.found
    end, { timeout = timeout, interval = interval })

    if waited.passed then
        return {
            ok = true,
            passed = true,
            found = true,
            message = found_result.message,
            line = found_result.line,
            text = found_result.text,
            result = found_result,
            elapsed = waited.elapsed,
        }
    end

    return {
        ok = false,
        passed = false,
        found = false,
        message = "Timed out waiting for log text: " .. tostring(text),
        result = found_result,
        elapsed = waited.elapsed,
    }
end
)NSAI_PLAYTEST");
});
