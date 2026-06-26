<script lang="ts">
	import Icon from '$lib/components/Icon.svelte';
	import {
		UserIcon,
		Login01Icon,
		AlertCircleIcon,
		ReloadIcon
	} from '@hugeicons/core-free-icons';
	import {
		cloudAccount,
		cloudAccountLoadState,
		cloudAccountError,
		formatTierLabel,
		userInitials,
		maskEmail,
		refreshCloudAccount
	} from '$lib/stores/cloudAccount.js';
	import { openSettings, hideEmail } from '$lib/stores/settings.js';
	import { clearNeoStackCloudKey } from '$lib/bridge.js';
	import { startDeviceAuth } from '$lib/stores/deviceAuth.js';

	let {
		variant = 'sidebar'
	}: {
		variant?: 'sidebar' | 'settings';
	} = $props();

	const isCompact = $derived(variant === 'sidebar');
	const account = $derived($cloudAccount);
	const connection = $derived(account?.connectionState ?? 'disconnected');
	const tierLabel = $derived(formatTierLabel(account?.status));
	// Prefer the entitlement tier (Lifetime / Subscription / Studio Seat / combined)
	// over the org's access-plan name — a lifetime owner whose org is on the
	// "Free" plan should still read as "Lifetime" in the sidebar.
	const planLabel = $derived(
		account?.status && account.status !== 'none'
			? tierLabel
			: account?.accessPlan?.planName || tierLabel || 'Free'
	);
	const visibleEmail = $derived(
		account?.user?.email ? ($hideEmail ? maskEmail(account.user.email) : account.user.email) : ''
	);
	const displayName = $derived(account?.user?.name || visibleEmail || 'NeoStack account');
	const workspaceName = $derived(account?.organization?.name || account?.organization?.slug || '');
	const isConnected = $derived(
		connection === 'connected' ||
			(connection === 'offline' && (account?.user?.name || account?.user?.email))
	);

	async function handleSignIn() {
		// Route through the shared store so the singleton dialog opens with
		// proper "Opening browser → Waiting → Connecting → Connected" feedback,
		// then refresh the account card once the flow lands a key.
		const terminal = await startDeviceAuth();
		if (terminal.status === 'success') {
			await refreshCloudAccount();
		}
	}

	async function handleSignOut() {
		await clearNeoStackCloudKey();
		await refreshCloudAccount();
	}
</script>

{#if isConnected}
	{#if isCompact}
		<button
			type="button"
			class="flex w-full items-center gap-2 rounded-md px-1.5 py-1.5 text-left transition-colors hover:bg-sidebar-accent/40"
			title="Open account settings"
			onclick={() => openSettings('agents')}
		>
			{#if account?.user?.image}
				<img
					src={account.user.image}
					alt=""
					class="h-7 w-7 shrink-0 rounded-full object-cover"
				/>
			{:else}
				<div
					class="flex h-7 w-7 shrink-0 items-center justify-center rounded-full bg-[var(--ue-accent)]/15 text-[10px] font-semibold text-[var(--ue-accent)]"
				>
					{userInitials(account?.user?.name, account?.user?.email)}
				</div>
			{/if}
			<div class="min-w-0 flex-1 leading-tight">
				<p class="truncate text-[12px] font-medium text-foreground">{displayName}</p>
				<p class="truncate text-[10.5px] text-muted-foreground/60">{planLabel}</p>
			</div>
			{#if connection === 'offline'}
				<span
					class="shrink-0 rounded-full bg-amber-500/15 px-1.5 py-0.5 text-[9px] font-medium text-amber-400"
					title="Offline — showing last known account"
				>offline</span>
			{/if}
		</button>
	{:else}
		<div class="mb-4 rounded-lg border border-border/60 bg-card/40 p-4">
			<div class="flex items-start gap-3">
				{#if account?.user?.image}
					<img
						src={account.user.image}
						alt=""
						class="h-10 w-10 shrink-0 rounded-full object-cover"
					/>
				{:else}
					<div
						class="flex h-10 w-10 shrink-0 items-center justify-center rounded-full bg-[var(--ue-accent)]/15 text-[12px] font-semibold text-[var(--ue-accent)]"
					>
						{userInitials(account?.user?.name, account?.user?.email)}
					</div>
				{/if}
				<div class="min-w-0 flex-1">
					<div class="flex items-start justify-between gap-2">
						<div class="min-w-0">
							<div class="flex items-center gap-1.5">
								<p class="truncate text-[14px] font-medium text-foreground">{displayName}</p>
								{#if connection === 'offline'}
									<span
										class="shrink-0 rounded-full bg-amber-500/15 px-1.5 py-0.5 text-[9px] font-medium text-amber-400"
										title="Offline — showing last known account"
									>offline</span>
								{/if}
							</div>
							{#if visibleEmail && visibleEmail !== displayName}
								<p class="truncate text-[11.5px] text-muted-foreground/55">{visibleEmail}</p>
							{/if}
							{#if workspaceName}
								<p class="truncate text-[11.5px] text-muted-foreground/60">{workspaceName}</p>
							{/if}
						</div>
						<button
							type="button"
							class="shrink-0 rounded p-1 text-muted-foreground/40 transition-colors hover:bg-accent hover:text-foreground"
							title="Refresh account"
							onclick={() => refreshCloudAccount()}
						>
							<Icon icon={ReloadIcon} size={14} strokeWidth={1.5} />
						</button>
					</div>
					<div class="mt-1.5 flex flex-wrap items-center gap-1.5">
						<span
							class="rounded-full bg-[var(--ue-accent)]/10 px-2 py-0.5 text-[10.5px] font-medium text-[var(--ue-accent)]"
						>{tierLabel}</span>
						{#if account?.accessPlan?.planName && account.accessPlan.planName !== tierLabel}
							<span class="text-[10.5px] text-muted-foreground/55">{account.accessPlan.planName}</span>
						{/if}
					</div>
					<div class="mt-3 flex items-center gap-3 text-[11px]">
						<button
							type="button"
							class="text-muted-foreground/60 underline-offset-2 hover:text-foreground hover:underline"
							onclick={() => openSettings('usage')}
						>View usage</button>
						<button
							type="button"
							class="text-muted-foreground/50 underline-offset-2 hover:text-foreground hover:underline"
							onclick={handleSignOut}
						>Sign out</button>
					</div>
				</div>
			</div>
		</div>
	{/if}
{:else if connection === 'loading' || $cloudAccountLoadState === 'loading'}
	{#if isCompact}
		<div class="flex items-center gap-2 px-1.5 py-1.5 text-[11.5px] text-muted-foreground/60">
			<div class="h-3 w-3 animate-spin rounded-full border-2 border-muted-foreground/20 border-t-muted-foreground/70"></div>
			<span>Checking NeoStack…</span>
		</div>
	{:else}
		<div class="mb-4 flex items-center gap-2 rounded-lg border border-border/60 bg-card/40 px-4 py-3 text-[12px] text-muted-foreground/60">
			<div class="h-3.5 w-3.5 animate-spin rounded-full border-2 border-muted-foreground/20 border-t-muted-foreground/70"></div>
			<span>Checking NeoStack Cloud…</span>
		</div>
	{/if}
{:else}
	{#if isCompact}
		<button
			type="button"
			class="flex w-full items-center gap-2 rounded-md px-1.5 py-1.5 text-left transition-colors hover:bg-sidebar-accent/40"
			onclick={handleSignIn}
		>
			<div
				class="flex h-7 w-7 shrink-0 items-center justify-center rounded-full bg-muted-foreground/10 text-muted-foreground/60"
			>
				<Icon icon={UserIcon} size={14} strokeWidth={1.5} />
			</div>
			<div class="min-w-0 flex-1 leading-tight">
				<p class="truncate text-[12px] font-medium text-foreground">
					{#if connection === 'key_rejected'}
						Sign in again
					{:else if account?.isBinaryBuild}
						Sign in with NeoStack
					{:else}
						NeoStack Cloud
					{/if}
				</p>
				<p class="truncate text-[10.5px] text-muted-foreground/55">
					{#if connection === 'key_rejected'}
						Key was rejected
					{:else if connection === 'offline'}
						Offline — tap to retry
					{:else}
						Not signed in
					{/if}
				</p>
			</div>
		</button>
	{:else}
		<div class="mb-4 flex flex-col gap-2 rounded-lg border border-border/60 bg-card/40 p-4">
			<div class="flex items-start gap-2.5">
				<Icon icon={UserIcon} size={20} strokeWidth={1.5} class="mt-0.5 shrink-0 text-muted-foreground/45" />
				<div class="min-w-0">
					<p class="text-[13px] font-medium text-foreground">
						{#if connection === 'key_rejected'}
							Connection issue
						{:else if connection === 'offline'}
							Can't reach NeoStack Cloud
						{:else if account?.isBinaryBuild}
							Sign in with NeoStack
						{:else}
							NeoStack Cloud
						{/if}
					</p>
					<p class="mt-0.5 text-[11.5px] leading-relaxed text-muted-foreground/60">
						{#if connection === 'key_rejected'}
							Your saved key was rejected. Sign in again to refresh it.
						{:else if connection === 'offline'}
							Check your connection.
						{:else if account?.isBinaryBuild}
							Already bought NeoStack on Fab? Sign in so we can verify your lifetime access and enable cloud features.
						{:else}
							Sign in to track your plan and credits (optional for source builds).
						{/if}
					</p>
					{#if $cloudAccountError}
						<p class="mt-1 flex items-center gap-1 text-[11px] text-amber-400/80">
							<Icon icon={AlertCircleIcon} size={12} strokeWidth={1.5} />
							{$cloudAccountError}
						</p>
					{/if}
				</div>
			</div>
			<div class="flex flex-col gap-1.5">
				<button
					type="button"
					class="flex w-full items-center justify-center gap-1.5 rounded-md px-3 py-1.5 text-[12px] font-medium transition-colors {account?.isBinaryBuild
						? 'bg-amber-500/15 text-amber-200 hover:bg-amber-500/25'
						: 'bg-[var(--ue-accent)] text-white hover:opacity-90'}"
					onclick={handleSignIn}
				>
					<Icon icon={Login01Icon} size={14} strokeWidth={2} />
					Sign in with NeoStack
				</button>
				{#if connection === 'offline' || account?.clientStatus === 'network'}
					<button
						type="button"
						class="text-[11px] text-muted-foreground/55 underline-offset-2 hover:text-foreground hover:underline"
						onclick={() => refreshCloudAccount()}
					>Retry</button>
				{/if}
			</div>
		</div>
	{/if}
{/if}
