<script lang="ts">
	import Icon from '$lib/components/Icon.svelte';
	import {
		ArrowReloadHorizontalIcon,
		AlertCircleIcon,
		CheckmarkCircle02Icon,
		PencilEdit02Icon,
		InformationCircleIcon,
		FolderOpenIcon
	} from '@hugeicons/core-free-icons';
	import {
		getSkills,
		openSkillFile,
		rescanSkills,
		resolveSkillConflict,
		type SkillsState,
		type SkillStatus,
		type SkillConflictMode,
		type SkillSyncReport
	} from '$lib/bridge.js';

	let state = $state<SkillsState>({ projectDir: '', manifestPath: '', skills: [], projectSkills: [] });
	let isLoading = $state(false);
	let isRescanning = $state(false);
	let hasLoadedOnce = $state(false);
	let lastReport = $state<SkillSyncReport | null>(null);
	let actionError = $state('');
	let busySkill = $state('');

	async function load() {
		isLoading = true;
		try {
			state = await getSkills();
		} catch (e) {
			actionError = `Failed to load skills: ${e}`;
		}
		isLoading = false;
		hasLoadedOnce = true;
	}

	async function handleRescan() {
		isRescanning = true;
		actionError = '';
		try {
			lastReport = await rescanSkills();
			await load();
		} catch (e) {
			actionError = `Rescan failed: ${e}`;
		}
		isRescanning = false;
	}

	async function handleResolve(name: string, mode: SkillConflictMode) {
		busySkill = name;
		actionError = '';
		try {
			const r = await resolveSkillConflict(name, mode);
			if (!r.success && r.error) { actionError = r.error; }
			await load();
		} catch (e) {
			actionError = `Resolve failed: ${e}`;
		}
		busySkill = '';
	}

	function group(skills: SkillStatus[]): Array<{ source: string; items: SkillStatus[] }> {
		const map = new Map<string, SkillStatus[]>();
		for (const s of skills) {
			const key = s.sourceDisplayName || s.sourceId || 'Unknown';
			if (!map.has(key)) { map.set(key, []); }
			map.get(key)!.push(s);
		}
		// Core first, then extensions alphabetically.
		return Array.from(map.entries())
			.sort(([a], [b]) => {
				const aCore = a.toLowerCase().includes('core');
				const bCore = b.toLowerCase().includes('core');
				if (aCore && !bCore) return -1;
				if (!aCore && bCore) return 1;
				return a.localeCompare(b);
			})
			.map(([source, items]) => ({ source, items }));
	}

	let grouped = $derived(group(state.skills));
	let editedCount = $derived(state.skills.filter((s) => s.userEdited).length);
	let conflictCount = $derived(state.skills.filter((s) => s.conflictPending).length);
	let installedCount = $derived(state.skills.filter((s) => s.installedPaths.length > 0).length);
	let projectSkillCount = $derived(state.projectSkills.length);

	$effect(() => {
		void load();
	});
</script>

<div class="mb-6 flex flex-wrap items-start justify-between gap-x-6 gap-y-3">
	<div>
		<h2 class="text-[18px] font-medium text-foreground">Agent Skills</h2>
		<p class="mt-1 text-[13px] text-muted-foreground/60">
			NeoStack ships skills from installed plugins and syncs them into
			<code class="rounded bg-foreground/5 px-1.5 py-0.5 text-[11px] text-muted-foreground">.claude/skills/</code>
			and
			<code class="rounded bg-foreground/5 px-1.5 py-0.5 text-[11px] text-muted-foreground">.agents/skills/</code>.
			Custom skills you add to those folders are picked up by Claude Code, Codex, and Copilot CLI — listed below under
			<span class="text-foreground/80">Your project skills</span>.
		</p>
	</div>
	<button
		type="button"
		onclick={handleRescan}
		disabled={isRescanning}
		class="inline-flex shrink-0 items-center gap-2 rounded-md border border-border/60 px-3 py-2 text-[12px] text-muted-foreground transition-colors hover:bg-accent hover:text-foreground disabled:cursor-not-allowed disabled:opacity-50"
	>
		<Icon icon={ArrowReloadHorizontalIcon} class={isRescanning ? 'animate-spin' : ''} size={14} />
		{isRescanning ? 'Rescanning…' : 'Rescan + sync'}
	</button>
</div>

{#if actionError}
	<div class="mb-4 flex items-start gap-2 rounded-lg border border-red-500/30 bg-red-500/10 p-3 text-[12px] text-red-300">
		<Icon icon={AlertCircleIcon} size={14} />
		<span>{actionError}</span>
	</div>
{/if}

{#if lastReport}
	<div class="mb-4 rounded-lg border border-border/60 bg-card p-3 text-[12px] text-muted-foreground/75">
		Sync result —
		installed <span class="tabular-nums text-foreground">{lastReport.installed}</span>,
		updated <span class="tabular-nums text-foreground">{lastReport.updated}</span>,
		no-op <span class="tabular-nums text-foreground">{lastReport.noOp}</span>,
		user edits kept <span class="tabular-nums text-foreground">{lastReport.userEditsKept}</span>,
		conflicts <span class="tabular-nums text-foreground">{lastReport.conflicts}</span>,
		orphans removed <span class="tabular-nums text-foreground">{lastReport.orphansRemoved}</span>{#if lastReport.errors.length > 0},
			errors <span class="tabular-nums text-red-300">{lastReport.errors.length}</span>{/if}.
	</div>
{/if}

<div class="mb-4 flex flex-wrap items-center gap-x-4 gap-y-1 text-[12px] text-muted-foreground/60">
	<span>NeoStack skills <span class="tabular-nums text-foreground">{state.skills.length}</span></span>
	<span>Installed <span class="tabular-nums text-foreground">{installedCount}</span></span>
	<span>Project skills <span class="tabular-nums text-foreground">{projectSkillCount}</span></span>
	<span>User-edited <span class="tabular-nums text-foreground">{editedCount}</span></span>
	{#if conflictCount > 0}
		<span class="text-amber-300">Conflicts <span class="tabular-nums">{conflictCount}</span></span>
	{/if}
</div>

{#if isLoading && !hasLoadedOnce}
	<div class="flex flex-col items-center justify-center gap-2 rounded-lg border border-border/60 bg-card p-6 text-center">
		<Icon icon={ArrowReloadHorizontalIcon} class="animate-spin" size={16} />
		<p class="text-[13px] text-muted-foreground/60">Loading skills…</p>
	</div>
{:else}
	{#if state.skills.length === 0}
		<div class="rounded-lg border border-border/60 bg-card p-6 text-center">
			<p class="text-[13px] text-muted-foreground/60">No skills shipped by any installed NeoStack plugin.</p>
			<p class="mt-1 text-[11px] text-muted-foreground/40">
				Core + each extension contributes its own <code class="rounded bg-foreground/5 px-1 py-0.5">Resources/Skills/</code> folder.
			</p>
		</div>
	{:else}
		{#each grouped as group (group.source)}
			<div class="mt-6 first:mt-0">
				<div class="mb-2 flex items-center gap-2 text-[12px] font-medium text-foreground">
					{group.source}
					<span class="tabular-nums text-muted-foreground/50">· {group.items.length}</span>
				</div>
				<ul class="space-y-2">
					{#each group.items as skill (skill.name)}
						<li class="rounded-lg border border-border/60 bg-card p-3.5">
							<div class="flex items-start justify-between gap-3">
								<div class="min-w-0 flex-1">
									<div class="flex flex-wrap items-center gap-2">
										<code class="rounded bg-foreground/6 px-1.5 py-0.5 text-[12px] font-medium text-foreground">
											{skill.name}
										</code>
										{#if skill.sourceVersion}
											<span class="text-[10px] text-muted-foreground/50">v{skill.sourceVersion}</span>
										{/if}
										{#if skill.conflictPending}
											<span class="inline-flex items-center gap-1 rounded-full border border-amber-500/30 bg-amber-500/10 px-2 py-0.5 text-[10px] text-amber-300">
												<Icon icon={AlertCircleIcon} size={10} />
												Upstream update available
											</span>
										{:else if skill.userEdited}
											<span class="inline-flex items-center gap-1 rounded-full border border-blue-500/30 bg-blue-500/10 px-2 py-0.5 text-[10px] text-blue-300">
												<Icon icon={PencilEdit02Icon} size={10} />
												Edited locally
											</span>
										{:else if skill.installedPaths.length > 0}
											<span class="inline-flex items-center gap-1 rounded-full border border-emerald-500/30 bg-emerald-500/10 px-2 py-0.5 text-[10px] text-emerald-300">
												<Icon icon={CheckmarkCircle02Icon} size={10} />
												In sync
											</span>
										{:else}
											<span class="inline-flex items-center gap-1 rounded-full border border-border/50 bg-foreground/5 px-2 py-0.5 text-[10px] text-muted-foreground/70">
												Not installed
											</span>
										{/if}
									</div>
									{#if skill.description}
										<p class="mt-1.5 text-[12px] leading-relaxed text-muted-foreground/70">
											{skill.description}
										</p>
									{/if}
									{#if skill.tags.length > 0}
										<div class="mt-1.5 flex flex-wrap gap-1">
											{#each skill.tags as tag (tag)}
												<span class="rounded-full bg-foreground/5 px-1.5 py-0.5 text-[9px] uppercase tracking-wide text-muted-foreground/50">
													{tag}
												</span>
											{/each}
										</div>
									{/if}
								</div>
							</div>

							<div class="mt-2.5 flex flex-wrap items-center gap-2">
								{#if skill.installedPaths.length > 0}
									<button
										type="button"
										onclick={() => openSkillFile(skill.name, false)}
										class="inline-flex items-center gap-1.5 rounded-md border border-border/50 px-2 py-1 text-[11px] text-muted-foreground transition-colors hover:bg-accent hover:text-foreground"
									>
										<Icon icon={FolderOpenIcon} size={11} />
										Open SKILL.md
									</button>
								{/if}
								{#if skill.conflictPending}
									<button
										type="button"
										onclick={() => openSkillFile(skill.name, true)}
										class="inline-flex items-center gap-1.5 rounded-md border border-amber-500/40 px-2 py-1 text-[11px] text-amber-300 transition-colors hover:bg-amber-500/10"
									>
										<Icon icon={InformationCircleIcon} size={11} />
										Review SKILL.new.md
									</button>
									<button
										type="button"
										disabled={busySkill === skill.name}
										onclick={() => handleResolve(skill.name, 'keep-user')}
										class="inline-flex items-center gap-1.5 rounded-md border border-border/50 px-2 py-1 text-[11px] text-muted-foreground transition-colors hover:bg-accent hover:text-foreground disabled:opacity-50"
									>
										Keep mine
									</button>
									<button
										type="button"
										disabled={busySkill === skill.name}
										onclick={() => handleResolve(skill.name, 'take-new')}
										class="inline-flex items-center gap-1.5 rounded-md border border-emerald-500/30 bg-emerald-500/10 px-2 py-1 text-[11px] text-emerald-300 transition-colors hover:bg-emerald-500/20 disabled:opacity-50"
									>
										Take upstream
									</button>
								{/if}
							</div>
						</li>
					{/each}
				</ul>
			</div>
		{/each}
	{/if}

	<div class="mt-6">
		<div class="mb-2 flex items-center gap-2 text-[12px] font-medium text-foreground">
			Your project skills
			<span class="tabular-nums text-muted-foreground/50">· {projectSkillCount}</span>
		</div>

		{#if projectSkillCount === 0}
			<div class="rounded-lg border border-border/60 bg-card p-4 text-center">
				<p class="text-[13px] text-muted-foreground/60">No custom project skills found.</p>
				<p class="mt-1 text-[11px] text-muted-foreground/40">
					Add folders to
					<code class="rounded bg-foreground/5 px-1 py-0.5">.agents/skills/&lt;name&gt;/SKILL.md</code>
					(or
					<code class="rounded bg-foreground/5 px-1 py-0.5">.claude/skills/</code>
					for Claude Code).
				</p>
			</div>
		{:else}
			<ul class="space-y-2">
				{#each state.projectSkills as skill (skill.name)}
					<li class="rounded-lg border border-border/60 bg-card p-3.5">
						<div class="flex items-start justify-between gap-3">
							<div class="min-w-0 flex-1">
								<div class="flex flex-wrap items-center gap-2">
									<code class="rounded bg-foreground/6 px-1.5 py-0.5 text-[12px] font-medium text-foreground">
										{skill.name}
									</code>
									{#if skill.parseError}
										<span class="inline-flex items-center gap-1 rounded-full border border-amber-500/30 bg-amber-500/10 px-2 py-0.5 text-[10px] text-amber-300">
											<Icon icon={AlertCircleIcon} size={10} />
											Invalid SKILL.md
										</span>
									{:else}
										<span class="inline-flex items-center gap-1 rounded-full border border-emerald-500/30 bg-emerald-500/10 px-2 py-0.5 text-[10px] text-emerald-300">
											<Icon icon={CheckmarkCircle02Icon} size={10} />
											Available to agents
										</span>
									{/if}
								</div>
								{#if skill.description}
									<p class="mt-1.5 text-[12px] leading-relaxed text-muted-foreground/70">
										{skill.description}
									</p>
								{:else if skill.parseError}
									<p class="mt-1.5 text-[12px] leading-relaxed text-muted-foreground/50">
										{skill.parseError}
									</p>
								{/if}
								{#if skill.tags.length > 0}
									<div class="mt-1.5 flex flex-wrap gap-1">
										{#each skill.tags as tag (tag)}
											<span class="rounded-full bg-foreground/5 px-1.5 py-0.5 text-[9px] uppercase tracking-wide text-muted-foreground/50">
												{tag}
											</span>
										{/each}
									</div>
								{/if}
							</div>
						</div>

						<div class="mt-2.5 flex flex-wrap items-center gap-2">
							<button
								type="button"
								onclick={() => openSkillFile(skill.folderName || skill.name, false)}
								class="inline-flex items-center gap-1.5 rounded-md border border-border/50 px-2 py-1 text-[11px] text-muted-foreground transition-colors hover:bg-accent hover:text-foreground"
							>
								<Icon icon={FolderOpenIcon} size={11} />
								Open SKILL.md
							</button>
						</div>
					</li>
				{/each}
			</ul>
		{/if}
	</div>

	<div class="mt-6 rounded-md border border-border/40 bg-background/40 p-3 text-[11px] text-muted-foreground/50">
		Manifest: <code class="text-muted-foreground/70">{state.manifestPath}</code>
	</div>
{/if}
