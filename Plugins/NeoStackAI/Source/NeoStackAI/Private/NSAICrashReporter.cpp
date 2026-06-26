// Copyright 2026 Betide Studio. All Rights Reserved.

#include "NSAICrashReporter.h"
#include "NSAIAnalytics.h"
#include "ACPSettings.h"
#include "Misc/CoreDelegates.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/PlatformFileManager.h"
#include "GenericPlatform/GenericPlatformCrashContext.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HttpModule.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"
#include "Interfaces/IPluginManager.h"
#include "Framework/Application/SlateApplication.h"
#include "Widgets/SWindow.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Input/SButton.h"
#include "Misc/App.h"
#include "Misc/Compression.h"
#include "Misc/EngineVersion.h"
#include "Serialization/MemoryWriter.h"
#include "PlatformHttp.h"

DEFINE_LOG_CATEGORY_STATIC(LogAIKCrash, Log, All);

// Mirror implementation of the official Unreal Crash reporter (FCompressedHeader / FCompressedCrashFile)
struct FNSAICrashHeader
{
	FString DirectoryName;
	FString FileName;
	int32 UncompressedSize = 0;
	int32 FileCount = 0;

	friend FArchive& operator<<(FArchive& Ar, FNSAICrashHeader& Data)
	{
		uint8 Magic[3] = { 'C', 'R', '1' };
		Ar.Serialize(Magic, sizeof(Magic));
		Data.DirectoryName.SerializeAsANSICharArray(Ar, 260);
		Data.FileName.SerializeAsANSICharArray(Ar, 260);
		Ar << Data.UncompressedSize;
		Ar << Data.FileCount;
		return Ar;
	}
};

struct FNSAICrashFile
{
	int32 Index = 0;
	FString Name;
	TArray<uint8> Bytes;

	friend FArchive& operator<<(FArchive& Ar, FNSAICrashFile& Data)
	{
		Ar << Data.Index;
		Data.Name.SerializeAsANSICharArray(Ar, 260);
		Ar << Data.Bytes;
		return Ar;
	}
};

static bool ZlibCompress(const TArray<uint8>& In, TArray<uint8>& Out)
{
	int32 Size = FCompression::CompressMemoryBound(NAME_Zlib, In.Num());
	Out.SetNumUninitialized(Size);
	if (!FCompression::CompressMemory(NAME_Zlib, Out.GetData(), Size, In.GetData(), In.Num()))
	{
		return false;
	}
	Out.SetNum(Size);
	return true;
}

// Static members
TMap<FString, FString> FNSAICrashReporter::CrashContextData;
FCriticalSection FNSAICrashReporter::ContextLock;

// Max crash history entries to keep
static constexpr int32 MaxCrashHistoryEntries = 30;


FNSAICrashReporter& FNSAICrashReporter::Get()
{
	static FNSAICrashReporter Instance;
	return Instance;
}

// ============================================
// Lifecycle
// ============================================

void FNSAICrashReporter::Initialize()
{
	if (bInitialized) return;
	bInitialized = true;

	// Set persistent crash context so crash dumps include our plugin info
	FGenericCrashContext::SetEngineData(TEXT("NSAI.Loaded"), TEXT("true"));

	// Get plugin version
	TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("NeoStackAI"));
	if (Plugin.IsValid())
	{
		FGenericCrashContext::SetEngineData(TEXT("NSAI.Version"), Plugin->GetDescriptor().VersionName);
	}

	// Register crash delegates
	CrashDelegateHandle = FCoreDelegates::OnHandleSystemError.AddRaw(this, &FNSAICrashReporter::OnCrash);
	EnsureDelegateHandle = FCoreDelegates::OnHandleSystemEnsure.AddRaw(this, &FNSAICrashReporter::OnEnsure);
	ShutdownErrorHandle = FCoreDelegates::OnShutdownAfterError.AddRaw(this, &FNSAICrashReporter::OnShutdownAfterError);

	// Check for crashes from previous session (deferred to allow editor to finish loading)
	FTSTicker::GetCoreTicker().AddTicker(
		FTickerDelegate::CreateLambda([this](float) -> bool
		{
			DetectPreviousCrashes();
			return false; // One-shot
		}),
		3.0f); // 3 second delay after startup

	UE_LOG(LogAIKCrash, Log, TEXT("AIK Crash Reporter initialized"));
}

void FNSAICrashReporter::Shutdown()
{
	if (!bInitialized) return;

	FCoreDelegates::OnHandleSystemError.Remove(CrashDelegateHandle);
	FCoreDelegates::OnHandleSystemEnsure.Remove(EnsureDelegateHandle);
	FCoreDelegates::OnShutdownAfterError.Remove(ShutdownErrorHandle);

	FGenericCrashContext::SetEngineData(TEXT("NSAI.Loaded"), TEXT(""));
	FGenericCrashContext::SetEngineData(TEXT("NSAI.Version"), TEXT(""));
	FGenericCrashContext::SetEngineData(TEXT("NSAI.Status"), TEXT(""));
	FGenericCrashContext::SetEngineData(TEXT("NSAI.CurrentTool"), TEXT(""));

	bInitialized = false;
}

// ============================================
// Crash context (called from tool execution code)
// ============================================

void FNSAICrashReporter::SetCrashContext(const FString& Key, const FString& Value)
{
	{
		FScopeLock Lock(&ContextLock);
		CrashContextData.Add(Key, Value);
	}
	FGenericCrashContext::SetEngineData(*FString::Printf(TEXT("NSAI.%s"), *Key), Value);
}

void FNSAICrashReporter::ClearCrashContext(const FString& Key)
{
	{
		FScopeLock Lock(&ContextLock);
		CrashContextData.Remove(Key);
	}
	FGenericCrashContext::SetEngineData(*FString::Printf(TEXT("NSAI.%s"), *Key), TEXT(""));
}

// ============================================
// Crash delegate callbacks
// ============================================

void FNSAICrashReporter::OnCrash()
{
	// CRASH HANDLER — minimal work, no allocations if possible
	// Write a breadcrumb file so next launch knows AIK was active
	WriteBreadcrumb();
}

void FNSAICrashReporter::OnEnsure()
{
	// Ensures are non-fatal — safe to do normal operations
	// Record as analytics event
	FNSAIAnalytics::Get().RecordEvent(TEXT("ensure_fired"));
}

void FNSAICrashReporter::OnShutdownAfterError()
{
	// Last chance before process dies — breadcrumb should already be written
}

// ============================================
// Breadcrumb (written during crash)
// ============================================

FString FNSAICrashReporter::GetBreadcrumbPath() const
{
	return FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("AIK"), TEXT("crash_breadcrumb.json"));
}

void FNSAICrashReporter::WriteBreadcrumb()
{
	// CRASH HANDLER — keep this minimal and safe
	// Build a simple JSON string manually (no TSharedPtr allocation)
	FString Json = TEXT("{\n");
	Json += FString::Printf(TEXT("  \"timestamp\": \"%s\",\n"), *FDateTime::UtcNow().ToIso8601());

	{
		FScopeLock Lock(&ContextLock);
		for (const auto& Pair : CrashContextData)
		{
			// Escape quotes in values
			FString SafeValue = Pair.Value.Replace(TEXT("\""), TEXT("\\\""));
			Json += FString::Printf(TEXT("  \"%s\": \"%s\",\n"), *Pair.Key, *SafeValue);
		}
	}

	Json += TEXT("  \"aik_active\": true\n");
	Json += TEXT("}\n");

	// Direct file write — FFileHelper should be safe in crash handlers
	FString Path = GetBreadcrumbPath();
	FString Dir = FPaths::GetPath(Path);
	IFileManager::Get().MakeDirectory(*Dir, true);
	FFileHelper::SaveStringToFile(Json, *Path);
}

// ============================================
// Crash history persistence
// ============================================

FString FNSAICrashReporter::GetCrashHistoryPath() const
{
	return FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("AIK"), TEXT("crash_history.json"));
}

TArray<FNSAICrashReporter::FCrashRecord> FNSAICrashReporter::LoadCrashHistory() const
{
	TArray<FCrashRecord> Records;
	FString JsonString;
	if (!FFileHelper::LoadFileToString(JsonString, *GetCrashHistoryPath()))
	{
		return Records;
	}

	TSharedPtr<FJsonValue> Parsed;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonString);
	if (!FJsonSerializer::Deserialize(Reader, Parsed) || !Parsed.IsValid())
	{
		return Records;
	}

	const TArray<TSharedPtr<FJsonValue>>* ArrayPtr;
	if (!Parsed->TryGetArray(ArrayPtr)) return Records;

	for (const TSharedPtr<FJsonValue>& Item : *ArrayPtr)
	{
		const TSharedPtr<FJsonObject>* ObjPtr;
		if (!Item->TryGetObject(ObjPtr)) continue;
		const TSharedPtr<FJsonObject>& Obj = *ObjPtr;

		FCrashRecord R;
		R.CrashId = Obj->GetStringField(TEXT("crashId"));
		R.Timestamp = Obj->GetStringField(TEXT("timestamp"));
		R.ErrorMessage = Obj->GetStringField(TEXT("errorMessage"));
		R.CrashType = Obj->GetStringField(TEXT("crashType"));
		R.CallstackSummary = Obj->GetStringField(TEXT("callstackSummary"));
		R.bBasicReported = Obj->GetBoolField(TEXT("basicReported"));
		R.bFullLogSent = Obj->GetBoolField(TEXT("fullLogSent"));
		R.bFullLogDeclined = Obj->GetBoolField(TEXT("fullLogDeclined"));
		R.bManuallyReported = Obj->GetBoolField(TEXT("manuallyReported"));
		Records.Add(MoveTemp(R));
	}

	return Records;
}

void FNSAICrashReporter::SaveCrashHistory(const TArray<FCrashRecord>& Records) const
{
	TArray<TSharedPtr<FJsonValue>> Array;
	for (const FCrashRecord& R : Records)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("crashId"), R.CrashId);
		Obj->SetStringField(TEXT("timestamp"), R.Timestamp);
		Obj->SetStringField(TEXT("errorMessage"), R.ErrorMessage);
		Obj->SetStringField(TEXT("crashType"), R.CrashType);
		Obj->SetStringField(TEXT("callstackSummary"), R.CallstackSummary);
		Obj->SetBoolField(TEXT("basicReported"), R.bBasicReported);
		Obj->SetBoolField(TEXT("fullLogSent"), R.bFullLogSent);
		Obj->SetBoolField(TEXT("fullLogDeclined"), R.bFullLogDeclined);
		Obj->SetBoolField(TEXT("manuallyReported"), R.bManuallyReported);
		Array.Add(MakeShared<FJsonValueObject>(Obj));
	}

	FString JsonString;
	TSharedRef<TJsonWriter<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>> Writer =
		TJsonWriterFactory<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>::Create(&JsonString);

	FJsonSerializer::Serialize(Array, Writer);

	FString Path = GetCrashHistoryPath();
	FString Dir = FPaths::GetPath(Path);
	IFileManager::Get().MakeDirectory(*Dir, true);
	FFileHelper::SaveStringToFile(JsonString, *Path);
}

FString FNSAICrashReporter::GetCrashHistoryJson() const
{
	FString JsonString;
	if (FFileHelper::LoadFileToString(JsonString, *GetCrashHistoryPath()))
	{
		return JsonString;
	}
	return TEXT("[]");
}

// ============================================
// Crash detection on startup
// ============================================

void FNSAICrashReporter::DetectPreviousCrashes()
{
	// Check for breadcrumb first
	FString BreadcrumbPath = GetBreadcrumbPath();
	bool bBreadcrumbExists = FPaths::FileExists(BreadcrumbPath);

	// Scan crash directories — location differs by platform
	// Windows: <Project>/Saved/Crashes/
	// Mac: ~/Library/Application Support/Epic/UnrealEngine/<Version>/Saved/Crashes/
	TArray<FString> CrashDirs;
	CrashDirs.Add(FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Crashes")));

#if PLATFORM_MAC
	// Mac stores crashes in ~/Library/Application Support/Epic/UnrealEngine/<Version>/Saved/Crashes/
	CrashDirs.Add(FPaths::Combine(FPaths::EngineUserDir(), TEXT("Saved"), TEXT("Crashes")));
#endif

	bool bAnyCrashDirExists = false;
	for (const FString& Dir : CrashDirs)
	{
		if (FPaths::DirectoryExists(Dir)) bAnyCrashDirExists = true;
	}
	if (!bAnyCrashDirExists && !bBreadcrumbExists)
	{
		return;
	}

	TArray<FCrashRecord> History = LoadCrashHistory();

	// Build set of known crash IDs
	TSet<FString> KnownCrashIds;
	for (const FCrashRecord& R : History)
	{
		KnownCrashIds.Add(R.CrashId);
	}

	// Find crash folders from all directories
	TArray<TPair<FString, FString>> CrashFoldersWithPaths; // FolderName -> FullPath
	for (const FString& CrashDir : CrashDirs)
	{
		if (!FPaths::DirectoryExists(CrashDir)) continue;
		TArray<FString> Folders;
		IFileManager::Get().FindFiles(Folders, *(CrashDir / TEXT("*")), false, true);
		for (const FString& F : Folders)
		{
			CrashFoldersWithPaths.Add(TPair<FString, FString>(F, CrashDir / F));
		}
	}

	// Collect new crashes that need prompting
	TArray<FPendingCrash> PendingPrompts;

	const UACPSettings* Settings = UACPSettings::Get();

	for (const TPair<FString, FString>& Entry : CrashFoldersWithPaths)
	{
		const FString& FolderName = Entry.Key;
		const FString& CrashFolderPath = Entry.Value;
		if (KnownCrashIds.Contains(FolderName)) continue;

		// Skip ensure reports — only prompt for actual crashes
		if (FolderName.StartsWith(TEXT("EnsureReport"))) continue;

		FString XmlPath = CrashFolderPath / TEXT("CrashContext.runtime-xml");
		if (!FPaths::FileExists(XmlPath)) continue;

		FString ErrorMessage, CrashType, Callstack;
		bool bAIKInCallstack = false;

		if (!ParseCrashContextXml(XmlPath, ErrorMessage, CrashType, Callstack, bAIKInCallstack))
		{
			continue;
		}

		// Only track crashes where AIK is in the callstack OR breadcrumb was written
		if (!bAIKInCallstack && !bBreadcrumbExists)
		{
			continue;
		}

		FCrashRecord Record;
		Record.CrashId = FolderName;
		Record.Timestamp = FDateTime::UtcNow().ToIso8601();
		Record.ErrorMessage = ErrorMessage.Left(500);
		Record.CrashType = CrashType;
		Record.CallstackSummary = Callstack.Left(1000);

		// Auto-send basic report if enabled
		if (Settings && Settings->bEnableAnalytics && Settings->bEnableCrashReporting)
		{
			SendBasicCrashReport(Record);
			Record.bBasicReported = true;
		}

		// Full crash handling
		if (Settings && Settings->bAlwaysSendCrashLogs)
		{
			SendFullCrashReport(Record);
			Record.bFullLogSent = true;
			History.Add(MoveTemp(Record));
		}
		else if (Settings && Settings->bEnableCrashReporting)
		{
			// Queue for batched prompt
			FPendingCrash Pending;
			Pending.Record = Record;
			Pending.FolderPath = CrashFolderPath;
			PendingPrompts.Add(MoveTemp(Pending));
			History.Add(Record);
		}
		else
		{
			History.Add(MoveTemp(Record));
		}
	}

	// Show ONE batched prompt for all pending crashes
	if (PendingPrompts.Num() > 0)
	{
		SaveCrashHistory(History);

		// Capture data for the lambda
		TArray<FPendingCrash> CapturedPending = MoveTemp(PendingPrompts);
		FTSTicker::GetCoreTicker().AddTicker(
			FTickerDelegate::CreateLambda([this, CapturedPending](float) -> bool
			{
				ShowBatchCrashPrompt(CapturedPending);
				return false;
			}),
			1.0f);
	}

	// Trim history to max entries
	while (History.Num() > MaxCrashHistoryEntries)
	{
		History.RemoveAt(0);
	}

	SaveCrashHistory(History);

	// Clean up breadcrumb
	if (bBreadcrumbExists)
	{
		IFileManager::Get().Delete(*BreadcrumbPath);
	}
}

// ============================================
// XML parsing
// ============================================

bool FNSAICrashReporter::ParseCrashContextXml(const FString& XmlPath, FString& OutErrorMessage,
	FString& OutCrashType, FString& OutCallstack, bool& bOutAIKInCallstack)
{
	FString XmlContent;
	if (!FFileHelper::LoadFileToString(XmlContent, *XmlPath))
	{
		return false;
	}

	// Simple tag extraction (crash context XML is well-formed, no need for full parser)
	auto ExtractTag = [&XmlContent](const FString& TagName) -> FString
	{
		FString OpenTag = FString::Printf(TEXT("<%s>"), *TagName);
		FString CloseTag = FString::Printf(TEXT("</%s>"), *TagName);
		int32 Start = XmlContent.Find(OpenTag);
		if (Start == INDEX_NONE) return FString();
		Start += OpenTag.Len();
		int32 End = XmlContent.Find(CloseTag, ESearchCase::CaseSensitive, ESearchDir::FromStart, Start);
		if (End == INDEX_NONE) return FString();
		return XmlContent.Mid(Start, End - Start).TrimStartAndEnd();
	};

	OutErrorMessage = ExtractTag(TEXT("ErrorMessage"));
	OutCrashType = ExtractTag(TEXT("CrashType"));
	OutCallstack = ExtractTag(TEXT("PCallStack"));

	// Check if our module appears in the portable callstack
	bOutAIKInCallstack = OutCallstack.Contains(TEXT("NeoStackAI"));

	// Also check EngineData for our breadcrumb context
	if (!bOutAIKInCallstack)
	{
		FString EngineData = ExtractTag(TEXT("EngineData"));
		if (EngineData.Contains(TEXT("NSAI.Loaded")))
		{
			bOutAIKInCallstack = true;
		}
	}

	return !OutErrorMessage.IsEmpty() || !OutCrashType.IsEmpty();
}

// ============================================
// Crash artifact discovery
// ============================================
FString FNSAICrashReporter::FindCrashFolderForId(const FString& CrashId) const
{
	TArray<FString> CrashDirs;
	CrashDirs.Add(FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Crashes")));
#if PLATFORM_MAC
	CrashDirs.Add(FPaths::Combine(FPaths::EngineUserDir(), TEXT("Saved"), TEXT("Crashes")));
#endif
	for (const FString& Dir : CrashDirs)
	{
		FString Candidate = Dir / CrashId;
		if (FPaths::DirectoryExists(Candidate))
		{
			return Candidate;
		}
	}
	return FString();
}

// ============================================
// Reporting
// ============================================

void FNSAICrashReporter::SendBasicCrashReport(const FCrashRecord& Record)
{
	TSharedPtr<FJsonObject> Props = MakeShared<FJsonObject>();
	Props->SetStringField(TEXT("crash_type"), Record.CrashType);
	Props->SetStringField(TEXT("error_message"), FNSAIAnalytics::SanitizeErrorForAnalytics(Record.ErrorMessage));
	Props->SetStringField(TEXT("callstack_summary"), FNSAIAnalytics::SanitizeErrorForAnalytics(Record.CallstackSummary));
	Props->SetStringField(TEXT("crash_id"), Record.CrashId);

	FNSAIAnalytics::Get().RecordEvent(TEXT("engine_crash"), Props);

	UE_LOG(LogAIKCrash, Log, TEXT("Basic crash report sent for %s"), *Record.CrashId);
}

void FNSAICrashReporter::SendFullCrashReport(const FCrashRecord& Record)
{
	const FString CrashId = Record.CrashId;

	const UACPSettings* Settings = UACPSettings::Get();
	if (!Settings || Settings->CrashReportUrl.IsEmpty())
	{
		UE_LOG(LogAIKCrash, Warning, TEXT("CrashReportUrl not configured; skipping crash upload for %s"), *CrashId);
		return;
	}

	const FString CrashFolder = FindCrashFolderForId(CrashId);
	if (CrashFolder.IsEmpty())
	{
		UE_LOG(LogAIKCrash, Warning, TEXT("Crash folder not found for %s"), *CrashId);
		return;
	}

	TArray<FString> FilesOnDisk;
	const FString CrashFilesRegex = CrashFolder / TEXT("*");
	IFileManager::Get().FindFiles(FilesOnDisk, *CrashFilesRegex, true, false);

	TArray<FNSAICrashFile> Files;
	for (const FString& FileName : FilesOnDisk)
	{
		const FString FilePath = CrashFolder / FileName;
		FNSAICrashFile File;
		File.Index = Files.Num();
		File.Name = FileName;
		if (FFileHelper::LoadFileToArray(File.Bytes, *FilePath))
		{
			Files.Add(MoveTemp(File));
		}
	}

	if (Files.IsEmpty())
	{
		UE_LOG(LogAIKCrash, Warning, TEXT("No files in crash folder for %s"), *CrashId);
		return;
	}

	TArray<uint8> PayloadBytes;

	FNSAICrashHeader Header;
	Header.DirectoryName = CrashId;
	Header.FileName = CrashId + TEXT(".uecrash");

	FMemoryWriter Writer(PayloadBytes);
	Writer << Header;
	for (FNSAICrashFile& File : Files)
	{
		Writer << File;
	}

	// Rewind and overwrite the placeholder header with final UncompressedSize/FileCount (only known once everything is written).
	// This is the same way the Unreal Crash Reporter does it
	Header.UncompressedSize = PayloadBytes.Num();
	Header.FileCount = Files.Num();
	Writer.Seek(0);
	Writer << Header;

	TArray<uint8> CompressBytes;
	if (!ZlibCompress(PayloadBytes, CompressBytes))
	{
		UE_LOG(LogAIKCrash, Warning, TEXT("Zlib compression failed for crash %s"), *CrashId);
		return;
	}

	FString RequestUrl = Settings->CrashReportUrl;

	// Sentry's unreal endpoint accept an optional UserID so we use it to sneak our install id.
	if (!FNSAIAnalytics::Get().GetInstallId().IsEmpty())
	{
		RequestUrl += FString::Printf(TEXT("?UserID=%s"), *FGenericPlatformHttp::UrlEncode(FNSAIAnalytics::Get().GetInstallId()));
	}

	UE_LOG(LogAIKCrash, Log, TEXT("Uploading crash %s (%d bytes, %d files)"), *CrashId, CompressBytes.Num(), Files.Num());

	TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = FHttpModule::Get().CreateRequest();
	Request->SetURL(RequestUrl);
	Request->SetVerb(TEXT("POST"));
	Request->SetHeader(TEXT("Content-Type"), TEXT("application/octet-stream"));
	Request->SetContent(CompressBytes);

	Request->OnProcessRequestComplete().BindLambda(
		[CrashId](FHttpRequestPtr, FHttpResponsePtr Resp, bool bConnected)
		{
			const int32 Code = Resp.IsValid() ? Resp->GetResponseCode() : 0;
			const FString Content = Resp.IsValid() ? Resp->GetContentAsString() : FString();
			UE_LOG(LogAIKCrash, Log, TEXT("Crash %s upload response: code=%d body=%s"), *CrashId, Code, *Content);
		});

	Request->ProcessRequest();
}

// ============================================
// Manual report from Web UI
// ============================================

bool FNSAICrashReporter::ManuallyReportCrash(const FString& CrashId)
{
	TArray<FCrashRecord> History = LoadCrashHistory();
	FCrashRecord* Found = History.FindByPredicate([&](const FCrashRecord& R) { return R.CrashId == CrashId; });
	if (!Found)
	{
		return false;
	}

	SendFullCrashReport(*Found);
	Found->bFullLogSent = true;
	Found->bManuallyReported = true;
	Found->bFullLogDeclined = false;
	SaveCrashHistory(History);
	return true;
}

// ============================================
// Batched crash prompt dialog
// ============================================

void FNSAICrashReporter::ShowBatchCrashPrompt(const TArray<FPendingCrash>& PendingCrashes)
{
	if (!FSlateApplication::IsInitialized() || PendingCrashes.Num() == 0) return;

	// Collect crash IDs and find log files
	TArray<TPair<FString, FCrashRecord>> CrashIdsAndReports; // CrashId -> CrashRecord
	for (const FPendingCrash& P : PendingCrashes)
	{
		CrashIdsAndReports.Add(TPair<FString, FCrashRecord>(P.Record.CrashId, P.Record));
	}

	if (CrashIdsAndReports.Num() == 0) return;

	// Build summary text
	FString TitleText = CrashIdsAndReports.Num() == 1
		? TEXT("NeoStack AI detected a crash from your last session.")
		: FString::Printf(TEXT("NeoStack AI detected %d crashes from previous sessions."), CrashIdsAndReports.Num());

	// Show the most recent error as preview
	FString ErrorPreview = PendingCrashes.Last().Record.ErrorMessage.Left(200);

	TSharedRef<SWindow> PromptWindow = SNew(SWindow)
		.Title(FText::FromString(TEXT("NeoStack AI Crash Report")))
		.ClientSize(FVector2D(520, 280))
		.SupportsMaximize(false)
		.SupportsMinimize(false)
		.IsTopmostWindow(true)
		.SizingRule(ESizingRule::FixedSize);

	TWeakPtr<SWindow> WeakWindow = PromptWindow;

	PromptWindow->SetContent(
		SNew(SBox)
		.Padding(FMargin(20))
		[
			SNew(SVerticalBox)

			// Title
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0, 0, 0, 12)
			[
				SNew(STextBlock)
				.Text(FText::FromString(TitleText))
				.Font(FCoreStyle::GetDefaultFontStyle("Bold", 11))
				.AutoWrapText(true)
			]

			// Error preview
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0, 0, 0, 16)
			[
				SNew(SBox)
				.Padding(FMargin(8))
				[
					SNew(STextBlock)
					.Text(FText::FromString(ErrorPreview))
					.Font(FCoreStyle::GetDefaultFontStyle("Mono", 9))
					.AutoWrapText(true)
					.ColorAndOpacity(FSlateColor(FLinearColor(1.0f, 0.4f, 0.4f)))
				]
			]

			// Description
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0, 0, 0, 20)
			[
				SNew(STextBlock)
				.Text(FText::FromString(
					CrashIdsAndReports.Num() == 1
						? TEXT("Sending the full editor log helps us diagnose and fix this issue faster.")
						: FString::Printf(TEXT("Send full editor logs for all %d crashes? This helps us diagnose and fix issues faster."), CrashIdsAndReports.Num())))
				.AutoWrapText(true)
				.ColorAndOpacity(FSlateColor(FLinearColor(0.7f, 0.7f, 0.7f)))
			]

			// Buttons
			+ SVerticalBox::Slot()
			.AutoHeight()
			.HAlign(HAlign_Right)
			[
				SNew(SHorizontalBox)

				// Don't Send
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.Padding(0, 0, 8, 0)
				[
					SNew(SButton)
					.Text(FText::FromString(TEXT("Don't Send")))
					.OnClicked_Lambda([this, CrashIdsAndReports, WeakWindow]() -> FReply
					{
						TArray<FCrashRecord> H = LoadCrashHistory();
						for (FCrashRecord& R : H)
						{
							for (const auto& Pair : CrashIdsAndReports)
							{
								if (R.CrashId == Pair.Key)
								{
									R.bFullLogDeclined = true;
								}
							}
						}
						SaveCrashHistory(H);

						if (TSharedPtr<SWindow> Win = WeakWindow.Pin())
						{
							Win->RequestDestroyWindow();
						}
						return FReply::Handled();
					})
				]

				// Always Send
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.Padding(0, 0, 8, 0)
				[
					SNew(SButton)
					.Text(FText::FromString(TEXT("Always Send")))
					.OnClicked_Lambda([this, CrashIdsAndReports, WeakWindow]() -> FReply
					{
						UACPSettings* S = GetMutableDefault<UACPSettings>();
						if (S)
						{
							S->bAlwaysSendCrashLogs = true;
							S->SaveConfig();
						}

						TArray<FCrashRecord> H = LoadCrashHistory();
						for (const auto& Pair : CrashIdsAndReports)
						{
							SendFullCrashReport(Pair.Value);
							for (FCrashRecord& R : H)
							{
								if (R.CrashId == Pair.Key)
								{
									R.bFullLogSent = true;
								}
							}
						}
						SaveCrashHistory(H);

						if (TSharedPtr<SWindow> Win = WeakWindow.Pin())
						{
							Win->RequestDestroyWindow();
						}
						return FReply::Handled();
					})
				]

				// Send
				+ SHorizontalBox::Slot()
				.AutoWidth()
				[
					SNew(SButton)
					.Text(FText::FromString(
						CrashIdsAndReports.Num() == 1
							? TEXT("Send Crash")
							: FString::Printf(TEXT("Send All (%d)"), CrashIdsAndReports.Num())))
					.ButtonStyle(&FAppStyle::Get().GetWidgetStyle<FButtonStyle>("PrimaryButton"))
					.OnClicked_Lambda([this, CrashIdsAndReports, WeakWindow]() -> FReply
					{
						TArray<FCrashRecord> H = LoadCrashHistory();
						for (const auto& Pair : CrashIdsAndReports)
						{
							SendFullCrashReport(Pair.Value);
							for (FCrashRecord& R : H)
							{
								if (R.CrashId == Pair.Key)
								{
									R.bFullLogSent = true;
								}
							}
						}
						SaveCrashHistory(H);

						if (TSharedPtr<SWindow> Win = WeakWindow.Pin())
						{
							Win->RequestDestroyWindow();
						}
						return FReply::Handled();
					})
				]
			]
		]
	);

	FSlateApplication::Get().AddWindow(PromptWindow);
}
