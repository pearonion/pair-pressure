<script lang="ts">
	import Icon from '$lib/components/Icon.svelte';
	import ExtensionCard, {
		type ExtensionCardStatus
	} from '$lib/components/ExtensionCard.svelte';
	import CustomSelect from '$lib/components/ui/custom-select/CustomSelect.svelte';
	import {
		Add01Icon,
		AlertCircleIcon,
		ArrowReloadHorizontalIcon,
		ArrowUp01Icon,
		Cancel01Icon,
		CloudDownloadIcon,
		Delete01Icon,
		PlayIcon
	} from '@hugeicons/core-free-icons';
	import {
		applyExtensionOps,
		fetchExtensionCatalog,
		getExtensionOpState,
		getExtensionSettings,
		queueExtensionOp,
		restartEditor,
		setExtensionEnabled,
		setExtensionPluginsEnabled,
		type CatalogExtension,
		type ExtensionCatalogChannel,
		type ExtensionCatalogState,
		type ExtensionDependency,
		type ExtensionInfo,
		type ExtensionOp,
		type ExtensionOpKind,
		type ExtensionOpPhase,
		type ExtensionOpRunState,
		type ExtensionSettingsState
	} from '$lib/bridge.js';

	type PendingOp = {
		slug: string;
		pluginName: string;
		kind: ExtensionOpKind;
		channel: string;
		name: string;
	};

	type DepReport = {
		required: ExtensionDependency[];
		optional: ExtensionDependency[];
		requiredMissing: ExtensionDependency[];
		requiredNotInstalled: ExtensionDependency[];
	};

	type CardAction = {
		label: string;
		tone?: 'neutral' | 'primary' | 'danger' | 'warning';
		icon?: readonly (readonly [string, Readonly<Record<string, string | number>>])[];
		disabled?: boolean;
		title?: string;
		onclick: () => void;
	};

	type ExtensionGroup<T> = {
		key: string;
		label: string;
		sortOrder: number;
		enabled: number;
		total: number;
		items: T[];
	};

	let settings = $state<ExtensionSettingsState>({
		coreApiVersion: 0,
		projectFile: '',
		restartRequired: false,
		extensions: []
	});
	let catalog = $state<ExtensionCatalogState>({
		status: 'idle',
		success: false,
		httpStatus: 0,
		channel: '',
		engine: '',
		extensions: []
	});

	let pendingOps = $state<Map<string, PendingOp>>(new Map());
	let runState = $state<ExtensionOpRunState>({
		running: false,
		restartRecommended: false,
		succeeded: 0,
		failed: 0,
		ops: []
	});
	let pollHandle: ReturnType<typeof setInterval> | null = null;

	let isLoading = $state(false);
	let isLoadingCatalog = $state(false);
	let hasLoadedOnce = $state(false);
	let busyPluginName = $state('');
	let actionError = $state('');
	let searchQuery = $state('');

	let channel = $state<ExtensionCatalogChannel>('stable');
	let includeAllEngines = $state(false);

	let installedByPluginName = $derived<Record<string, ExtensionInfo>>(
		Object.fromEntries(settings.extensions.map((ext) => [ext.pluginName, ext]))
	);
	let remoteByPluginName = $derived<Record<string, CatalogExtension>>(
		Object.fromEntries(catalog.extensions.map((ext) => [ext.pluginName, ext]))
	);

	let filteredInstalled = $derived(settings.extensions.filter(matchesInstalled));
	let availableFromCloud = $derived<CatalogExtension[]>(
		catalog.extensions.filter(
			(remote) =>
				remote.latestVersion &&
				!installedByPluginName[remote.pluginName] &&
				matchesRemote(remote)
		)
	);
	let installedGroups = $derived(groupInstalled(filteredInstalled));
	let availableGroups = $derived(groupRemote(availableFromCloud));
	let enabledCount = $derived(settings.extensions.filter((ext) => ext.enabledInProject).length);
	let activeCount = $derived(settings.extensions.filter((ext) => ext.activeInSession).length);
	let pendingList = $derived<PendingOp[]>(Array.from(pendingOps.values()));

	export async function load() {
		if (isLoading) return;
		isLoading = true;
		actionError = '';
		try {
			await Promise.all([
				getExtensionSettings()
					.then((result) => {
						settings = result;
					})
					.catch((error) => {
						console.warn('Failed to load extension settings:', error);
						actionError = 'Failed to load extensions.';
					}),
				loadCatalog()
			]);
		} finally {
			isLoading = false;
			hasLoadedOnce = true;
		}
		runState = await getExtensionOpState();
	}

	async function loadCatalog() {
		if (isLoadingCatalog) return;
		isLoadingCatalog = true;
		try {
			catalog = await fetchExtensionCatalog(channel, includeAllEngines);
		} catch (error) {
			console.warn('Failed to load extension catalog:', error);
			catalog = {
				status: 'error',
				success: false,
				httpStatus: 0,
				channel,
				engine: '',
				error: error instanceof Error ? error.message : String(error),
				extensions: []
			};
		} finally {
			isLoadingCatalog = false;
		}
	}

	async function refresh() {
		await load();
	}

	async function onChannelChange(next: ExtensionCatalogChannel) {
		channel = next;
		await loadCatalog();
	}

	async function onIncludeAllEnginesChange(next: boolean) {
		includeAllEngines = next;
		await loadCatalog();
	}

	function pendingKey(slug: string, kind: ExtensionOpKind): string {
		return `${slug}:${kind}`;
	}

	function isQueued(slug: string, kind: ExtensionOpKind): boolean {
		return pendingOps.has(pendingKey(slug, kind));
	}

	function hasUpdate(local: ExtensionInfo): { remote: CatalogExtension } | null {
		const remote = remoteByPluginName[local.pluginName];
		if (!remote || !remote.latestVersion || !local.version) return null;
		if (remote.latestVersion === local.version) return null;
		return { remote };
	}

	function remoteSlugFor(pluginName: string): string {
		return remoteByPluginName[pluginName]?.slug ?? '';
	}

	async function toggleExtension(extension: ExtensionInfo) {
		if (!extension.canToggle || busyPluginName) return;
		busyPluginName = extension.pluginName;
		actionError = '';
		try {
			const result = await setExtensionEnabled(extension.pluginName, !extension.enabledInProject);
			if (!result.success) {
				actionError = result.error || `Failed to update ${extension.displayName}.`;
				return;
			}
			await load();
		} catch (error) {
			console.warn('Failed to toggle extension:', error);
			actionError = `Failed to update ${extension.displayName}.`;
		} finally {
			busyPluginName = '';
		}
	}

	function depReport(extension: ExtensionInfo): DepReport {
		const deps = extension.dependencies ?? [];
		const required = deps.filter((d) => !d.optional);
		const optional = deps.filter((d) => d.optional);
		const requiredMissing = required.filter((d) => !d.enabled);
		const requiredNotInstalled = required.filter((d) => !d.installed);
		return { required, optional, requiredMissing, requiredNotInstalled };
	}

	async function enableExtensionWithDeps(extension: ExtensionInfo) {
		if (busyPluginName) return;
		const report = depReport(extension);
		if (report.requiredNotInstalled.length > 0) {
			actionError = `Install ${report.requiredNotInstalled.map((d) => d.name).join(', ')} before enabling ${extension.displayName}.`;
			return;
		}

		const names = report.requiredMissing.map((d) => d.name);
		if (!extension.enabledInProject && extension.canToggle) {
			names.push(extension.pluginName);
		}
		if (names.length === 0) return;

		busyPluginName = extension.pluginName;
		actionError = '';
		try {
			const result = await setExtensionPluginsEnabled(names, true);
			if (!result.success) {
				actionError = result.error || `Failed to enable required plugins for ${extension.displayName}.`;
				return;
			}
			await load();
		} catch (error) {
			console.warn('Failed to bulk-enable plugins:', error);
			actionError = `Failed to enable required plugins for ${extension.displayName}.`;
		} finally {
			busyPluginName = '';
		}
	}

	function queueLocally(
		slug: string,
		pluginName: string,
		kind: ExtensionOpKind,
		displayName: string
	) {
		const key = pendingKey(slug, kind);
		if (pendingOps.has(key)) {
			pendingOps.delete(key);
		} else {
			for (const otherKind of ['install', 'update', 'uninstall'] as const) {
				if (otherKind !== kind) pendingOps.delete(pendingKey(slug, otherKind));
			}
			pendingOps.set(key, { slug, pluginName, kind, channel, name: displayName });
		}
		pendingOps = new Map(pendingOps);
	}

	function clearCart() {
		pendingOps.clear();
		pendingOps = new Map(pendingOps);
	}

	async function applyBatch() {
		if (pendingOps.size === 0 || runState.running) return;
		for (const op of pendingOps.values()) {
			await queueExtensionOp(op.slug, op.pluginName, op.kind, op.channel);
		}
		clearCart();
		await applyExtensionOps();
		startPolling();
	}

	function startPolling() {
		if (pollHandle !== null) return;
		pollHandle = setInterval(async () => {
			try {
				runState = await getExtensionOpState();
			} catch (error) {
				console.warn('Failed to poll install state:', error);
			}
			if (!runState.running) {
				stopPolling();
				await load();
			}
		}, 250);
	}

	function stopPolling() {
		if (pollHandle !== null) {
			clearInterval(pollHandle);
			pollHandle = null;
		}
	}

	$effect(() => {
		if (runState.running && pollHandle === null) {
			startPolling();
		}
	});

	function phaseLabel(op: ExtensionOp): string {
		switch (op.phase) {
			case 'queued':
				return 'Queued';
			case 'resolving_download':
				return 'Resolving download…';
			case 'downloading': {
				if (op.bytesTotal && op.bytesTotal > 0) {
					const mb = (b: number) => (b / (1024 * 1024)).toFixed(1);
					return `Downloading ${mb(op.bytesDone ?? 0)} / ${mb(op.bytesTotal)} MiB`;
				}
				return 'Downloading…';
			}
			case 'verifying':
				return 'Verifying…';
			case 'extracting':
				return 'Extracting…';
			case 'installing':
				return 'Installing…';
			case 'updating_project':
				return op.kind === 'uninstall' ? 'Disabling…' : 'Enabling…';
			case 'uninstalling':
				return 'Removing folder…';
			case 'pending_restart':
				return op.kind === 'uninstall'
					? 'Staged for removal on restart'
					: `Staged for update to v${op.resolvedVersion ?? '–'} on restart`;
			case 'success':
				return op.kind === 'install'
					? `Installed v${op.resolvedVersion ?? '–'}`
					: op.kind === 'update'
						? `Updated to v${op.resolvedVersion ?? '–'}`
						: 'Uninstalled';
			case 'failed':
				return op.error ?? 'Failed';
		}
	}

	function phaseDotClass(phase: ExtensionOpPhase): string {
		switch (phase) {
			case 'success':
				return 'bg-emerald-400';
			case 'failed':
				return 'bg-red-400';
			case 'pending_restart':
				return 'bg-amber-300';
			case 'queued':
				return 'bg-muted-foreground/40';
			default:
				return 'bg-foreground/45';
		}
	}

	function installedStatus(extension: ExtensionInfo): ExtensionCardStatus {
		if (extension.restartRequired) return 'restart';
		if (extension.runtimeState === 'failed' || extension.runtimeState === 'incompatible') return 'failed';
		if (extension.runtimeState === 'unavailable') return 'unavailable';
		if (!extension.enabledInProject) return 'disabled';
		if (extension.activeInSession) return 'active';
		return 'idle';
	}

	function installedStatusLabel(extension: ExtensionInfo): string {
		if (extension.restartRequired) return 'Restart required';
		if (extension.runtimeState === 'failed') return 'Failed to load';
		if (extension.runtimeState === 'incompatible') return 'Incompatible';
		if (extension.runtimeState === 'unavailable') return 'Unavailable';
		if (!extension.enabledInProject) return 'Disabled';
		return '';
	}

	function normalizedSearch(): string {
		return searchQuery.trim().toLowerCase();
	}

	function matchesInstalled(extension: ExtensionInfo): boolean {
		const query = normalizedSearch();
		if (!query) return true;
		return [
			extension.displayName,
			extension.pluginName,
			extension.description,
			extension.agentSummary,
			extension.whenToEnable,
			...(extension.enablesAgentTo ?? [])
		]
			.join(' ')
			.toLowerCase()
			.includes(query);
	}

	function matchesRemote(remote: CatalogExtension): boolean {
		const query = normalizedSearch();
		if (!query) return true;
		return [
			remote.name,
			remote.pluginName,
			remote.description,
			remote.agentSummary ?? '',
			remote.whenToEnable ?? '',
			...(remote.enablesAgentTo ?? [])
		]
			.join(' ')
			.toLowerCase()
			.includes(query);
	}

	function groupInstalled(extensions: ExtensionInfo[]): ExtensionGroup<ExtensionInfo>[] {
		const groups = new Map<string, ExtensionGroup<ExtensionInfo>>();
		for (const extension of extensions) {
			const key = extension.isRecommended ? 'recommended' : extension.domain || 'other';
			const label = extension.isRecommended
				? 'Recommended'
				: extension.domainLabel || 'Other extensions';
			const sortOrder = extension.isRecommended ? -1000 : extension.sortOrder || 999;
			const group = groups.get(key) ?? {
				key,
				label,
				sortOrder,
				enabled: 0,
				total: 0,
				items: []
			};
			group.sortOrder = Math.min(group.sortOrder, sortOrder);
			group.total += 1;
			if (extension.enabledInProject) group.enabled += 1;
			group.items.push(extension);
			groups.set(key, group);
		}
		return Array.from(groups.values())
			.map((group) => ({
				...group,
				items: group.items.sort((a, b) => a.displayName.localeCompare(b.displayName))
			}))
			.sort((a, b) => a.sortOrder - b.sortOrder || a.label.localeCompare(b.label));
	}

	function groupRemote(extensions: CatalogExtension[]): ExtensionGroup<CatalogExtension>[] {
		const groups = new Map<string, ExtensionGroup<CatalogExtension>>();
		for (const extension of extensions) {
			const key = extension.isRecommended ? 'recommended' : extension.domain || 'other';
			const label = extension.isRecommended
				? 'Recommended'
				: extension.domainLabel || 'Other extensions';
			const sortOrder = extension.isRecommended ? -1000 : extension.sortOrder ?? 999;
			const group = groups.get(key) ?? {
				key,
				label,
				sortOrder,
				enabled: 0,
				total: 0,
				items: []
			};
			group.sortOrder = Math.min(group.sortOrder, sortOrder);
			group.total += 1;
			group.items.push(extension);
			groups.set(key, group);
		}
		return Array.from(groups.values())
			.map((group) => ({
				...group,
				items: group.items.sort((a, b) => a.name.localeCompare(b.name))
			}))
			.sort((a, b) => a.sortOrder - b.sortOrder || a.label.localeCompare(b.label));
	}

	function installedActions(extension: ExtensionInfo): CardAction[] {
		const deps = depReport(extension);
		const blockingMissing = deps.requiredMissing.length > 0;
		const hasUninstalledDep = deps.requiredNotInstalled.length > 0;
		const updatePair = hasUpdate(extension);
		const updateSlug = remoteSlugFor(extension.pluginName);
		const uninstallSlug = updateSlug || extension.pluginName;
		const updateQueued = updateSlug !== '' && isQueued(updateSlug, 'update');
		const uninstallQueued = isQueued(uninstallSlug, 'uninstall');
		const busy = busyPluginName === extension.pluginName;
		const actions: CardAction[] = [];

		if (extension.canToggle) {
			if (!extension.enabledInProject && blockingMissing) {
				actions.push({
					label: busy
						? 'Saving…'
						: `Enable + ${deps.requiredMissing.length} dep${deps.requiredMissing.length === 1 ? '' : 's'}`,
					tone: 'warning',
					disabled: busy || runState.running || hasUninstalledDep,
					title: hasUninstalledDep
						? `Install ${deps.requiredNotInstalled.map((d) => d.name).join(', ')} first.`
						: `Enable ${extension.displayName} together with ${deps.requiredMissing.length} required plugin${deps.requiredMissing.length === 1 ? '' : 's'}.`,
					onclick: () => enableExtensionWithDeps(extension)
				});
			} else if (extension.enabledInProject && blockingMissing && !hasUninstalledDep) {
				actions.push({
					label: busy
						? 'Saving…'
						: `Enable ${deps.requiredMissing.length} dep${deps.requiredMissing.length === 1 ? '' : 's'}`,
					tone: 'warning',
					disabled: busy || runState.running,
					onclick: () => enableExtensionWithDeps(extension)
				});
			} else {
				actions.push({
					label: busy ? 'Saving…' : extension.enabledInProject ? 'Disable' : 'Enable',
					tone: extension.enabledInProject ? 'neutral' : 'primary',
					disabled: busy || runState.running,
					onclick: () => toggleExtension(extension)
				});
			}
		}

		if (updatePair && updateSlug !== '') {
			actions.push({
				label: updateQueued ? 'Queued · Update' : 'Update',
				tone: updateQueued ? 'warning' : 'neutral',
				icon: ArrowUp01Icon,
				disabled: runState.running,
				onclick: () => queueLocally(updateSlug, extension.pluginName, 'update', extension.displayName)
			});
		}

		if (extension.isProjectPlugin) {
			actions.push({
				label: uninstallQueued ? 'Queued · Uninstall' : 'Uninstall',
				tone: uninstallQueued ? 'danger' : 'neutral',
				icon: Delete01Icon,
				disabled: runState.running,
				title: 'Disable and delete this extension on the next apply.',
				onclick: () =>
					queueLocally(uninstallSlug, extension.pluginName, 'uninstall', extension.displayName)
			});
		}

		return actions;
	}

	function remoteActions(remote: CatalogExtension): CardAction[] {
		const installQueued = isQueued(remote.slug, 'install');
		return [
			{
				label: installQueued ? 'Queued' : 'Install',
				tone: installQueued ? 'warning' : 'primary',
				icon: installQueued ? Cancel01Icon : Add01Icon,
				disabled: runState.running,
				onclick: () => queueLocally(remote.slug, remote.pluginName, 'install', remote.name)
			}
		];
	}
</script>

<div class="mb-5 flex flex-wrap items-start justify-between gap-x-6 gap-y-3">
	<div class="min-w-0">
		<h2 class="text-[18px] font-medium text-foreground">Extensions</h2>
		<p class="mt-1 max-w-prose text-[12.5px] leading-relaxed text-muted-foreground/55">
			NeoStackAI core covers the agent on its own. Extensions are optional packs that add tools for specific Unreal systems — enable only what your project uses.
		</p>
	</div>

	<div class="flex shrink-0 items-center gap-2">
		<div class="w-[104px]">
			<CustomSelect
				value={channel}
				options={[
					{ value: 'stable', label: 'Stable' },
					{ value: 'beta', label: 'Beta' },
					{ value: 'dev', label: 'Dev' },
					{ value: 'alpha', label: 'Alpha' }
				]}
				onchange={(v) => onChannelChange(v as ExtensionCatalogChannel)}
			/>
		</div>

		<button
			type="button"
			role="switch"
			aria-checked={includeAllEngines}
			class="inline-flex shrink-0 items-center gap-2 rounded-md border px-3 py-2 text-[12px] transition-colors disabled:cursor-not-allowed disabled:opacity-50 {includeAllEngines
				? 'border-foreground/30 bg-foreground/[0.05] text-foreground'
				: 'border-border/55 text-muted-foreground hover:border-border hover:text-foreground'}"
			onclick={() => onIncludeAllEnginesChange(!includeAllEngines)}
			disabled={isLoading || isLoadingCatalog || runState.running}
		>
			<span
				class="h-1.5 w-1.5 rounded-full {includeAllEngines
					? 'bg-foreground'
					: 'bg-muted-foreground/35'}"
				aria-hidden="true"
			></span>
			All engines
		</button>

		<button
			class="inline-flex shrink-0 items-center gap-2 rounded-md border border-border/55 px-3 py-2 text-[12px] text-muted-foreground transition-colors hover:border-border hover:text-foreground disabled:cursor-not-allowed disabled:opacity-50"
			onclick={refresh}
			disabled={isLoading || !!busyPluginName || runState.running}
			aria-label="Refresh catalog"
		>
			<Icon icon={ArrowReloadHorizontalIcon} size={12} strokeWidth={1.5} />
			Refresh
		</button>
	</div>
</div>

<div class="mb-4">
	<input
		class="w-full rounded-md border border-border/55 bg-transparent px-3 py-2 text-[13px] text-foreground outline-none placeholder:text-muted-foreground/35 focus:border-foreground/40"
		placeholder="Search by name, tool, or Unreal system…"
		bind:value={searchQuery}
	/>
</div>

{#if pendingList.length > 0 && !runState.running}
	<div class="mb-4 rounded-lg border border-border/60 bg-card p-3.5">
		<div class="flex items-start justify-between gap-3">
			<div class="min-w-0 flex-1">
				<h3 class="text-[12.5px] font-medium text-foreground">
					{pendingList.length} pending operation{pendingList.length === 1 ? '' : 's'}
				</h3>
				<ul class="mt-1.5 flex flex-wrap gap-1.5 text-[11px] text-muted-foreground/75">
					{#each pendingList as op}
						<li class="inline-flex items-center gap-1.5 rounded-md border border-border/45 px-2 py-0.5">
							<span class="text-muted-foreground/45">{op.kind}</span>
							<span>{op.name}</span>
							<button
								class="ml-0.5 -mr-1 rounded px-1 text-muted-foreground/40 hover:text-foreground"
								onclick={() => queueLocally(op.slug, op.pluginName, op.kind, op.name)}
								aria-label={`Remove ${op.name} from batch`}
							>
								×
							</button>
						</li>
					{/each}
				</ul>
			</div>
			<div class="flex shrink-0 gap-2">
				<button
					class="rounded-md border border-border/55 px-2.5 py-1 text-[11.5px] text-muted-foreground transition-colors hover:border-border hover:text-foreground"
					onclick={clearCart}
				>
					Clear
				</button>
				<button
					class="inline-flex items-center gap-1.5 rounded-md border border-foreground/35 bg-foreground/[0.05] px-2.5 py-1 text-[11.5px] text-foreground transition-colors hover:bg-foreground/[0.1]"
					onclick={applyBatch}
				>
					<Icon icon={PlayIcon} size={11} strokeWidth={1.8} />
					Apply
				</button>
			</div>
		</div>
	</div>
{/if}

{#if runState.running || runState.ops.length > 0}
	<div class="mb-4 rounded-lg border border-border/60 bg-card p-3.5">
		<div class="mb-2 flex items-center justify-between gap-3">
			<h3 class="text-[12.5px] font-medium text-foreground">
				{runState.running
					? `Applying ${runState.ops.length} operation${runState.ops.length === 1 ? '' : 's'}…`
					: `Done · ${runState.succeeded} succeeded${runState.failed > 0 ? ` · ${runState.failed} failed` : ''}`}
			</h3>
			{#if !runState.running && runState.restartRecommended}
				<button
					class="inline-flex items-center gap-1.5 rounded-md border border-amber-400/35 px-2.5 py-1 text-[11.5px] text-amber-100/90 transition-colors hover:bg-amber-500/10"
					onclick={restartEditor}
				>
					Restart editor
				</button>
			{/if}
		</div>

		<ul class="space-y-1">
			{#each runState.ops as op}
				<li class="flex items-center gap-2 py-0.5 text-[11.5px]">
					<span class="h-1.5 w-1.5 shrink-0 rounded-full {phaseDotClass(op.phase)}" aria-hidden="true"></span>
					<span class="truncate text-foreground/85">{op.pluginName}</span>
					<span class="text-muted-foreground/45">· {op.kind}</span>
					<span class="ml-auto truncate text-muted-foreground/65">{phaseLabel(op)}</span>
				</li>
			{/each}
		</ul>
	</div>
{/if}

{#if settings.restartRequired && !runState.running}
	<div class="mb-4 flex items-start justify-between gap-3 rounded-lg border border-amber-400/25 bg-amber-500/[0.04] px-3.5 py-2.5">
		<div class="flex items-start gap-2">
			<Icon icon={AlertCircleIcon} size={14} strokeWidth={1.6} class="mt-0.5 text-amber-300/85" />
			<p class="text-[12px] leading-relaxed text-amber-100/80">
				Restart the editor to apply enable/disable changes.
			</p>
		</div>
		<button
			class="shrink-0 rounded-md border border-amber-400/30 px-2.5 py-1 text-[11.5px] text-amber-100/90 transition-colors hover:bg-amber-500/10"
			onclick={restartEditor}
		>
			Restart editor
		</button>
	</div>
{/if}

{#if actionError}
	<div class="mb-4 rounded-md border border-red-400/25 bg-red-500/[0.04] px-3 py-2 text-[12px] text-red-300/90">
		{actionError}
	</div>
{/if}

{#if isLoading || !hasLoadedOnce}
	<div class="flex flex-col items-center justify-center gap-2 rounded-lg border border-border/60 bg-card p-6 text-center">
		<span class="inline-block h-4 w-4 animate-spin rounded-full border-2 border-muted-foreground/30 border-t-muted-foreground"></span>
		<p class="text-[12.5px] text-muted-foreground/55">Loading extensions…</p>
	</div>
{:else if settings.extensions.length === 0}
	<div class="rounded-lg border border-border/60 bg-card p-6 text-center">
		<p class="text-[12.5px] text-muted-foreground/60">No NeoStack extensions in this project yet.</p>
		<p class="mt-1 text-[11px] text-muted-foreground/40">
			Install one from NeoStack Cloud below, or drop an extension under
			<span class="font-mono text-foreground/70">Plugins/NeoExtensions</span>.
		</p>
	</div>
{:else}
	<div class="mb-3 flex items-baseline justify-between gap-3">
		<h3 class="text-[13px] font-medium text-foreground">
			Installed <span class="tabular-nums text-muted-foreground/45">· {filteredInstalled.length}</span>
		</h3>
		<p class="text-[11px] tabular-nums text-muted-foreground/45">
			{enabledCount} enabled · {activeCount} active
		</p>
	</div>

	{#if filteredInstalled.length === 0}
		<div class="rounded-lg border border-border/60 bg-card p-5 text-center">
			<p class="text-[12px] text-muted-foreground/55">No installed extensions match your search.</p>
		</div>
	{:else}
		<div class="space-y-5">
			{#each installedGroups as group}
				<section>
					<div class="mb-1.5 flex items-baseline justify-between gap-3">
						<h4 class="text-[11px] font-medium uppercase tracking-wide text-muted-foreground/55">
							{group.label}
						</h4>
						<p class="text-[10.5px] tabular-nums text-muted-foreground/40">
							{group.enabled}/{group.total}
						</p>
					</div>
					<div class="space-y-2">
						{#each group.items as extension}
							{@const updatePair = hasUpdate(extension)}
							<ExtensionCard
								name={extension.displayName}
								pluginName={extension.pluginName}
								version={extension.version}
								vendor={extension.vendor}
								summary={extension.agentSummary}
								description={extension.description}
								enablesAgentTo={extension.enablesAgentTo}
								whenToEnable={extension.whenToEnable}
								dependencies={extension.dependencies}
								status={installedStatus(extension)}
								statusLabel={installedStatusLabel(extension)}
								statusMessage={extension.statusMessage}
								updateLabel={updatePair
									? `Update available · v${updatePair.remote.latestVersion}`
									: ''}
								isThirdParty={extension.isThirdParty}
								details={[
									{
										label: 'Source',
										value: extension.isProjectPlugin
											? 'Project'
											: extension.isInstalledOnEngine
												? 'Engine'
												: 'External'
									},
									{ label: 'Domain', value: extension.domainLabel || extension.category },
									{ label: 'Core API', value: String(settings.coreApiVersion) }
								]}
								actions={installedActions(extension)}
							/>
						{/each}
					</div>
				</section>
			{/each}
		</div>
	{/if}
{/if}

<div class="mt-10">
	<div class="mb-3 flex items-baseline justify-between gap-3">
		<h3 class="text-[13px] font-medium text-foreground">
			Available
			{#if catalog.status === 'ready'}
				<span class="tabular-nums text-muted-foreground/45">· {availableFromCloud.length}</span>
			{/if}
		</h3>
		<p class="text-[11px] text-muted-foreground/45">
			<span class="capitalize text-foreground/55">{channel}</span>
			{#if catalog.engine}
				· UE <span class="tabular-nums text-foreground/55">{catalog.engine}</span>
			{:else if includeAllEngines}
				· all engines
			{/if}
		</p>
	</div>

	{#if (catalog.status === 'fetching' && catalog.extensions.length === 0) || catalog.status === 'idle'}
		<div class="flex flex-col items-center justify-center gap-2 rounded-lg border border-border/60 bg-card p-6 text-center">
			<span class="inline-block h-4 w-4 animate-spin rounded-full border-2 border-muted-foreground/30 border-t-muted-foreground"></span>
			<p class="text-[12.5px] text-muted-foreground/55">Fetching catalog…</p>
		</div>
	{:else if catalog.status === 'error'}
		<div class="rounded-md border border-red-400/25 bg-red-500/[0.04] px-3 py-2 text-[12px] text-red-300/90">
			<div class="flex items-start gap-2">
				<Icon icon={AlertCircleIcon} size={13} strokeWidth={1.6} class="mt-0.5" />
				<div class="min-w-0 flex-1">
					<div class="font-medium">Couldn’t load the catalog</div>
					<div class="mt-0.5 text-red-200/75">{catalog.error ?? 'Unknown error'}</div>
					{#if catalog.httpStatus === 401}
						<div class="mt-1 text-red-200/70">
							Your saved NeoStack key looks invalid or revoked. Sign in again from the Setup Checklist, or paste a fresh key in Chat Providers → NeoStack Cloud.
						</div>
					{/if}
				</div>
			</div>
		</div>
	{:else if availableFromCloud.length === 0 && catalog.status === 'ready'}
		<div class="rounded-lg border border-border/60 bg-card p-5 text-center">
			<Icon icon={CloudDownloadIcon} size={18} strokeWidth={1.5} class="mx-auto mb-1.5 text-muted-foreground/45" />
			<p class="text-[12px] text-muted-foreground/55">
				{searchQuery.trim()
					? 'No available extensions match your search.'
					: 'Nothing new to install on this channel.'}
			</p>
		</div>
	{:else}
		<div class="space-y-5">
			{#each availableGroups as group}
				<section>
					<div class="mb-1.5 flex items-baseline justify-between gap-3">
						<h4 class="text-[11px] font-medium uppercase tracking-wide text-muted-foreground/55">
							{group.label}
						</h4>
						<p class="text-[10.5px] tabular-nums text-muted-foreground/40">{group.total}</p>
					</div>
					<div class="space-y-2">
						{#each group.items as remote}
							<ExtensionCard
								name={remote.name}
								pluginName={remote.pluginName}
								version={remote.latestVersion}
								summary={remote.agentSummary ?? ''}
								description={remote.description}
								enablesAgentTo={remote.enablesAgentTo ?? []}
								whenToEnable={remote.whenToEnable ?? ''}
								changelog={remote.changelog}
								details={[
									{ label: 'Channel', value: remote.latestChannel },
									{ label: 'Domain', value: remote.domainLabel ?? '' },
									{ label: 'Engines', value: remote.supportedEngineVersions.join(', ') }
								]}
								actions={remoteActions(remote)}
								queued={isQueued(remote.slug, 'install')}
							/>
						{/each}
					</div>
				</section>
			{/each}
		</div>
	{/if}
</div>
