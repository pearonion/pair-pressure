// Copyright 2026 Betide Studio. All Rights Reserved.

#include "Chat/Streaming/SseLineReader.h"

void FSseLineReader::Feed(const FString& Bytes)
{
	Buffer += Bytes;
}

bool FSseLineReader::NextLine(FString& OutLine)
{
	int32 NewlineIdx = INDEX_NONE;
	if (!Buffer.FindChar(TEXT('\n'), NewlineIdx))
	{
		return false;
	}

	OutLine = Buffer.Left(NewlineIdx);
	Buffer = Buffer.Mid(NewlineIdx + 1);

	// Strip trailing \r left by CRLF endings.
	if (OutLine.EndsWith(TEXT("\r")))
	{
		OutLine.LeftChopInline(1);
	}

	return true;
}

void FSseLineReader::Reset()
{
	Buffer.Empty();
}
