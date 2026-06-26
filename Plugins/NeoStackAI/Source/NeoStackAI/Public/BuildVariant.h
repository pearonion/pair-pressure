// Copyright 2025 Betide Studio. All Rights Reserved.

#pragma once

// ────────────────────────────────────────────────────────────────────────
// Single source of truth for build variant.
//
// The release workflow (.github/workflows/release-plugin.yml + the macOS
// companion script) rewrites the literal "0" below to "1" before the
// **binary** compile pass and restores it before the **full** pass. The
// resulting macro value is baked into the compiled binary, so the variant
// answer cannot be flipped post-build by editing the .uplugin or any
// other on-disk asset.
//
// DO NOT modify manually. If you're hand-building from source, leave it
// at 0 — that's the correct value for source/lifetime distributions.
// ────────────────────────────────────────────────────────────────────────

#define NEOSTACK_BUILD_VARIANT_BINARY 0
