// Copyright 2026 Betide Studio. All Rights Reserved.

#include "Skills/NeoStackSkill.h"

#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/SecureHash.h"
#include "NeoStackAIModule.h"

namespace
{

FString TrimQuotes(const FString& In)
{
	FString Trimmed = In;
	Trimmed.TrimStartAndEndInline();
	if (Trimmed.Len() >= 2)
	{
		const TCHAR First = Trimmed[0];
		const TCHAR Last = Trimmed[Trimmed.Len() - 1];
		if ((First == TEXT('"') && Last == TEXT('"')) || (First == TEXT('\'') && Last == TEXT('\'')))
		{
			Trimmed = Trimmed.Mid(1, Trimmed.Len() - 2);
		}
	}
	return Trimmed;
}

// Parse a minimal YAML scalar/list value for the frontmatter keys we support.
// Handles:  `name: foo`   `description: "hello"`   `tags: [a, b]`   `tags:\n  - a\n  - b`
void ApplyFrontmatterField(FNeoStackSkill& Skill, const FString& Key, const FString& Value)
{
	if (Key.Equals(TEXT("name"), ESearchCase::IgnoreCase))
	{
		Skill.Name = TrimQuotes(Value);
	}
	else if (Key.Equals(TEXT("description"), ESearchCase::IgnoreCase))
	{
		Skill.Description = TrimQuotes(Value);
	}
	else if (Key.Equals(TEXT("tags"), ESearchCase::IgnoreCase))
	{
		FString Trimmed = Value;
		Trimmed.TrimStartAndEndInline();
		if (Trimmed.StartsWith(TEXT("[")) && Trimmed.EndsWith(TEXT("]")))
		{
			Trimmed = Trimmed.Mid(1, Trimmed.Len() - 2);
			TArray<FString> Parts;
			Trimmed.ParseIntoArray(Parts, TEXT(","), /*CullEmpty*/ true);
			for (FString& Part : Parts)
			{
				Part = TrimQuotes(Part);
				if (!Part.IsEmpty()) { Skill.Tags.Add(Part); }
			}
		}
	}
}

} // namespace

bool ParseSkillMdFile(const FString& AbsPath, FNeoStackSkill& OutSkill)
{
	OutSkill = FNeoStackSkill{};

	FString Raw;
	if (!FFileHelper::LoadFileToString(Raw, *AbsPath))
	{
		UE_LOG(LogNeoStackAI, Warning, TEXT("Skill: failed to read %s"), *AbsPath);
		return false;
	}

	OutSkill.SkillMdAbsPath = FPaths::ConvertRelativePathToFull(AbsPath);

	{
		const FTCHARToUTF8 Utf8(*Raw);
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
		OutSkill.ShippedDigest = Hex;
	}

	// Split on the first two `---` markers (at column 0) for the frontmatter block.
	// The standard form is:
	//   ---\n
	//   key: value\n
	//   ...
	//   ---\n
	//   <body>
	TArray<FString> Lines;
	Raw.ParseIntoArrayLines(Lines, /*CullEmpty*/ false);

	int32 StartIdx = INDEX_NONE;
	int32 EndIdx = INDEX_NONE;
	for (int32 i = 0; i < Lines.Num(); ++i)
	{
		const FString Line = Lines[i].TrimEnd();
		if (Line == TEXT("---"))
		{
			if (StartIdx == INDEX_NONE) { StartIdx = i; }
			else { EndIdx = i; break; }
		}
		else if (StartIdx == INDEX_NONE && !Line.IsEmpty())
		{
			// Content before any `---` — no frontmatter at all.
			break;
		}
	}

	if (StartIdx == INDEX_NONE || EndIdx == INDEX_NONE)
	{
		UE_LOG(LogNeoStackAI, Warning, TEXT("Skill: %s missing YAML frontmatter"), *AbsPath);
		return false;
	}

	// Parse key:value lines between StartIdx+1 and EndIdx. Multi-line list form
	// (`tags:\n  - a\n  - b`) is captured by folding subsequent `-` lines into the
	// last-seen key.
	FString CurrentKey;
	TArray<FString> CurrentList;
	auto FlushList = [&]()
	{
		if (!CurrentKey.IsEmpty() && CurrentList.Num() > 0)
		{
			FString Joined = TEXT("[") + FString::Join(CurrentList, TEXT(",")) + TEXT("]");
			ApplyFrontmatterField(OutSkill, CurrentKey, Joined);
			CurrentKey.Empty();
			CurrentList.Reset();
		}
	};

	for (int32 i = StartIdx + 1; i < EndIdx; ++i)
	{
		const FString& RawLine = Lines[i];
		FString Line = RawLine;
		Line.TrimEndInline();
		if (Line.IsEmpty()) { FlushList(); continue; }

		// List continuation: indented `- item`
		FString LeftTrimmed = Line;
		LeftTrimmed.TrimStartInline();
		if (LeftTrimmed.StartsWith(TEXT("- ")) && !CurrentKey.IsEmpty())
		{
			CurrentList.Add(LeftTrimmed.Mid(2));
			continue;
		}

		// New key — flush any pending list first
		FlushList();

		int32 ColonIdx = INDEX_NONE;
		if (!Line.FindChar(TEXT(':'), ColonIdx)) { continue; }

		const FString Key = Line.Left(ColonIdx).TrimStartAndEnd();
		const FString Value = Line.Mid(ColonIdx + 1).TrimStartAndEnd();

		if (Value.IsEmpty())
		{
			// Multi-line list follows on subsequent indented lines.
			CurrentKey = Key;
			CurrentList.Reset();
		}
		else
		{
			ApplyFrontmatterField(OutSkill, Key, Value);
		}
	}
	FlushList();

	if (OutSkill.Name.IsEmpty())
	{
		// Fallback to the parent dir name — matches Claude Code's behavior when `name`
		// is omitted from frontmatter.
		OutSkill.Name = FPaths::GetCleanFilename(FPaths::GetPath(OutSkill.SkillMdAbsPath));
	}

	if (OutSkill.Name.IsEmpty())
	{
		UE_LOG(LogNeoStackAI, Warning, TEXT("Skill: %s has no resolvable name"), *AbsPath);
		return false;
	}

	return true;
}
