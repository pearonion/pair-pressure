<script lang="ts">
	import { onMount } from 'svelte';
	import {
		copyToClipboard,
		getMcpConnectionInfo,
		openUrl,
		type McpConnectionInfo
	} from '$lib/bridge.js';
	import { CheckmarkCircle02Icon, Copy01Icon, Link01Icon } from '@hugeicons/core-free-icons';
	import Icon from '$lib/components/Icon.svelte';

	type IntegrationCard = {
		id: string;
		title: string;
		configPath: string;
		docsUrl: string;
		note: string;
		snippet: string;
		quickSteps: string[];
	};

	let info = $state<McpConnectionInfo | null>(null);
	let isLoading = $state(false);
	let lastCopiedKey = $state('');
	let errorMessage = $state('');

	async function load() {
		if (isLoading) return;
		isLoading = true;
		errorMessage = '';
		try {
			info = await getMcpConnectionInfo();
		} catch (e) {
			console.warn('Failed to load MCP connection info:', e);
			info = null;
			errorMessage = 'Could not load MCP connection info. The editor bridge may still be starting up.';
		} finally {
			isLoading = false;
		}
	}

	onMount(() => {
		load();
	});

	function markCopied(key: string) {
		lastCopiedKey = key;
		setTimeout(() => {
			if (lastCopiedKey === key) {
				lastCopiedKey = '';
			}
		}, 1600);
	}

	async function handleCopy(key: string, text: string) {
		await copyToClipboard(text);
		markCopied(key);
	}

	function copyLabel(key: string): string {
		return lastCopiedKey === key ? 'Copied' : 'Copy';
	}

	function formatJson(value: unknown): string {
		return JSON.stringify(value, null, 2);
	}

	let cards = $derived.by((): IntegrationCard[] => {
		if (!info) return [];

		return [
			{
				id: 'cursor',
				title: 'Cursor IDE',
				configPath: '.cursor/mcp.json or ~/.cursor/mcp.json',
				docsUrl: 'https://docs.cursor.com/context/model-context-protocol',
				note: 'Cursor supports HTTP MCP servers through mcp.json.',
				snippet: formatJson({
					mcpServers: {
						[info.serverName]: {
							url: info.recommendedUrl
						}
					}
				}),
				quickSteps: [
					'Save this JSON in your project for workspace-only access, or in ~/.cursor/mcp.json for all projects.',
					'Restart Cursor or reload the window if the server does not appear immediately.'
				]
			},
			{
				id: 'vscode',
				title: 'VS Code + GitHub Copilot',
				configPath: '.vscode/mcp.json',
				docsUrl: 'https://code.visualstudio.com/docs/copilot/customization/mcp-servers',
				note: 'VS Code uses a servers map in .vscode/mcp.json for MCP configuration.',
				snippet: formatJson({
					servers: {
						[info.serverName]: {
							type: 'http',
							url: info.recommendedUrl
						}
					}
				}),
				quickSteps: [
					'Save this JSON in .vscode/mcp.json inside the workspace you want Copilot to use.',
					'Open the workspace in VS Code and reload if Copilot does not pick up the server right away.'
				]
			},
			{
				id: 'copilot-cli',
				title: 'GitHub Copilot CLI',
				configPath: '~/.copilot/mcp-config.json',
				docsUrl: 'https://docs.github.com/en/copilot/how-tos/copilot-cli/customize-copilot/add-mcp-servers',
				note: 'Copilot CLI supports HTTP MCP servers and can also add them interactively with /mcp add.',
				snippet: formatJson({
					mcpServers: {
						[info.serverName]: {
							type: 'http',
							url: info.recommendedUrl,
							tools: ['*']
						}
					}
				}),
				quickSteps: [
					'In interactive mode, run /mcp add, choose HTTP, paste the URL, and keep Tools as *.',
					'Or edit ~/.copilot/mcp-config.json directly with the JSON below.'
				]
			}
		];
	});
</script>

<div class="mb-4 rounded-lg border border-border/60 bg-card p-4">
	<div class="mb-4">
		<h3 class="text-[14px] font-medium text-foreground">MCP Server</h3>
		<p class="mt-1 text-[12px] text-muted-foreground/60">
			Use this live MCP endpoint to connect local desktop clients. The plugin may auto-scan to a nearby port if the preferred port was busy.
		</p>
	</div>

	{#if isLoading}
		<div class="flex items-center gap-2 py-3 text-[12px] text-muted-foreground/50">
			<span class="inline-block h-3.5 w-3.5 animate-spin rounded-full border-2 border-muted-foreground/30 border-t-muted-foreground"></span>
			Loading MCP endpoints...
		</div>
	{:else if info}
		{@const mcpInfo = info}
		<div class="grid gap-3">
			<div class="rounded-md border border-border/40 bg-background/50 p-3">
				<div class="mb-1 flex items-center justify-between gap-3">
					<div>
						<div class="text-[12px] font-medium text-muted-foreground">Primary MCP URL</div>
						<div class="mt-1 font-mono text-[12px] text-foreground break-all">{mcpInfo.recommendedUrl}</div>
					</div>
					<button
						class="inline-flex shrink-0 items-center gap-1.5 rounded-md border border-border/60 px-2.5 py-1.5 text-[12px] text-foreground transition-colors hover:bg-accent"
						onclick={() => handleCopy('primary-url', mcpInfo.recommendedUrl)}
					>
						<Icon icon={lastCopiedKey === 'primary-url' ? CheckmarkCircle02Icon : Copy01Icon} size={13} strokeWidth={1.5} />
						{copyLabel('primary-url')}
					</button>
				</div>
				<p class="text-[11px] text-muted-foreground/50">
					Streamable HTTP on <span class="font-mono">/mcp</span>. Use <span class="font-mono">127.0.0.1</span> if a client has localhost or IPv6 resolution issues.
				</p>
			</div>

			<div class="rounded-md border border-border/40 bg-background/50 p-3">
				<div class="mb-1 flex items-center justify-between gap-3">
					<div>
						<div class="text-[12px] font-medium text-muted-foreground">Alternate localhost URL</div>
						<div class="mt-1 font-mono text-[12px] text-foreground break-all">{mcpInfo.localhostUrl}</div>
					</div>
					<button
						class="inline-flex shrink-0 items-center gap-1.5 rounded-md border border-border/60 px-2.5 py-1.5 text-[12px] text-foreground transition-colors hover:bg-accent"
						onclick={() => handleCopy('localhost-url', mcpInfo.localhostUrl)}
					>
						<Icon icon={lastCopiedKey === 'localhost-url' ? CheckmarkCircle02Icon : Copy01Icon} size={13} strokeWidth={1.5} />
						{copyLabel('localhost-url')}
					</button>
				</div>
				<p class="text-[11px] text-muted-foreground/50">
					Server name: <span class="font-mono">{mcpInfo.serverName}</span>. Legacy SSE endpoint is <span class="font-mono">{mcpInfo.legacySseUrl}</span> for older clients that still need it.
				</p>
			</div>
		</div>

		<div class="mt-5">
			<h4 class="text-[13px] font-medium text-foreground">Integrations</h4>
			<p class="mt-1 text-[12px] text-muted-foreground/60">
				Ready-to-paste configs for common local MCP clients.
			</p>
		</div>

		<div class="mt-3 grid gap-3">
			{#each cards as card}
				<div class="rounded-lg border border-border/50 bg-background/40 p-4">
					<div class="flex flex-wrap items-start justify-between gap-3">
						<div>
							<h5 class="text-[13px] font-medium text-foreground">{card.title}</h5>
							<p class="mt-1 text-[11px] text-muted-foreground/50">{card.note}</p>
							<p class="mt-1 text-[11px] text-muted-foreground/40">
								Config path: <span class="font-mono">{card.configPath}</span>
							</p>
						</div>
						<div class="flex items-center gap-2">
							<button
								class="inline-flex items-center gap-1.5 rounded-md border border-border/60 px-2.5 py-1.5 text-[12px] text-foreground transition-colors hover:bg-accent"
								onclick={() => handleCopy(`snippet-${card.id}`, card.snippet)}
							>
								<Icon icon={lastCopiedKey === `snippet-${card.id}` ? CheckmarkCircle02Icon : Copy01Icon} size={13} strokeWidth={1.5} />
								{copyLabel(`snippet-${card.id}`)}
							</button>
							<button
								class="inline-flex items-center gap-1.5 rounded-md border border-border/60 px-2.5 py-1.5 text-[12px] text-foreground transition-colors hover:bg-accent"
								onclick={() => openUrl(card.docsUrl)}
							>
								<Icon icon={Link01Icon} size={13} strokeWidth={1.5} />
								Docs
							</button>
						</div>
					</div>

					<div class="mt-3 rounded-md border border-border/40 bg-terminal-bg px-3 py-2">
						<pre class="overflow-x-auto whitespace-pre-wrap break-all font-mono text-[11px] leading-5 text-slate-100">{card.snippet}</pre>
					</div>

					<div class="mt-3 flex flex-col gap-1 text-[11px] text-muted-foreground/55">
						{#each card.quickSteps as step}
							<div>{step}</div>
						{/each}
					</div>
				</div>
			{/each}
		</div>
	{:else}
		<div class="rounded-md border border-red-500/20 bg-red-500/5 px-3 py-2 text-[12px] text-red-300/80">
			{errorMessage || 'Failed to load MCP connection info.'}
		</div>
	{/if}
</div>
