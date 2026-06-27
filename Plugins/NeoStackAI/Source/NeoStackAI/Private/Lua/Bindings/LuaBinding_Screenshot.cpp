// Copyright 2026 Betide Studio. All Rights Reserved.
//
// `screenshot()` Lua binding — runs FScreenshotTool::Execute asynchronously so
// the editor stays interactive during the warmup poll. Mirrors the generate()
// pattern: AwaitAsync yields the coroutine, the tool fires OnComplete later
// (from the warmup ticker), and the continuation pushes the result string.

#include "Lua/LuaBindingRegistry.h"
#include "Lua/LuaCoroutineAwait.h"
#include "Lua/LuaStr.h"
#include "Lua/NeoLuaState.h"
#include "Tools/ScreenshotViewportTool.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

#define SOL_ALL_SAFETIES_ON 1
#include <sol/sol.hpp>

namespace
{
	void SetOptionalString(TSharedPtr<FJsonObject>& Args, const sol::table& T, const char* Key)
	{
		sol::optional<std::string> Val = T.get<sol::optional<std::string>>(Key);
		if (Val.has_value() && !Val.value().empty())
		{
			Args->SetStringField(UTF8_TO_TCHAR(Key), NeoLuaStr::ToFString(Val.value()));
		}
	}

	void SetOptionalNumber(TSharedPtr<FJsonObject>& Args, const sol::table& T, const char* Key)
	{
		sol::optional<double> Val = T.get<sol::optional<double>>(Key);
		if (Val.has_value())
		{
			Args->SetNumberField(UTF8_TO_TCHAR(Key), Val.value());
		}
	}

	void SetOptionalBool(TSharedPtr<FJsonObject>& Args, const sol::table& T, const char* Key)
	{
		sol::optional<bool> Val = T.get<sol::optional<bool>>(Key);
		if (Val.has_value())
		{
			Args->SetBoolField(UTF8_TO_TCHAR(Key), Val.value());
		}
	}

	void SetOptionalSubObject(TSharedPtr<FJsonObject>& Args, const sol::table& T, const char* Key)
	{
		sol::optional<sol::table> SubTable = T.get<sol::optional<sol::table>>(Key);
		if (SubTable.has_value())
		{
			TSharedPtr<FJsonObject> Sub = MakeShared<FJsonObject>();
			sol::table ST = SubTable.value();

			for (auto& Pair : ST)
			{
				if (Pair.second.is<double>())
				{
					Sub->SetNumberField(
						NeoLuaStr::ToFString(Pair.first.as<std::string>()),
						Pair.second.as<double>());
				}
			}

			Args->SetObjectField(UTF8_TO_TCHAR(Key), Sub);
		}
	}

	// Mirror generate.cpp: pin the runner before touching Session in async callbacks.
	void ScreenshotSessionLog(const TWeakPtr<FNeoLuaState>& WeakRunner, const FString& Msg)
	{
		if (TSharedPtr<FNeoLuaState> Runner = WeakRunner.Pin())
		{
			if (FLuaSessionData* S = Runner->GetSession())
			{
				S->Log(Msg);
			}
		}
	}

	void ScreenshotSessionAddImages(const TWeakPtr<FNeoLuaState>& WeakRunner, const TArray<FToolResultImage>& Images)
	{
		if (TSharedPtr<FNeoLuaState> Runner = WeakRunner.Pin())
		{
			if (FLuaSessionData* S = Runner->GetSession())
			{
				S->AddImages(Images);
			}
		}
	}

	// Raw Lua C function. Registered via lua_pushcclosure so we control the
	// return-count exactly (the AwaitAsync long-jump never returns from this
	// frame — the continuation pushes the result string from the resume side).
	int ScreenshotLuaCFunc(lua_State* L)
	{
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();

		// Args: stack position 1 may be the opts table (optional).
		if (lua_gettop(L) >= 1 && lua_istable(L, 1))
		{
			sol::table T(L, 1);

			SetOptionalString(Args, T, "mode");
			SetOptionalString(Args, T, "asset");
			SetOptionalString(Args, T, "focus_actor");
			SetOptionalString(Args, T, "view_mode");
			SetOptionalString(Args, T, "widget_capture");

			SetOptionalBool(Args, T, "hide_overlays");

			SetOptionalNumber(Args, T, "viewport_index");
			SetOptionalNumber(Args, T, "max_dimension");
			SetOptionalNumber(Args, T, "wait_for_ready_ms");
			SetOptionalNumber(Args, T, "fov");
			SetOptionalNumber(Args, T, "orbit_yaw");
			SetOptionalNumber(Args, T, "orbit_pitch");
			SetOptionalNumber(Args, T, "orbit_distance");

			SetOptionalSubObject(Args, T, "location");
			SetOptionalSubObject(Args, T, "rotation");
		}

		// Heap-allocated tool — needs to outlive this binding call so its async
		// OnComplete can fire after the coroutine has yielded.
		TSharedRef<FScreenshotTool> Tool = MakeShared<FScreenshotTool>();
		TWeakPtr<FNeoLuaState> WeakRunner = FNeoLuaState::FindForState(L);

		return UE::NeoStack::Lua::AwaitAsync(L,
			[Tool, Args, WeakRunner](UE::NeoStack::Lua::FAwaitContinuation Continuation)
			{
				Tool->Execute(Args,
					[Tool, WeakRunner, Continuation = MoveTemp(Continuation)](const FToolResult& Result) mutable
					{
						ScreenshotSessionAddImages(WeakRunner, Result.Images);
						ScreenshotSessionLog(WeakRunner, Result.Output);

						const FString Output = Result.Output;
						Continuation([Output](lua_State* CoroL) -> int
						{
							lua_pushstring(CoroL, TCHAR_TO_UTF8(*Output));
							return 1;
						});
					});
			});
	}
}

static TArray<FLuaFunctionDoc> ScreenshotDocs = {
	{ TEXT("screenshot(opts?)"),
	  TEXT("Capture a screenshot from the editor. Returns image via pipeline. "
	       "opts: {mode='active'|'level'|'asset', asset='/Game/Path', viewport_index=0, "
	       "max_dimension=2048, wait_for_ready_ms=1500, hide_overlays=false, "
	       "location={x,y,z}, rotation={pitch,yaw,roll}, fov=90, "
	       "focus_actor='ActorName', view_mode='lit'|'unlit'|'wireframe', "
	       "orbit_yaw=0, orbit_pitch=0, orbit_distance=0, "
	       "widget_capture='preview'|'designer'}. WidgetBlueprint assets default to clean "
	       "preview capture; use widget_capture='designer' only when editor overlays/rulers/pan are needed."),
	  TEXT("string (description; image returned via pipeline)") }
};

REGISTER_LUA_BINDING(Screenshot, ScreenshotDocs,
[](sol::state& Lua, FLuaSessionData& /*Session*/)
{
	lua_State* L = Lua.lua_state();
	lua_pushcclosure(L, &ScreenshotLuaCFunc, /*nupvalues*/ 0);
	lua_setglobal(L, "screenshot");
});
