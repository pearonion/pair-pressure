// Copyright 2026 Betide Studio. All Rights Reserved.

#include "Skills/NeoStackSkillInstaller.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/SecureHash.h"
#include "NeoStackAIModule.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Skills/NeoStackSkill.h"
#include "Skills/NeoStackSkillRegistry.h"

namespace
{

struct FTargetDir
{
	FString Key;       // "claude" | "agents" — identifier in the manifest.
	FString AbsRoot;   // <ProjectDir>/.claude/skills (or .agents/skills)
};

struct FManifestEntry
{
	FString SourceId;
	FString SourceVersion;
	FString ShippedDigest;   // digest of the source SKILL.md the last time we ran.
	FString WrittenDigest;   // digest of whatever we wrote to both targets last time.
	bool    bConflictPending = false;
};

struct FManifest
{
	TMap<FString /*skillName*/, FManifestEntry> Skills;
};

// ────────────────────────────────────────────────────────────────────────────
// Paths
// ────────────────────────────────────────────────────────────────────────────

FString GetProjectDir()
{
	return FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
}

FString GetManifestPathAbs()
{
	return FPaths::Combine(GetProjectDir(), TEXT(".neostack"), TEXT("skills-manifest.json"));
}

TArray<FTargetDir> GetTargets()
{
	TArray<FTargetDir> Targets;
	Targets.Add({ TEXT("claude"), FPaths::Combine(GetProjectDir(), TEXT(".claude"), TEXT("skills")) });
	Targets.Add({ TEXT("agents"), FPaths::Combine(GetProjectDir(), TEXT(".agents"), TEXT("skills")) });
	return Targets;
}

FString SkillFileInTarget(const FTargetDir& Target, const FString& SkillName)
{
	return FPaths::Combine(Target.AbsRoot, SkillName, TEXT("SKILL.md"));
}

FString SkillNewFileInTarget(const FTargetDir& Target, const FString& SkillName)
{
	return FPaths::Combine(Target.AbsRoot, SkillName, TEXT("SKILL.new.md"));
}

// ────────────────────────────────────────────────────────────────────────────
// File I/O helpers
// ────────────────────────────────────────────────────────────────────────────

bool FileExistsAbs(const FString& Path)
{
	return FPlatformFileManager::Get().GetPlatformFile().FileExists(*Path);
}

FString DigestForBytes(const FString& Contents)
{
	const FTCHARToUTF8 Utf8(*Contents);
	FSHA1 Hasher;
	Hasher.Update(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
	Hasher.Final();
	uint8 Digest[FSHA1::DigestSize];
	Hasher.GetHash(Digest);
	FString Hex;
	Hex.Reserve(FSHA1::DigestSize * 2);
	for (int32 i = 0; i < FSHA1::DigestSize; ++i)
	{
		Hex += FString::Printf(TEXT("%02x"), Digest[i]);
	}
	return Hex;
}

// Returns empty string if the file does not exist.
FString DigestForFile(const FString& Path)
{
	if (!FileExistsAbs(Path)) { return FString(); }
	FString Contents;
	if (!FFileHelper::LoadFileToString(Contents, *Path)) { return FString(); }
	return DigestForBytes(Contents);
}

bool EnsureDirectoryExists(const FString& Dir)
{
	IFileManager& Files = IFileManager::Get();
	if (Files.DirectoryExists(*Dir)) { return true; }
	return Files.MakeDirectory(*Dir, /*Tree*/ true);
}

bool WriteFileAtomic(const FString& Path, const FString& Contents)
{
	if (!EnsureDirectoryExists(FPaths::GetPath(Path)))
	{
		return false;
	}
	return FFileHelper::SaveStringToFile(
		Contents, *Path,
		FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
}

bool DeleteFileIfExists(const FString& Path)
{
	if (!FileExistsAbs(Path)) { return true; }
	return IFileManager::Get().Delete(*Path, /*RequireExists*/ false, /*EvenReadOnly*/ true);
}

/** Remove the directory that immediately contains `SKILL.md` if it's now empty.
 *  Skips silently if the parent is non-empty (user-authored scripts/, examples/, etc.). */
void TryRemoveEmptyParentDir(const FString& SkillMdPath)
{
	const FString ParentDir = FPaths::GetPath(SkillMdPath);
	TArray<FString> Files;
	TArray<FString> Dirs;
	IFileManager::Get().FindFiles(Files, *(ParentDir / TEXT("*")), true, false);
	IFileManager::Get().FindFiles(Dirs, *(ParentDir / TEXT("*")), false, true);
	if (Files.Num() == 0 && Dirs.Num() == 0)
	{
		IFileManager::Get().DeleteDirectory(*ParentDir, false, false);
	}
}

// ────────────────────────────────────────────────────────────────────────────
// Manifest (de)serialization
// ────────────────────────────────────────────────────────────────────────────

FManifest LoadManifest()
{
	FManifest Out;
	const FString Path = GetManifestPathAbs();
	FString Raw;
	if (!FFileHelper::LoadFileToString(Raw, *Path))
	{
		return Out;
	}

	TSharedPtr<FJsonObject> Root;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Raw);
	if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
	{
		UE_LOG(LogNeoStackAI, Warning, TEXT("SkillInstaller: manifest %s was unreadable, discarding"), *Path);
		return Out;
	}

	const TSharedPtr<FJsonObject>* SkillsObj = nullptr;
	if (!Root->TryGetObjectField(TEXT("skills"), SkillsObj) || !SkillsObj->IsValid())
	{
		return Out;
	}

	for (const auto& Pair : (*SkillsObj)->Values)
	{
		const TSharedPtr<FJsonObject>* Entry = nullptr;
		if (!Pair.Value->TryGetObject(Entry) || !Entry->IsValid()) { continue; }

		FManifestEntry E;
		(*Entry)->TryGetStringField(TEXT("sourceId"),       E.SourceId);
		(*Entry)->TryGetStringField(TEXT("sourceVersion"),  E.SourceVersion);
		(*Entry)->TryGetStringField(TEXT("shippedDigest"),  E.ShippedDigest);
		(*Entry)->TryGetStringField(TEXT("writtenDigest"),  E.WrittenDigest);
		(*Entry)->TryGetBoolField  (TEXT("conflictPending"), E.bConflictPending);
		Out.Skills.Add(FString(*Pair.Key), E);
	}

	return Out;
}

bool SaveManifest(const FManifest& Manifest)
{
	TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetNumberField(TEXT("version"), 1);

	TSharedRef<FJsonObject> Skills = MakeShared<FJsonObject>();
	for (const TPair<FString, FManifestEntry>& Pair : Manifest.Skills)
	{
		TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("sourceId"),        Pair.Value.SourceId);
		Entry->SetStringField(TEXT("sourceVersion"),   Pair.Value.SourceVersion);
		Entry->SetStringField(TEXT("shippedDigest"),   Pair.Value.ShippedDigest);
		Entry->SetStringField(TEXT("writtenDigest"),   Pair.Value.WrittenDigest);
		Entry->SetBoolField  (TEXT("conflictPending"), Pair.Value.bConflictPending);
		Skills->SetObjectField(Pair.Key, Entry);
	}
	Root->SetObjectField(TEXT("skills"), Skills);

	FString Out;
	const TSharedRef<TJsonWriter<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>> Writer =
		TJsonWriterFactory<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>::Create(&Out);
	FJsonSerializer::Serialize(Root, Writer);

	return WriteFileAtomic(GetManifestPathAbs(), Out);
}

// ────────────────────────────────────────────────────────────────────────────
// Sync: one source skill × one target dir
// ────────────────────────────────────────────────────────────────────────────

enum class ETargetOutcome : uint8 { Installed, Updated, NoOp, UserKept, Conflict, Error };

ETargetOutcome ReconcileOneTarget(
	const FNeoStackSkill& Source,
	const FString& SourceBody,
	const FManifestEntry* PriorManifest,
	const FTargetDir& Target,
	FNeoStackSkillSyncReport& Report)
{
	const FString SkillPath   = SkillFileInTarget(Target, Source.Name);
	const FString NewPath     = SkillNewFileInTarget(Target, Source.Name);
	const FString ExistingDig = DigestForFile(SkillPath);
	const bool    bHasFile    = !ExistingDig.IsEmpty();

	const bool bUpstreamChanged = !PriorManifest
		|| !PriorManifest->ShippedDigest.Equals(Source.ShippedDigest, ESearchCase::IgnoreCase);

	// First-time install (no existing file).
	if (!bHasFile)
	{
		if (!WriteFileAtomic(SkillPath, SourceBody))
		{
			Report.Errors.Add(FString::Printf(TEXT("write failed: %s"), *SkillPath));
			return ETargetOutcome::Error;
		}
		DeleteFileIfExists(NewPath);
		return ETargetOutcome::Installed;
	}

	const bool bClean = PriorManifest
		&& !PriorManifest->WrittenDigest.IsEmpty()
		&& ExistingDig.Equals(PriorManifest->WrittenDigest, ESearchCase::IgnoreCase);

	if (bClean)
	{
		if (!bUpstreamChanged)
		{
			DeleteFileIfExists(NewPath);  // defensive — shouldn't exist
			return ETargetOutcome::NoOp;
		}
		if (!WriteFileAtomic(SkillPath, SourceBody))
		{
			Report.Errors.Add(FString::Printf(TEXT("write failed: %s"), *SkillPath));
			return ETargetOutcome::Error;
		}
		DeleteFileIfExists(NewPath);
		return ETargetOutcome::Updated;
	}

	// File differs from what we wrote → user edited (or this is a pre-existing file
	// we've never seen, e.g. user authored a skill with the same name by hand).
	if (!bUpstreamChanged)
	{
		DeleteFileIfExists(NewPath);
		return ETargetOutcome::UserKept;
	}

	// Upstream changed AND user edited → drop SKILL.new.md beside.
	if (!WriteFileAtomic(NewPath, SourceBody))
	{
		Report.Errors.Add(FString::Printf(TEXT("write failed: %s"), *NewPath));
		return ETargetOutcome::Error;
	}
	return ETargetOutcome::Conflict;
}

/** Read the shipped source body for a skill from its absolute path. */
bool ReadSourceBody(const FNeoStackSkill& Skill, FString& OutBody)
{
	if (!FFileHelper::LoadFileToString(OutBody, *Skill.SkillMdAbsPath))
	{
		UE_LOG(LogNeoStackAI, Warning, TEXT("SkillInstaller: could not read source %s"), *Skill.SkillMdAbsPath);
		return false;
	}
	return true;
}

// ────────────────────────────────────────────────────────────────────────────
// Orphan cleanup for a single skill
// ────────────────────────────────────────────────────────────────────────────

void CleanOrphan(
	const FString& SkillName,
	const FManifestEntry& Prior,
	FNeoStackSkillSyncReport& Report)
{
	for (const FTargetDir& Target : GetTargets())
	{
		const FString SkillPath = SkillFileInTarget(Target, SkillName);
		const FString NewPath   = SkillNewFileInTarget(Target, SkillName);
		const FString Dig       = DigestForFile(SkillPath);

		DeleteFileIfExists(NewPath);  // .new.md is always ours to drop

		if (Dig.IsEmpty())
		{
			// Already gone — nothing to do.
			continue;
		}
		const bool bClean = !Prior.WrittenDigest.IsEmpty()
			&& Dig.Equals(Prior.WrittenDigest, ESearchCase::IgnoreCase);

		if (bClean)
		{
			DeleteFileIfExists(SkillPath);
			TryRemoveEmptyParentDir(SkillPath);
			Report.OrphansRemoved++;
		}
		else
		{
			Report.OrphansKept++;
		}
	}
}

} // namespace

// ════════════════════════════════════════════════════════════════════════════
// Public API
// ════════════════════════════════════════════════════════════════════════════

FString FNeoStackSkillInstaller::GetManifestPath()
{
	return GetManifestPathAbs();
}

FNeoStackSkillSyncReport FNeoStackSkillInstaller::SyncProject()
{
	FNeoStackSkillSyncReport Report;

	const TArray<FNeoStackSkill> Sources = FNeoStackSkillRegistry::Get().GetAll();
	FManifest Manifest = LoadManifest();
	FManifest Next;

	TSet<FString> SeenSourceNames;

	for (const FNeoStackSkill& Source : Sources)
	{
		SeenSourceNames.Add(Source.Name);

		FString Body;
		if (!ReadSourceBody(Source, Body))
		{
			Report.Errors.Add(FString::Printf(TEXT("read source failed: %s"), *Source.SkillMdAbsPath));
			continue;
		}

		const FManifestEntry* Prior = Manifest.Skills.Find(Source.Name);

		bool bAnyConflict = false;
		bool bAnyWrite = false;
		for (const FTargetDir& Target : GetTargets())
		{
			const ETargetOutcome Outcome = ReconcileOneTarget(Source, Body, Prior, Target, Report);
			switch (Outcome)
			{
				case ETargetOutcome::Installed: Report.Installed++;     bAnyWrite = true; break;
				case ETargetOutcome::Updated:   Report.Updated++;       bAnyWrite = true; break;
				case ETargetOutcome::NoOp:      Report.NoOp++;                            break;
				case ETargetOutcome::UserKept:  Report.UserEditsKept++;                   break;
				case ETargetOutcome::Conflict:  Report.Conflicts++; bAnyConflict = true;  break;
				case ETargetOutcome::Error:     /* already counted in Report.Errors */    break;
			}
		}

		// `WrittenDigest` = "what we last wrote to SKILL.md". Advance only when we actually
		// wrote the new source to at least one target this run; otherwise the prior value
		// keeps drift detection meaningful on targets the user hasn't touched.
		FManifestEntry NewEntry;
		NewEntry.SourceId         = Source.SourceId;
		NewEntry.SourceVersion    = Source.SourceVersion;
		NewEntry.ShippedDigest    = Source.ShippedDigest;
		NewEntry.WrittenDigest    = bAnyWrite
			? Source.ShippedDigest
			: (Prior ? Prior->WrittenDigest : FString());
		NewEntry.bConflictPending = bAnyConflict;
		Next.Skills.Add(Source.Name, NewEntry);
	}

	// Orphans: manifest entries whose source plugin is no longer present.
	for (const TPair<FString, FManifestEntry>& Pair : Manifest.Skills)
	{
		if (SeenSourceNames.Contains(Pair.Key)) { continue; }
		CleanOrphan(Pair.Key, Pair.Value, Report);
	}

	if (!SaveManifest(Next))
	{
		Report.Errors.Add(FString::Printf(TEXT("manifest write failed: %s"), *GetManifestPathAbs()));
	}

	UE_LOG(LogNeoStackAI, Log,
		TEXT("SkillInstaller: sync done — installed=%d updated=%d noop=%d userKept=%d conflicts=%d orphansRemoved=%d orphansKept=%d errors=%d"),
		Report.Installed, Report.Updated, Report.NoOp, Report.UserEditsKept,
		Report.Conflicts, Report.OrphansRemoved, Report.OrphansKept, Report.Errors.Num());

	return Report;
}

TArray<FNeoStackSkillStatus> FNeoStackSkillInstaller::GetStatus()
{
	TArray<FNeoStackSkillStatus> Out;
	const TArray<FNeoStackSkill> Sources = FNeoStackSkillRegistry::Get().GetAll();
	const FManifest Manifest = LoadManifest();
	const TArray<FTargetDir> Targets = GetTargets();

	for (const FNeoStackSkill& S : Sources)
	{
		FNeoStackSkillStatus Status;
		Status.Name = S.Name;
		Status.SourceId = S.SourceId;
		Status.SourceDisplayName = S.SourceDisplayName;
		Status.SourceVersion = S.SourceVersion;
		Status.Description = S.Description;
		Status.Tags = S.Tags;

		const FManifestEntry* Prior = Manifest.Skills.Find(S.Name);

		for (const FTargetDir& Target : Targets)
		{
			const FString Path = SkillFileInTarget(Target, S.Name);
			if (!FileExistsAbs(Path)) { continue; }
			Status.InstalledPaths.Add(Path);

			const FString Dig = DigestForFile(Path);
			if (Prior && !Prior->WrittenDigest.IsEmpty()
				&& !Dig.Equals(Prior->WrittenDigest, ESearchCase::IgnoreCase))
			{
				Status.bUserEdited = true;
			}

			const FString NewPath = SkillNewFileInTarget(Target, S.Name);
			if (FileExistsAbs(NewPath))
			{
				Status.ConflictNewPath = NewPath;
				Status.bConflictPending = true;
			}
		}
		Out.Add(MoveTemp(Status));
	}

	return Out;
}

TArray<FNeoStackProjectSkillStatus> FNeoStackSkillInstaller::GetProjectSkills()
{
	const FManifest Manifest = LoadManifest();
	TMap<FString, FNeoStackProjectSkillStatus> Seen;

	// Prefer .agents metadata when the same skill exists in both target dirs.
	TArray<FTargetDir> Targets = GetTargets();
	Targets.Sort([](const FTargetDir& A, const FTargetDir& B)
	{
		if (A.Key == B.Key) { return false; }
		if (A.Key == TEXT("agents")) { return true; }
		if (B.Key == TEXT("agents")) { return false; }
		return A.Key < B.Key;
	});

	IFileManager& Files = IFileManager::Get();
	IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();

	for (const FTargetDir& Target : Targets)
	{
		if (!Files.DirectoryExists(*Target.AbsRoot))
		{
			continue;
		}

		TArray<FString> SkillDirs;
		Files.FindFiles(SkillDirs, *(Target.AbsRoot / TEXT("*")), /*Files*/ false, /*Directories*/ true);

		for (const FString& DirName : SkillDirs)
		{
			const FString SkillPath = SkillFileInTarget(Target, DirName);
			if (!PlatformFile.FileExists(*SkillPath))
			{
				continue;
			}

			FString ResolvedName = DirName;
			FString Description;
			TArray<FString> Tags;
			FString ParseError;

			FNeoStackSkill Parsed;
			if (ParseSkillMdFile(SkillPath, Parsed))
			{
				if (!Parsed.Name.IsEmpty())
				{
					ResolvedName = Parsed.Name;
				}
				Description = Parsed.Description;
				Tags = Parsed.Tags;
			}
			else
			{
				ParseError = TEXT("SKILL.md has invalid or missing frontmatter");
			}

			if (Manifest.Skills.Contains(ResolvedName))
			{
				continue;
			}

			FNeoStackProjectSkillStatus* Existing = Seen.Find(ResolvedName);
			if (Existing)
			{
				Existing->Paths.AddUnique(SkillPath);
				if (Existing->Description.IsEmpty() && !Description.IsEmpty())
				{
					Existing->Description = Description;
					Existing->Tags = Tags;
					Existing->ParseError.Empty();
				}
				else if (Existing->ParseError.IsEmpty() && !ParseError.IsEmpty())
				{
					Existing->ParseError = ParseError;
				}
			}
			else
			{
				FNeoStackProjectSkillStatus Status;
				Status.Name = ResolvedName;
				Status.FolderName = DirName;
				Status.Description = Description;
				Status.Tags = Tags;
				Status.Paths.Add(SkillPath);
				Status.ParseError = ParseError;
				Seen.Add(ResolvedName, MoveTemp(Status));
			}
		}
	}

	TArray<FNeoStackProjectSkillStatus> Out;
	Seen.GenerateValueArray(Out);
	Out.Sort([](const FNeoStackProjectSkillStatus& A, const FNeoStackProjectSkillStatus& B)
	{
		return A.Name.Compare(B.Name, ESearchCase::IgnoreCase) < 0;
	});
	return Out;
}

FString FNeoStackSkillInstaller::ReadSkillBody(const FString& SkillName)
{
	for (const FTargetDir& Target : GetTargets())
	{
		const FString Path = SkillFileInTarget(Target, SkillName);
		if (FileExistsAbs(Path))
		{
			FString Body;
			if (FFileHelper::LoadFileToString(Body, *Path)) { return Body; }
		}
	}
	FNeoStackSkill Source;
	if (FNeoStackSkillRegistry::Get().FindByName(SkillName, Source))
	{
		FString Body;
		if (FFileHelper::LoadFileToString(Body, *Source.SkillMdAbsPath)) { return Body; }
	}
	return FString();
}

FString FNeoStackSkillInstaller::ReadConflictBody(const FString& SkillName)
{
	for (const FTargetDir& Target : GetTargets())
	{
		const FString Path = SkillNewFileInTarget(Target, SkillName);
		if (FileExistsAbs(Path))
		{
			FString Body;
			if (FFileHelper::LoadFileToString(Body, *Path)) { return Body; }
		}
	}
	return FString();
}

bool FNeoStackSkillInstaller::ResolveConflict(const FString& SkillName, const FString& Mode)
{
	FManifest Manifest = LoadManifest();
	FManifestEntry* Entry = Manifest.Skills.Find(SkillName);
	if (!Entry || !Entry->bConflictPending)
	{
		return false;
	}

	FString NewBody;
	bool bHaveNew = false;
	for (const FTargetDir& Target : GetTargets())
	{
		const FString Path = SkillNewFileInTarget(Target, SkillName);
		if (FileExistsAbs(Path) && !bHaveNew)
		{
			bHaveNew = FFileHelper::LoadFileToString(NewBody, *Path);
		}
	}

	const bool bTakeNew = Mode.Equals(TEXT("take-new"), ESearchCase::IgnoreCase);

	for (const FTargetDir& Target : GetTargets())
	{
		const FString SkillPath = SkillFileInTarget(Target, SkillName);
		const FString NewPath   = SkillNewFileInTarget(Target, SkillName);

		if (bTakeNew && bHaveNew)
		{
			WriteFileAtomic(SkillPath, NewBody);
		}
		DeleteFileIfExists(NewPath);
	}

	// In both modes we re-anchor to the new shipped digest so future sync runs start
	// from a clean baseline (either: user's edits are now the user's forever-accepted
	// version; or: user took the new one and it's identical to shipped).
	if (bTakeNew && bHaveNew)
	{
		Entry->WrittenDigest = DigestForBytes(NewBody);
	}
	else
	{
		// keep-user → treat current installed file as our new written baseline.
		FString FirstInstalled;
		for (const FTargetDir& Target : GetTargets())
		{
			const FString Path = SkillFileInTarget(Target, SkillName);
			if (FileExistsAbs(Path) && FFileHelper::LoadFileToString(FirstInstalled, *Path))
			{
				break;
			}
		}
		if (!FirstInstalled.IsEmpty())
		{
			Entry->WrittenDigest = DigestForBytes(FirstInstalled);
		}
	}
	Entry->bConflictPending = false;
	SaveManifest(Manifest);
	return true;
}
