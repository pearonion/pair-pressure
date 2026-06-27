<script lang="ts">
	import { onMount, onDestroy } from 'svelte';
	import { getEntitlementStatus, type EntitlementStatus } from '$lib/bridge';

	let state: EntitlementStatus | null = $state(null);
	let dismissed = $state(false);
	let pollHandle: ReturnType<typeof setInterval> | null = null;

	async function refresh() {
		try {
			state = await getEntitlementStatus();
			// Stop polling once we have a definitive answer (anything other
			// than "unknown"). Network errors are definitive too — the user
			// needs to see the banner so they know what's happening.
			if (state.status !== 'unknown' && pollHandle) {
				clearInterval(pollHandle);
				pollHandle = null;
			}
		} catch {
			// Bridge not ready yet — keep polling.
		}
	}

	onMount(() => {
		refresh();
		// Entitlement check is fired 0s after StartupModule but the HTTP
		// request takes ~100-500ms; poll every 1s for the first ~12s while
		// the editor warms up.
		let attempts = 0;
		pollHandle = setInterval(() => {
			attempts += 1;
			refresh();
			if (attempts > 12 && pollHandle) {
				clearInterval(pollHandle);
				pollHandle = null;
			}
		}, 1000);
	});

	onDestroy(() => {
		if (pollHandle) clearInterval(pollHandle);
	});

	const visible = $derived(
		!dismissed &&
			state !== null &&
			!state.entitled &&
			state.status !== 'unknown'
	);

	const headline = $derived(
		state?.status === 'network'
			? state.isBinaryBuild
				? 'Subscription verification offline'
				: 'Couldn’t reach NeoStack Cloud'
			: 'NeoStack subscription required'
	);

	const body = $derived(
		state?.status === 'network'
			? state.isBinaryBuild
				? 'This binary build needs to verify your subscription on every launch. Tools are paused while we’re offline.'
				: 'Reconnect to enable plugin updates and the cloud chat provider.'
			: 'Set a NeoStack API key in Settings > Chat & Agents > Chat Providers, or renew your subscription to keep using the plugin.'
	);
</script>

{#if visible}
	<div class="entitlement-banner" role="alert">
		<div class="msg">
			<strong>{headline}</strong>
			<span>{body}</span>
		</div>
		<div class="actions">
			<a
				class="primary"
				href="https://neostack.dev/account"
				target="_blank"
				rel="noopener noreferrer"
			>
				Open billing
			</a>
			{#if state?.status === 'network'}
				<button type="button" onclick={() => refresh()}>Retry</button>
			{/if}
			<button type="button" class="ghost" onclick={() => (dismissed = true)} aria-label="Dismiss">
				&times;
			</button>
		</div>
	</div>
{/if}

<style>
	.entitlement-banner {
		display: flex;
		align-items: center;
		justify-content: space-between;
		gap: 1rem;
		padding: 0.625rem 1rem;
		background: rgba(220, 38, 38, 0.10);
		color: var(--fg-4, inherit);
		border-bottom: 1px solid rgba(220, 38, 38, 0.20);
		font-size: 0.875rem;
	}
	.msg {
		display: flex;
		flex-direction: column;
		min-width: 0;
	}
	.msg strong {
		font-weight: 600;
	}
	.msg span {
		opacity: 0.8;
	}
	.actions {
		display: flex;
		align-items: center;
		gap: 0.5rem;
		flex-shrink: 0;
	}
	.actions a.primary,
	.actions button {
		font-size: 0.8125rem;
		font-weight: 500;
		padding: 0.375rem 0.75rem;
		border-radius: 9999px;
		border: 1px solid rgba(220, 38, 38, 0.40);
		background: transparent;
		color: inherit;
		cursor: pointer;
		text-decoration: none;
	}
	.actions a.primary {
		background: rgb(220, 38, 38);
		color: white;
		border-color: rgb(220, 38, 38);
	}
	.actions button.ghost {
		border-color: transparent;
		opacity: 0.6;
		padding: 0.25rem 0.5rem;
	}
	.actions button.ghost:hover {
		opacity: 1;
	}
</style>
