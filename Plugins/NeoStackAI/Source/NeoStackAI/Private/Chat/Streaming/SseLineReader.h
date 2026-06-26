// Copyright 2026 Betide Studio. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/**
 * Incremental line extractor for SSE-style text streams.
 *
 * SSE chunks arrive piecemeal from HTTP progress callbacks and may split
 * lines at arbitrary byte offsets. This reader buffers incoming bytes,
 * extracts complete lines delimited by \n (stripping an optional trailing
 * \r), and returns them one at a time. Incomplete trailing data stays in
 * the buffer until the next Feed() call.
 *
 * Usage:
 *   FSseLineReader Reader;
 *   Reader.Feed(NewBytes);
 *   FString Line;
 *   while (Reader.NextLine(Line))
 *   {
 *       // Process "data: {...}" or blank line (event boundary).
 *   }
 *
 * Not thread-safe. One reader per in-flight stream.
 */
class FSseLineReader
{
public:
	/** Append incoming bytes to the internal buffer. */
	void Feed(const FString& Bytes);

	/**
	 * Extract the next complete line from the buffer. Returns true and
	 * populates OutLine if a line-terminator was found; returns false if
	 * the buffer has no more complete lines.
	 *
	 * OutLine does not include the trailing newline. A blank line (event
	 * boundary in SSE) returns true with OutLine set to an empty string.
	 */
	bool NextLine(FString& OutLine);

	/** Drop any buffered incomplete line. */
	void Reset();

private:
	FString Buffer;
};
