<script lang="ts">
	import Icon from '$lib/components/Icon.svelte';
	import {
		Cancel01Icon,
		AlertCircleIcon,
		Loading03Icon
	} from '@hugeicons/core-free-icons';
	import {
		cancelDeviceAuth,
		deviceAuthState,
		isActiveStatus,
		startDeviceAuth
	} from '$lib/stores/deviceAuth.js';

	// Singleton — mounted once in +layout.svelte. Every NeoStackSignInButton
	// (and the sidebar NeoStackAccountCard) drives this same modal via the
	// deviceAuthState store, so there's never more than one dialog on screen
	// regardless of how many sign-in entry points exist on the page.

	const state = $derived($deviceAuthState);
	const showModal = $derived(isActiveStatus(state.status) || state.status === 'error');

	function reopenUrl() {
		if (state.verificationUri) {
			window.open(state.verificationUri, '_blank', 'noopener,noreferrer');
		}
	}

	async function handleCancel() {
		await cancelDeviceAuth();
	}

	async function handleRetry() {
		// startDeviceAuth resets the store to `requesting` and runs the
		// bridge call — same path as the original button-side retry.
		await startDeviceAuth();
	}
</script>

{#if showModal}
	<div
		role="dialog"
		aria-modal="true"
		class="fixed inset-0 z-[300] flex items-center justify-center bg-black/40 p-4 backdrop-blur-sm"
	>
		<div class="w-full max-w-md rounded-2xl border border-border bg-card p-6 shadow-xl">
			<div class="flex items-start justify-between gap-3">
				<div class="flex flex-col gap-1">
					<h3 class="text-[15px] font-semibold text-foreground">
						{#if state.status === 'error'}
							Couldn't sign in
						{:else if state.status === 'redeeming'}
							Connecting…
						{:else if state.status === 'waiting' || state.status === 'polling'}
							Waiting for approval
						{:else}
							Opening browser…
						{/if}
					</h3>
					<p class="text-[12.5px] leading-relaxed text-muted-foreground/80">
						{#if state.status === 'error'}
							{state.message || 'Try again, or paste a key manually in Settings.'}
						{:else if state.status === 'waiting' || state.status === 'polling'}
							Approve the request in the browser tab we opened. You can pick which workspace
							the key belongs to there.
						{:else if state.status === 'redeeming'}
							Retrieving your key.
						{:else}
							Hold on a second.
						{/if}
					</p>
				</div>
				<button
					type="button"
					aria-label="Cancel"
					onclick={handleCancel}
					class="rounded-md p-1 text-muted-foreground hover:bg-card/70 hover:text-foreground"
				>
					<Icon icon={Cancel01Icon} size={16} strokeWidth={2} />
				</button>
			</div>

			<div class="mt-5 flex flex-col items-center justify-center gap-3 py-2">
				{#if state.status === 'error'}
					<Icon icon={AlertCircleIcon} size={36} strokeWidth={1.5} class="text-destructive" />
				{:else}
					<Icon icon={Loading03Icon} size={36} strokeWidth={1.5} class="animate-spin text-foreground/60" />
				{/if}

				{#if (state.status === 'waiting' || state.status === 'polling') && state.verificationUri}
					<button
						type="button"
						onclick={reopenUrl}
						class="text-[12px] text-[var(--ue-accent)] underline-offset-2 hover:underline"
					>
						Browser didn't open? Click here.
					</button>
				{/if}
			</div>

			<div class="mt-4 flex justify-end gap-2">
				{#if state.status === 'error'}
					<button
						type="button"
						onclick={handleRetry}
						class="rounded-lg bg-[var(--ue-accent)] px-4 py-1.5 text-[13px] font-medium text-white hover:opacity-90"
					>
						Try again
					</button>
				{:else}
					<button
						type="button"
						onclick={handleCancel}
						class="rounded-lg border border-border bg-card px-4 py-1.5 text-[13px] font-medium text-foreground hover:bg-card/70"
					>
						Cancel
					</button>
				{/if}
			</div>
		</div>
	</div>
{/if}
