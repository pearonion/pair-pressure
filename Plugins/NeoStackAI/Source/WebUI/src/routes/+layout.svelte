<script lang="ts">
	import './layout.css';
	import '@fontsource/geist/400.css';
	import '@fontsource/geist/500.css';
	import '@fontsource/geist/600.css';
	import '@fontsource/geist/700.css';
	import '@fontsource/geist-mono/400.css';
	import '@fontsource/geist-mono/500.css';
	import '@fontsource/inter/400.css';
	import '@fontsource/inter/500.css';
	import '@fontsource/roboto/400.css';
	import '@fontsource/roboto/500.css';
	import '@fontsource/jetbrains-mono/400.css';
	import '@fontsource/jetbrains-mono/500.css';
	import '@fontsource/fira-code/400.css';
	import '@fontsource/fira-code/500.css';
	import '@fontsource/source-code-pro/400.css';
	import '@fontsource/source-code-pro/500.css';
	import '@fontsource/ibm-plex-mono/400.css';
	import '@fontsource/ibm-plex-mono/500.css';
	import '$lib/polyfills.js';
	import { onMount } from 'svelte';
	import { loadAgents } from '$lib/stores/agents.js';
	import { loadSessions, bindSessionListListener } from '$lib/stores/sessions.js';
	import { bindAgentStateListener } from '$lib/stores/agentState.js';
	import { bindMessageListener } from '$lib/stores/messages.js';
	import { bindPermissionListener } from '$lib/stores/permissions.js';
	import { bindModeListener } from '$lib/stores/modes.js';
	import { bindInstallListeners } from '$lib/stores/setup.js';
	import { bindCommandsListener } from '$lib/stores/commands.js';
	import { bindPlanListener } from '$lib/stores/plan.js';
	import { bindModelsListener } from '$lib/stores/models.js';
	import { bindUsageListener } from '$lib/stores/rateLimits.js';
	import { bindCloudAccountListener, refreshCloudAccount } from '$lib/stores/cloudAccount.js';
	import { bindDeviceAuthListener } from '$lib/stores/deviceAuth.js';
	import { bindAttachmentsListener } from '$lib/stores/attachments.js';
	import { bindLoginListener } from '$lib/stores/auth.js';
	import { loadSourceControlStatus } from '$lib/stores/sourceControl.js';
	import { isInUnreal, copyToClipboard, getClipboardText, pasteClipboardImage, waitForBridge } from '$lib/bridge.js';
	import * as Tooltip from '$lib/components/ui/tooltip/index.js';
	import { ModeWatcher } from 'mode-watcher';
	import { Toaster } from 'svelte-sonner';
	import TopNav from '$lib/components/TopNav.svelte';
	import EntitlementBanner from '$lib/components/EntitlementBanner.svelte';
	import DeviceAuthDialog from '$lib/components/DeviceAuthDialog.svelte';
	import {
		themeChoice,
		accentHue,
		accentIntensity,
		reduceTransparency,
		uiFontSize,
		codeFontSize,
		uiFontFamily,
		codeFontFamily,
		fontSmoothing,
		DEFAULT_UI_FONT_STACK,
		DEFAULT_CODE_FONT_STACK,
		normalizeFontStack
	} from '$lib/stores/settings.js';
	import { modKey } from '$lib/platform.js';

	let { children } = $props();

	// ── Appearance: apply user prefs to <html> as CSS variables / classes ──
	// Single source of truth. Components read CSS vars; we just write them.
	$effect(() => {
		if (typeof document === 'undefined') return;
		const root = document.documentElement;

		// Theme class (replaces existing class so we don't accumulate)
		root.classList.remove('theme-dark', 'theme-light', 'theme-high-contrast');
		root.classList.add(`theme-${$themeChoice}`);
		root.classList.toggle('dark', $themeChoice !== 'light');

		// Accent: HSL derived from hue + intensity slider
		// Base saturation 80%, lightness 55% — chosen to match the original #2b8ceb.
		const hue = $accentHue;
		const sat = Math.max(0, Math.min(100, $accentIntensity)) * 0.8;
		root.style.setProperty('--ue-accent', `hsl(${hue} ${sat}% 55%)`);
		root.style.setProperty('--ue-accent-hover', `hsl(${hue} ${sat}% 65%)`);
		root.style.setProperty('--ue-accent-muted', `hsl(${hue} ${sat}% 42%)`);

		// Typography
		root.style.setProperty('--ui-font-size', `${$uiFontSize}px`);
		root.style.setProperty('--code-font-size', `${$codeFontSize}px`);
		root.style.setProperty('--ui-font-family', normalizeFontStack($uiFontFamily, DEFAULT_UI_FONT_STACK));
		root.style.setProperty('--code-font-family', normalizeFontStack($codeFontFamily, DEFAULT_CODE_FONT_STACK));

		// Body classes for toggles that need CSS-side overrides
		document.body.classList.toggle('no-translucency', $reduceTransparency);
		document.body.classList.toggle('font-smoothing-off', !$fontSmoothing);
	});

	onMount(async () => {
		// Wait for UE bridge — CEF starts page load before BindUObject() completes
		await waitForBridge();

		loadAgents();
		loadSessions();
		bindAgentStateListener();
		bindMessageListener();
		bindPermissionListener();
		bindModeListener();
		bindInstallListeners();
		bindCommandsListener();
		bindPlanListener();
		bindModelsListener();
		bindUsageListener();
		bindCloudAccountListener();
		bindDeviceAuthListener();
		refreshCloudAccount();
		bindAttachmentsListener();
		bindLoginListener();
		bindSessionListListener();
		loadSourceControlStatus();
	});

	/**
	 * CEF in off-screen rendering mode doesn't execute default clipboard/selection
	 * actions for keyboard shortcuts. We handle them explicitly via the UE bridge.
	 */
	function handleGlobalKeydown(e: KeyboardEvent) {
		if (!e.metaKey && !e.ctrlKey) return;

		const key = e.key.toLowerCase();
		const el = document.activeElement as HTMLInputElement | HTMLTextAreaElement | null;
		const isInput = el && (el.tagName === 'INPUT' || el.tagName === 'TEXTAREA');

		if (key === 'a') {
			// Select all
			if (isInput) {
				e.preventDefault();
				el.select();
			}
		} else if (key === 'c') {
			// Copy
			const selection = isInput
				? el.value.substring(el.selectionStart ?? 0, el.selectionEnd ?? 0)
				: window.getSelection()?.toString() ?? '';
			if (selection) {
				e.preventDefault();
				copyToClipboard(selection);
			}
		} else if (key === 'x') {
			// Cut
			if (isInput && el.selectionStart !== el.selectionEnd) {
				const start = el.selectionStart ?? 0;
				const end = el.selectionEnd ?? 0;
				const selection = el.value.substring(start, end);
				e.preventDefault();
				copyToClipboard(selection);
				// Delete the selected text
				el.setRangeText('', start, end, 'end');
				el.dispatchEvent(new Event('input', { bubbles: true }));
			}
		} else if (key === 'v') {
			// Paste
			if (isInput) {
				e.preventDefault();
				getClipboardText().then(async (text) => {
					// Text has priority for normal paste. If clipboard text is empty and the
					// focused field is a textarea, try image paste into chat attachments.
					if (text === '' && el.tagName === 'TEXTAREA') {
						await pasteClipboardImage();
						return;
					}

					if (!text) return;
					const start = el.selectionStart ?? 0;
					const end = el.selectionEnd ?? 0;
					el.setRangeText(text, start, end, 'end');
					el.dispatchEvent(new Event('input', { bubbles: true }));
				});
			}
		}
	}

	// ── Global right-click context menu (CEF blocks native menus) ──
	let globalCtxVisible = $state(false);
	let globalCtxX = $state(0);
	let globalCtxY = $state(0);
	let globalCtxHasSelection = $state(false);
	let globalCtxIsInput = $state(false);
	let globalCtxTarget = $state<HTMLInputElement | HTMLTextAreaElement | null>(null);

	function isEditableElement(el: EventTarget | null): el is HTMLInputElement | HTMLTextAreaElement {
		if (!el) return false;
		return (el instanceof HTMLInputElement && el.type !== 'checkbox' && el.type !== 'radio' && el.type !== 'button' && el.type !== 'submit') || el instanceof HTMLTextAreaElement;
	}

	function handleGlobalContextMenu(e: MouseEvent) {
		// If a bits-ui context menu trigger will handle this, let it through
		if ((e.target as HTMLElement)?.closest?.('[data-slot="context-menu-trigger"]')) return;
		e.preventDefault();
		// If the chat pane's own context menu handler will handle this, skip
		if ((e.target as HTMLElement)?.closest?.('.chat-scroll-area, .chat-composer')) return;
		const target = e.target as EventTarget | null;
		globalCtxIsInput = isEditableElement(target);
		globalCtxTarget = globalCtxIsInput ? (target as HTMLInputElement | HTMLTextAreaElement) : null;
		if (globalCtxIsInput && globalCtxTarget) {
			globalCtxHasSelection = (globalCtxTarget.selectionStart ?? 0) !== (globalCtxTarget.selectionEnd ?? 0);
		} else {
			globalCtxHasSelection = !!(window.getSelection()?.toString());
		}
		const vw = window.innerWidth;
		const vh = window.innerHeight;
		globalCtxX = Math.min(e.clientX, vw - 200);
		globalCtxY = Math.min(e.clientY, vh - 160);
		globalCtxVisible = true;
	}

	function handleGlobalCtxCopy() {
		if (globalCtxIsInput && globalCtxTarget) {
			const sel = globalCtxTarget.value.substring(globalCtxTarget.selectionStart ?? 0, globalCtxTarget.selectionEnd ?? 0);
			if (sel) copyToClipboard(sel);
		} else {
			const sel = window.getSelection()?.toString() ?? '';
			if (sel) copyToClipboard(sel);
		}
		globalCtxVisible = false;
	}

	function handleGlobalCtxCut() {
		if (!globalCtxIsInput || !globalCtxTarget) return;
		const s = globalCtxTarget.selectionStart ?? 0;
		const end = globalCtxTarget.selectionEnd ?? 0;
		const sel = globalCtxTarget.value.substring(s, end);
		if (sel) {
			copyToClipboard(sel);
			globalCtxTarget.setRangeText('', s, end, 'end');
			globalCtxTarget.dispatchEvent(new Event('input', { bubbles: true }));
		}
		globalCtxVisible = false;
	}

	async function handleGlobalCtxPaste() {
		if (!globalCtxIsInput || !globalCtxTarget) return;
		const text = await getClipboardText();
		if (text) {
			const s = globalCtxTarget.selectionStart ?? 0;
			const end = globalCtxTarget.selectionEnd ?? 0;
			globalCtxTarget.setRangeText(text, s, end, 'end');
			globalCtxTarget.dispatchEvent(new Event('input', { bubbles: true }));
		}
		globalCtxVisible = false;
	}

	function handleGlobalCtxSelectAll() {
		if (globalCtxIsInput && globalCtxTarget) {
			globalCtxTarget.select();
		} else {
			const sel = window.getSelection();
			const range = document.createRange();
			range.selectNodeContents(document.body);
			sel?.removeAllRanges();
			sel?.addRange(range);
		}
		globalCtxVisible = false;
	}
</script>

<svelte:window onkeydown={handleGlobalKeydown} oncontextmenu={handleGlobalContextMenu} />

<svelte:head>
	<title>Agent Chat</title>
</svelte:head>

<ModeWatcher defaultMode={$themeChoice === 'light' ? 'light' : 'dark'} />
<Toaster theme={$themeChoice === 'light' ? 'light' : 'dark'} position="top-right" richColors />

<Tooltip.Provider delayDuration={0} skipDelayDuration={0}>
	<div class="flex h-screen w-screen flex-col overflow-hidden">
		<TopNav />
		<EntitlementBanner />
		<div class="flex min-h-0 flex-1 overflow-hidden">
			{@render children()}
		</div>
	</div>
	<DeviceAuthDialog />
</Tooltip.Provider>

{#if globalCtxVisible}
	<!-- svelte-ignore a11y_no_static_element_interactions -->
	<!-- svelte-ignore a11y_click_events_have_key_events -->
	<div class="fixed inset-0 z-[200]" onclick={() => globalCtxVisible = false} oncontextmenu={(e) => { e.preventDefault(); globalCtxVisible = false; }}></div>
	<div class="fixed z-[201] min-w-[180px] rounded-lg border border-border bg-popover p-1 shadow-lg" style="left: {globalCtxX}px; top: {globalCtxY}px;">
		{#if globalCtxIsInput}
			<button class="flex w-full items-center justify-between rounded-md px-3 py-1.5 text-[13px] text-popover-foreground {globalCtxHasSelection ? 'hover:bg-accent cursor-default' : 'opacity-40 cursor-not-allowed'}" onclick={handleGlobalCtxCut} disabled={!globalCtxHasSelection}>Cut<span class="ml-auto text-[11px] text-muted-foreground/60">{modKey}X</span></button>
		{/if}
		<button class="flex w-full items-center justify-between rounded-md px-3 py-1.5 text-[13px] text-popover-foreground {globalCtxHasSelection ? 'hover:bg-accent cursor-default' : 'opacity-40 cursor-not-allowed'}" onclick={handleGlobalCtxCopy} disabled={!globalCtxHasSelection}>Copy<span class="ml-auto text-[11px] text-muted-foreground/60">{modKey}C</span></button>
		{#if globalCtxIsInput}
			<button class="flex w-full items-center justify-between rounded-md px-3 py-1.5 text-[13px] text-popover-foreground hover:bg-accent cursor-default" onclick={handleGlobalCtxPaste}>Paste<span class="ml-auto text-[11px] text-muted-foreground/60">{modKey}V</span></button>
		{/if}
		<div class="my-1 h-px bg-border/40"></div>
		<button class="flex w-full items-center justify-between rounded-md px-3 py-1.5 text-[13px] text-popover-foreground hover:bg-accent cursor-default" onclick={handleGlobalCtxSelectAll}>Select All<span class="ml-auto text-[11px] text-muted-foreground/60">{modKey}A</span></button>
	</div>
{/if}
