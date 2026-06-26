<script lang="ts">
	import Icon from '$lib/components/Icon.svelte';
	import { Login01Icon, CheckmarkCircle02Icon } from '@hugeicons/core-free-icons';
	import {
		deviceAuthState,
		isActiveStatus,
		startDeviceAuth
	} from '$lib/stores/deviceAuth.js';

	// Caller props.
	let {
		label = 'Sign in with NeoStack',
		variant = 'primary',
		onsuccess
	}: {
		label?: string;
		variant?: 'primary' | 'secondary';
		onsuccess?: () => void;
	} = $props();

	// Read state from the singleton store. The dialog lives in +layout.svelte;
	// each button only renders its own trigger plus an optional "Connected"
	// pill when the latest flow succeeded.
	const state = $derived($deviceAuthState);

	async function handleClick() {
		const terminal = await startDeviceAuth();
		if (terminal.status === 'success') {
			onsuccess?.();
		}
	}
</script>

<button
	type="button"
	onclick={handleClick}
	disabled={isActiveStatus(state.status)}
	class="group flex items-center justify-center gap-2 rounded-xl px-5 py-2.5 text-[14px] font-medium transition-all disabled:cursor-not-allowed disabled:opacity-50 {variant ===
	'primary'
		? 'bg-[var(--ue-accent)] text-white hover:opacity-90'
		: 'border border-border bg-card text-foreground hover:bg-card/70'}"
>
	<Icon icon={Login01Icon} size={16} strokeWidth={2} />
	{state.status === 'success' ? 'Connected' : label}
</button>

{#if state.status === 'success'}
	<div class="mt-2 flex items-center gap-1.5 text-[12px] text-emerald-500">
		<Icon icon={CheckmarkCircle02Icon} size={14} strokeWidth={2} />
		<span>{state.message || 'Connected to NeoStack.'}</span>
	</div>
{/if}
