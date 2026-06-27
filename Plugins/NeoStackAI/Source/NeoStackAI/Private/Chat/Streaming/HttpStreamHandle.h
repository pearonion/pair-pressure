// Copyright 2026 Betide Studio. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Chat/IChatStreamHandle.h"
#include "Interfaces/IHttpRequest.h"

/**
 * Concrete IChatStreamHandle for HTTP-based chat providers.
 *
 * Wraps an IHttpRequest. Cancel() aborts the request via CancelRequest().
 * IsActive() returns true until MarkComplete() is called by the provider
 * (typically from the OnProcessRequestComplete callback after emitting
 * Done or Error). Safe to call Cancel() after completion — it's a no-op.
 *
 * Thread contract: Cancel() may be called from any thread. The atomic
 * active flag is read/written without a lock.
 */
class FHttpStreamHandle : public IChatStreamHandle
{
public:
	explicit FHttpStreamHandle(TSharedRef<IHttpRequest, ESPMode::ThreadSafe> InRequest);

	// IChatStreamHandle
	virtual void Cancel() override;
	virtual bool IsActive() const override;

	/** Called by the provider when the stream has reached a terminal state. */
	void MarkComplete();

private:
	TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request;
	TAtomic<bool> bActive;
};
