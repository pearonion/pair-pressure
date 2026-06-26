// Copyright 2026 Betide Studio. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/**
 * Handle for an in-flight chat completion stream.
 *
 * Returned from IChatProvider::StreamCompletion. The session stores this
 * in CurrentStream for the duration of the Streaming state and calls Cancel()
 * on it if the user aborts. Providers implement Cancel() by aborting their
 * underlying transport (e.g. IHttpRequest::CancelRequest).
 *
 * The handle is managed by TSharedRef — when the session drops its reference
 * and no events are in flight, the provider's implementation can be destroyed.
 * Providers should tolerate Cancel() being called after the stream has already
 * emitted Done or Error (i.e. treat it as a no-op in that case).
 */
class NEOSTACKAI_API IChatStreamHandle
{
public:
	virtual ~IChatStreamHandle() = default;

	/** Abort the in-flight stream. Safe to call multiple times. */
	virtual void Cancel() = 0;

	/** True if the stream has not yet emitted Done or Error. */
	virtual bool IsActive() const = 0;
};
