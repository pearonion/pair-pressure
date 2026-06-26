// Copyright 2026 Betide Studio. All Rights Reserved.

#include "Chat/Streaming/HttpStreamHandle.h"

FHttpStreamHandle::FHttpStreamHandle(TSharedRef<IHttpRequest, ESPMode::ThreadSafe> InRequest)
	: Request(InRequest)
	, bActive(true)
{
}

void FHttpStreamHandle::Cancel()
{
	// CancelRequest is safe to call after completion — the HTTP module
	// handles the "already done" case internally.
	if (bActive.Exchange(false))
	{
		Request->CancelRequest();
	}
}

bool FHttpStreamHandle::IsActive() const
{
	return bActive.Load();
}

void FHttpStreamHandle::MarkComplete()
{
	bActive.Store(false);
}
