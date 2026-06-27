/**
 * TypeScript wrapper for the UE ↔ JS bridge (window.ue.bridge).
 * All calls are async (UE returns Promises from bound UFUNCTIONs).
 *
 * Transport modes:
 * - 'embedded': Running inside UE's CEF browser, uses window.ue.bridge
 * - 'remote': Running on website, uses WebSocket relay to connected instance
 * - 'standalone': No backend available (dev mode), returns mock data
 */
import { createUUID } from '$lib/utils.js';
import { relayCall, onRelayEvent, getRelayState } from './relay.js';

export type Transport = 'embedded' | 'remote' | 'standalone';

let currentTransport: Transport = 'standalone';

export function getTransport(): Transport {
	return currentTransport;
}

export function setTransport(transport: Transport): void {
	currentTransport = transport;
}

/**
 * Detect and set the transport mode.
 * Called during app initialization.
 */
export function detectTransport(): Transport {
	if (getBridge()) {
		currentTransport = 'embedded';
	} else if (getRelayState() === 'connected') {
		currentTransport = 'remote';
	} else {
		currentTransport = 'standalone';
	}
	return currentTransport;
}

export type AgentStatus = 'available' | 'not_installed' | 'missing_key' | 'unknown';

export type AgentInfo = {
	id: string;
	name: string;
	status: AgentStatus;
	statusMessage: string;
	isBuiltIn: boolean;
	isConnected: boolean;
	registryId?: string;
	iconUrl?: string;
	description?: string;
};

export type SessionInfo = {
	sessionId: string;
	agentName: string;
	/** ACP registry agent id when configured (e.g. claude-acp); empty for bundled-only rows */
	registryId?: string;
	/** Server-computed: embedded terminal can generate a CLI resume line for this agent */
	terminalResumeSupported?: boolean;
	title: string;
	messageCount?: number;
	createdAt?: string;
	lastModifiedAt: string;
	isConnected: boolean;
	isActive?: boolean;
	/** True when the user has explicitly renamed this session — title survives remote sync */
	hasCustomTitle?: boolean;
};

export type ExportSessionResult = {
	success: boolean;
	canceled?: boolean;
	savedPath?: string;
	error?: string;
};

export type ToolResultImage = {
	base64: string;
	mimeType: string;
	width: number;
	height: number;
};

export type ContentBlock = {
	type: 'text' | 'thought' | 'tool_call' | 'tool_result' | 'image' | 'error' | 'system';
	text: string;
	isStreaming: boolean;
	toolCallId?: string;
	toolName?: string;
	toolArguments?: string;
	toolResult?: string;
	toolSuccess?: boolean;
	imageCount?: number;
	images?: ToolResultImage[];
	/** If this tool call was made inside a subagent (Task), the parent Task's toolCallId */
	parentToolCallId?: string;
	/** For system status blocks (e.g. "compacting", "compacted") */
	systemStatus?: string;
};

export type ChatMessage = {
	messageId: string;
	role: 'user' | 'assistant' | 'system';
	isStreaming: boolean;
	timestamp: string;
	contentBlocks: ContentBlock[];
};

export type ModelUsageEntry = {
	modelName: string;
	inputTokens: number;
	outputTokens: number;
	cacheReadTokens: number;
	cacheCreationTokens: number;
	costUSD: number;
	contextWindow: number;
	maxOutputTokens: number;
};

export type StreamingUpdate = {
	agentName: string;
	type:
		| 'text_chunk'
		| 'thought_chunk'
		| 'tool_call'
		| 'tool_result'
		| 'error'
		| 'usage'
		| 'plan'
		| 'user_message_chunk'
		| 'history_replay_started'
		| 'history_replay_finished'
		| 'unknown';
	text: string;
	systemStatus?: string;
	toolCallId?: string;
	toolName?: string;
	toolArguments?: string;
	toolResult?: string;
	toolSuccess?: boolean;
	images?: ToolResultImage[];
	/** If this tool call was made inside a subagent (Task), the parent Task's toolCallId */
	parentToolCallId?: string;
	errorMessage?: string;
	errorCode?: number;
	// Usage fields (present when type === 'usage')
	inputTokens?: number;
	outputTokens?: number;
	totalTokens?: number;
	cacheReadTokens?: number;
	cacheCreationTokens?: number;
	reasoningTokens?: number;
	costAmount?: number;
	costCurrency?: string;
	turnCostUSD?: number;
	contextUsed?: number;
	contextSize?: number;
	numTurns?: number;
	durationMs?: number;
	modelUsage?: ModelUsageEntry[];
	replayMessageCount?: number;
	replayEmpty?: boolean;
	replayPreserveCached?: boolean;
};

// Check if we're running inside UE's embedded browser
function getBridge(): any | null {
	if (typeof window !== 'undefined' && (window as any).ue?.bridge) {
		return (window as any).ue.bridge;
	}
	return null;
}

/**
 * Wait for the UE bridge to become available.
 * The CEF browser starts loading the page before BindUObject() completes,
 * so window.ue.bridge may not be available when onMount fires.
 * This polls until the bridge appears or the timeout expires.
 */
export async function waitForBridge(maxWaitMs = 5000): Promise<boolean> {
	// Already available — no wait needed
	if (getBridge()) return true;
	// Not in a browser environment (SSR)
	if (typeof window === 'undefined') return false;

	const start = Date.now();
	while (Date.now() - start < maxWaitMs) {
		if ((window as any).ue?.bridge) return true;
		await new Promise((r) => setTimeout(r, 50));
	}
	console.warn('Bridge not available after', maxWaitMs, 'ms — running in standalone mode');
	return false;
}

/** Safely parse a bridge result - UE wraps returns in { ReturnValue: "json string" } */
function parseResult<T>(value: unknown): T {
	// UE bridge wraps UFUNCTION returns in { ReturnValue: ... }
	const raw = (value && typeof value === 'object' && 'ReturnValue' in (value as any))
		? (value as any).ReturnValue
		: value;
	if (typeof raw === 'string') {
		return JSON.parse(raw);
	}
	return raw as T;
}

export function isInUnreal(): boolean {
	return getBridge() !== null || currentTransport === 'remote';
}

/** Get the last used agent name (persisted across editor sessions) */
export async function getLastUsedAgent(): Promise<string> {
	if (currentTransport === 'remote') return relayCall<string>('getLastUsedAgent');
	const bridge = getBridge();
	if (bridge) {
		const result = await bridge.getlastusedagent();
		const raw = (result && typeof result === 'object' && 'ReturnValue' in result)
			? result.ReturnValue
			: result;
		return (raw as string) || '';
	}
	return '';
}

// ── Onboarding ──────────────────────────────────────────────────────

/** Check if the onboarding wizard has been completed or skipped */
export async function getOnboardingCompleted(): Promise<boolean> {
	const bridge = getBridge();
	if (bridge) {
		const result = await bridge.getonboardingcompleted();
		const raw = (result && typeof result === 'object' && 'ReturnValue' in result)
			? result.ReturnValue
			: result;
		return !!raw;
	}
	return true; // In dev mode (no UE), skip wizard
}

/** Mark onboarding as completed. Persists across editor sessions. */
export async function setOnboardingCompleted(): Promise<void> {
	const bridge = getBridge();
	if (bridge) {
		await bridge.setonboardingcompleted();
	}
}

/** OS-level user language tag (e.g. "en-US", "fr-FR", "pt-BR"). Empty in standalone mode. */
export async function getDefaultLanguage(): Promise<string> {
	const bridge = getBridge();
	if (bridge) {
		const result = await bridge.getdefaultlanguage();
		const raw = (result && typeof result === 'object' && 'ReturnValue' in result)
			? (result as any).ReturnValue
			: result;
		return (raw as string) || '';
	}
	return '';
}

export type SystemFontInfo = {
	family: string;
	isMonospace?: boolean;
};

export type SystemFontsResult = {
	fonts: SystemFontInfo[];
	platform?: 'mac' | 'windows' | 'linux' | 'unknown';
};

/** Installed OS fonts as seen by the embedded UE bridge. Empty in standalone mode. */
export async function listSystemFonts(): Promise<SystemFontsResult> {
	const bridge = getBridge();
	if (bridge?.listsystemfonts) {
		const result = await bridge.listsystemfonts();
		const parsed = parseResult<Partial<SystemFontsResult>>(result);
		return {
			fonts: Array.isArray(parsed?.fonts) ? parsed.fonts : [],
			platform: parsed?.platform
		};
	}
	return { fonts: [], platform: 'unknown' };
}

/** Whether the user has confirmed a UI language. Independent of onboarding. */
export async function getLanguageOnboardingCompleted(): Promise<boolean> {
	const bridge = getBridge();
	if (bridge) {
		const result = await bridge.getlanguageonboardingcompleted();
		const raw = (result && typeof result === 'object' && 'ReturnValue' in result)
			? (result as any).ReturnValue
			: result;
		return !!raw;
	}
	return true; // Skip in dev mode
}

/** Mark the language step as done. Persists across editor sessions. */
export async function setLanguageOnboardingCompleted(): Promise<void> {
	const bridge = getBridge();
	if (bridge) {
		await bridge.setlanguageonboardingcompleted();
	}
}

// ── Entitlement ─────────────────────────────────────────────────────

export interface EntitlementStatus {
	entitled: boolean;
	status: 'lifetime' | 'subscription' | 'none' | 'network' | 'unknown';
	isBinaryBuild: boolean;
}

/** Snapshot of the latest entitlement check from the C++ side. Polled by
 *  the upgrade banner so it disappears once the StartupModule check lands. */
export async function getEntitlementStatus(): Promise<EntitlementStatus> {
	const bridge = getBridge();
	if (bridge) {
		const result = await bridge.getentitlementstatus();
		return parseResult(result);
	}
	// Outside UE (browser dev), assume entitled so we don't block the dev loop.
	return { entitled: true, status: 'lifetime', isBinaryBuild: false };
}

// ── NeoStack Cloud account (entitlement/status payload) ───────────────

export type CloudConnectionState =
	| 'disconnected'
	| 'loading'
	| 'connected'
	| 'key_rejected'
	| 'offline';

export type CloudAccountUser = {
	name: string | null;
	email: string | null;
	image: string | null;
};

export type CloudAccountOrganization = {
	id: string;
	name: string | null;
	slug: string | null;
	logo: string | null;
};

export type CloudAccountAccessPlan = {
	planId: string | null;
	planName: string | null;
	requiresPluginEntitlement: boolean;
	allowed: boolean;
	reason: string | null;
};

export type CloudAccountCredits = {
	subscriptionBalanceUsd: number;
	permanentBalanceUsd: number;
	total: number;
};

export type CloudAccountQuota = {
	period: { percent: number };
	burst: { percent: number } | null;
};

export type CloudAccountStatus = {
	hasApiKey: boolean;
	entitled: boolean;
	isBinaryBuild: boolean;
	checkPending: boolean;
	clientStatus: string;
	connectionState: CloudConnectionState;
	connected: boolean;
	status?: string;
	variant?: 'full' | 'binary';
	hasLifetime?: boolean;
	subscriptionActive?: boolean;
	entitledSlugs?: string[];
	user?: CloudAccountUser;
	organization?: CloudAccountOrganization;
	accessPlan?: CloudAccountAccessPlan;
	credits?: CloudAccountCredits;
	quota?: CloudAccountQuota | null;
};

export async function getNeoStackAccountStatus(): Promise<CloudAccountStatus> {
	const bridge = getBridge();
	if (bridge) {
		const result = await bridge.getneostackaccountstatus();
		return parseResult<CloudAccountStatus>(result);
	}
	return {
		hasApiKey: false,
		entitled: true,
		isBinaryBuild: false,
		checkPending: false,
		clientStatus: 'lifetime',
		connectionState: 'disconnected',
		connected: false
	};
}

export async function clearNeoStackCloudKey(): Promise<void> {
	const bridge = getBridge();
	if (bridge) {
		await bridge.clearneostackcloudkey();
	}
}

export function bindNeoStackAccountChanged(callback: (status: CloudAccountStatus) => void): void {
	const bridge = getBridge();
	if (bridge) {
		bridge.bindonneostackaccountchanged((statusJson: string) => {
			try {
				callback(JSON.parse(statusJson) as CloudAccountStatus);
			} catch {
				console.warn('NeoStack account status payload was malformed');
			}
		});
	}
}

// ── Provider Settings ───────────────────────────────────────────────

export type CustomProviderModel = {
	id: string;
	name: string;
	description: string;
};

export type ProviderConfig = {
	id: string;
	name: string;
	description: string;
	requiresApiKey: boolean;
	hasApiKey: boolean;
	apiKeyMasked: string;
	baseUrl: string;
	defaultBaseUrl: string;
	defaultModel: string;
	supportsModelDiscovery: boolean;
	configured: boolean;
	inPriorityList: boolean;
	isUserDefined: boolean;
	enableModelDiscovery: boolean;
	models?: CustomProviderModel[];
};

export type ProviderSettings = {
	priority: string[];
	providers: ProviderConfig[];
};

export type ExtensionRuntimeState =
	| 'disabled'
	| 'registered'
	| 'active'
	| 'unavailable'
	| 'incompatible'
	| 'failed'
	| 'unknown';

export type ExtensionDependency = {
	name: string;
	optional: boolean;
	enabled: boolean;
	installed: boolean;
};

export type ExtensionInfo = {
	pluginName: string;
	extensionId: string;
	displayName: string;
	description: string;
	version: string;
	vendor: string;
	category: string;
	statusMessage: string;
	baseDir: string;
	runtimeState: ExtensionRuntimeState;
	enabledInProject: boolean;
	hasExplicitProjectEntry: boolean;
	loadedInSession: boolean;
	mountedInSession: boolean;
	activeInSession: boolean;
	restartRequired: boolean;
	canToggle: boolean;
	isProjectPlugin: boolean;
	isInstalledOnEngine: boolean;
	explicitlyLoaded: boolean;
	enabledByDefault: boolean;
	isBetaVersion: boolean;
	isExperimentalVersion: boolean;
	isBuiltIn: boolean;
	isThirdParty: boolean;
	hasRuntimeDescriptor: boolean;
	hasUIMetadata: boolean;
	domain: string;
	domainLabel: string;
	sortOrder: number;
	agentSummary: string;
	enablesAgentTo: string[];
	whenToEnable: string;
	isRecommended: boolean;
	dependencies: ExtensionDependency[];
};

export type ExtensionSettingsState = {
	coreApiVersion: number;
	projectFile: string;
	restartRequired: boolean;
	extensions: ExtensionInfo[];
};

export type ExtensionToggleResult = {
	success: boolean;
	pluginName: string;
	enabledInProject: boolean;
	restartRequired: boolean;
	error?: string;
};

export async function getProviderSettings(): Promise<ProviderSettings> {
	const bridge = getBridge();
	if (bridge) {
		const result = await bridge.getprovidersettings();
		return parseResult(result);
	}
	return { priority: [], providers: [] };
}

export async function setProviderPriority(priority: string[]): Promise<void> {
	const bridge = getBridge();
	if (bridge) {
		await bridge.setproviderpriority(JSON.stringify(priority));
	}
}

export async function addProvider(providerId: string): Promise<void> {
	const bridge = getBridge();
	if (bridge) {
		await bridge.addprovider(providerId);
	}
}

export async function removeProvider(providerId: string): Promise<void> {
	const bridge = getBridge();
	if (bridge) {
		await bridge.removeprovider(providerId);
	}
}

export async function setProviderApiKey(providerId: string, apiKey: string): Promise<void> {
	const bridge = getBridge();
	if (bridge) {
		await bridge.setproviderapikey(providerId, apiKey);
	}
}

export async function setProviderBaseUrl(providerId: string, baseUrl: string): Promise<void> {
	const bridge = getBridge();
	if (bridge) {
		await bridge.setproviderbaseurl(providerId, baseUrl);
	}
}

export async function refreshProviderModels(): Promise<void> {
	const bridge = getBridge();
	if (bridge) {
		await bridge.refreshprovidermodels();
	}
}

// ── Custom Providers ────────────────────────────────────────────────

export async function createCustomProvider(displayName: string, baseUrl: string): Promise<{ providerId: string }> {
	const bridge = getBridge();
	if (bridge) {
		const result = await bridge.createcustomprovider(displayName, baseUrl);
		return parseResult(result);
	}
	return { providerId: '' };
}

export async function deleteCustomProvider(providerId: string): Promise<void> {
	const bridge = getBridge();
	if (bridge) {
		await bridge.deletecustomprovider(providerId);
	}
}

export async function updateCustomProvider(providerId: string, displayName: string, baseUrl: string): Promise<void> {
	const bridge = getBridge();
	if (bridge) {
		await bridge.updatecustomprovider(providerId, displayName, baseUrl);
	}
}

export async function addCustomProviderModel(providerId: string, modelId: string, displayName: string, description: string): Promise<{ success: boolean }> {
	const bridge = getBridge();
	if (bridge) {
		const result = await bridge.addcustomprovidermodel(providerId, modelId, displayName, description);
		return parseResult(result);
	}
	return { success: false };
}

export async function removeCustomProviderModel(providerId: string, modelId: string): Promise<void> {
	const bridge = getBridge();
	if (bridge) {
		await bridge.removecustomprovidermodel(providerId, modelId);
	}
}

export async function importCustomProviderModels(providerId: string, modelsJson: string): Promise<{ imported: number; errors: string[] }> {
	const bridge = getBridge();
	if (bridge) {
		const result = await bridge.importcustomprovidermodels(providerId, modelsJson);
		return parseResult(result);
	}
	return { imported: 0, errors: ['Bridge not available'] };
}

export async function setCustomProviderModelDiscovery(providerId: string, enabled: boolean): Promise<void> {
	const bridge = getBridge();
	if (bridge) {
		await bridge.setcustomprovidermodeldiscovery(providerId, enabled);
	}
}

export async function setCustomProviderRequiresApiKey(providerId: string, requiresApiKey: boolean): Promise<void> {
	const bridge = getBridge();
	if (bridge) {
		await bridge.setcustomproviderrequiresapikey(providerId, requiresApiKey);
	}
}

export type EnabledModelsState = {
	enabledModels: string[];
	hasCustomSelection: boolean;
};

export async function getEnabledModels(): Promise<EnabledModelsState> {
	const bridge = getBridge();
	if (bridge) {
		const result = await bridge.getenabledmodels();
		return parseResult(result);
	}
	return { enabledModels: [], hasCustomSelection: false };
}

export async function setModelEnabled(modelId: string, enabled: boolean): Promise<void> {
	const bridge = getBridge();
	if (bridge) {
		await bridge.setmodelenabled(modelId, enabled);
	}
}

export async function setEnabledModels(modelIds: string[]): Promise<void> {
	const bridge = getBridge();
	if (bridge) {
		await bridge.setenabledmodels(JSON.stringify(modelIds));
	}
}

// ── Extension Settings ──────────────────────────────────────────────

export async function getExtensionSettings(): Promise<ExtensionSettingsState> {
	const bridge = getBridge();
	if (bridge) {
		const result = await bridge.getextensionsettings();
		return parseResult(result);
	}
	return { coreApiVersion: 0, projectFile: '', restartRequired: false, extensions: [] };
}

export async function setExtensionEnabled(
	pluginName: string,
	enabled: boolean
): Promise<ExtensionToggleResult> {
	const bridge = getBridge();
	if (bridge) {
		const result = await bridge.setextensionenabled(pluginName, enabled);
		return parseResult(result);
	}
	return {
		success: false,
		pluginName,
		enabledInProject: enabled,
		restartRequired: false,
		error: 'Bridge not available'
	};
}

export type ExtensionBulkToggleResult = {
	success: boolean;
	count: number;
	restartRequired: boolean;
	error?: string;
};

// Flip the extension and its missing required deps in one .uproject save.
// Used by the "Enable required" button so a single restart picks up the lot.
export async function setExtensionPluginsEnabled(
	pluginNames: string[],
	enabled: boolean
): Promise<ExtensionBulkToggleResult> {
	const bridge = getBridge();
	if (bridge) {
		const result = await bridge.setextensionpluginsenabled(JSON.stringify(pluginNames), enabled);
		return parseResult(result);
	}
	return {
		success: false,
		count: 0,
		restartRequired: false,
		error: 'Bridge not available'
	};
}

// ── Extension Catalog (NeoStack Cloud) ─────────────────────────

export type ExtensionCatalogChannel = 'stable' | 'beta' | 'dev' | 'alpha';

export type CatalogExtension = {
	slug: string;
	pluginName: string;
	name: string;
	description: string;
	latestVersion: string;
	latestChannel: ExtensionCatalogChannel | '';
	publishedAt: string;
	changelog: string;
	domain?: string;
	domainLabel?: string;
	sortOrder?: number;
	agentSummary?: string;
	enablesAgentTo?: string[];
	whenToEnable?: string;
	isRecommended?: boolean;
	supportedEngineVersions: string[];
};

export type CatalogStatus = 'idle' | 'fetching' | 'ready' | 'error';

export type ExtensionCatalogState = {
	status: CatalogStatus;
	success: boolean;
	httpStatus: number;
	channel: string;
	engine: string;
	fetchedAt?: string;
	error?: string;
	extensions: CatalogExtension[];
};

const EMPTY_CATALOG: ExtensionCatalogState = {
	status: 'idle',
	success: false,
	httpStatus: 0,
	channel: '',
	engine: '',
	extensions: []
};

export async function refreshExtensionCatalog(
	channel: ExtensionCatalogChannel,
	includeAllEngines: boolean
): Promise<void> {
	const bridge = getBridge();
	if (bridge) {
		await bridge.refreshextensioncatalog(channel, includeAllEngines);
	}
}

export async function getExtensionCatalog(): Promise<ExtensionCatalogState> {
	const bridge = getBridge();
	if (bridge) {
		const result = await bridge.getextensioncatalog();
		return parseResult(result);
	}
	return EMPTY_CATALOG;
}

/** Convenience helper: kicks off a refresh and polls until status flips to
 *  ready / error, or the timeout elapses. The Svelte panel uses this so
 *  callers don't hand-roll the poll loop. */
export async function fetchExtensionCatalog(
	channel: ExtensionCatalogChannel,
	includeAllEngines: boolean,
	timeoutMs = 15000,
	pollMs = 250
): Promise<ExtensionCatalogState> {
	await refreshExtensionCatalog(channel, includeAllEngines);
	const start = Date.now();
	while (Date.now() - start < timeoutMs) {
		const snapshot = await getExtensionCatalog();
		if (snapshot.status === 'ready' || snapshot.status === 'error') {
			return snapshot;
		}
		await new Promise((resolve) => setTimeout(resolve, pollMs));
	}
	return { ...EMPTY_CATALOG, status: 'error', error: 'Catalog fetch timed out' };
}

// ── Extension Installer (batch install / update / uninstall) ───

export type ExtensionOpKind = 'install' | 'update' | 'uninstall';

export type ExtensionOpPhase =
	| 'queued'
	| 'resolving_download'
	| 'downloading'
	| 'verifying'
	| 'extracting'
	| 'installing'
	| 'updating_project'
	| 'uninstalling'
	| 'pending_restart'
	| 'success'
	| 'failed';

export type ExtensionOp = {
	slug: string;
	pluginName: string;
	kind: ExtensionOpKind;
	phase: ExtensionOpPhase;
	channel: string;
	engine: string;
	platform: string;
	error?: string;
	bytesTotal?: number;
	bytesDone?: number;
	resolvedVersion?: string;
	resolvedFileName?: string;
	stagedPluginRoot?: string;
};

export type ExtensionOpQueueState = {
	count: number;
	queue: ExtensionOp[];
};

export type ExtensionOpRunState = {
	running: boolean;
	restartRecommended: boolean;
	succeeded: number;
	failed: number;
	startedAt?: string;
	completedAt?: string;
	ops: ExtensionOp[];
};

const EMPTY_QUEUE: ExtensionOpQueueState = { count: 0, queue: [] };
const EMPTY_RUN: ExtensionOpRunState = {
	running: false,
	restartRecommended: false,
	succeeded: 0,
	failed: 0,
	ops: []
};

export async function queueExtensionOp(
	slug: string,
	pluginName: string,
	kind: ExtensionOpKind,
	channel: string
): Promise<void> {
	const bridge = getBridge();
	if (bridge) {
		await bridge.queueextensionop(slug, pluginName, kind, channel);
	}
}

export async function dequeueExtensionOp(slug: string, kind: ExtensionOpKind): Promise<void> {
	const bridge = getBridge();
	if (bridge) {
		await bridge.dequeueextensionop(slug, kind);
	}
}

export async function clearExtensionOpQueue(): Promise<void> {
	const bridge = getBridge();
	if (bridge) {
		await bridge.clearextensionopqueue();
	}
}

export async function getExtensionOpQueue(): Promise<ExtensionOpQueueState> {
	const bridge = getBridge();
	if (bridge) {
		const result = await bridge.getextensionopqueue();
		return parseResult(result);
	}
	return EMPTY_QUEUE;
}

export async function applyExtensionOps(): Promise<void> {
	const bridge = getBridge();
	if (bridge) {
		await bridge.applyextensionops();
	}
}

export async function getExtensionOpState(): Promise<ExtensionOpRunState> {
	const bridge = getBridge();
	if (bridge) {
		const result = await bridge.getextensionopstate();
		return parseResult(result);
	}
	return EMPTY_RUN;
}

// ── Agent Skills ────────────────────────────────────────────────

export type SkillStatus = {
	name: string;
	description: string;
	sourceId: string;
	sourceDisplayName: string;
	sourceVersion: string;
	tags: string[];
	installedPaths: string[];
	userEdited: boolean;
	conflictPending: boolean;
	conflictNewPath: string;
};

export type ProjectSkillStatus = {
	name: string;
	folderName: string;
	description: string;
	tags: string[];
	paths: string[];
	parseError: string;
};

export type SkillsState = {
	projectDir: string;
	manifestPath: string;
	skills: SkillStatus[];
	projectSkills: ProjectSkillStatus[];
};

export type SkillBody = {
	success: boolean;
	name: string;
	body?: string;
	error?: string;
};

export type SkillSyncReport = {
	installed: number;
	updated: number;
	noOp: number;
	userEditsKept: number;
	conflicts: number;
	orphansRemoved: number;
	orphansKept: number;
	errors: string[];
};

export type SkillConflictMode = 'keep-user' | 'take-new';

const EMPTY_SKILLS: SkillsState = { projectDir: '', manifestPath: '', skills: [], projectSkills: [] };

export async function getSkills(): Promise<SkillsState> {
	const bridge = getBridge();
	if (bridge) {
		const result = await bridge.getskills();
		const parsed = parseResult(result) as SkillsState;
		if (!parsed.projectSkills) {
			parsed.projectSkills = [];
		}
		return parsed;
	}
	return EMPTY_SKILLS;
}

export async function getSkillBody(name: string): Promise<SkillBody> {
	const bridge = getBridge();
	if (bridge) {
		const result = await bridge.getskillbody(name);
		return parseResult(result);
	}
	return { success: false, name, error: 'Bridge not available' };
}

export async function getSkillConflictBody(name: string): Promise<SkillBody> {
	const bridge = getBridge();
	if (bridge) {
		const result = await bridge.getskillconflictbody(name);
		return parseResult(result);
	}
	return { success: false, name, error: 'Bridge not available' };
}

export async function resolveSkillConflict(
	name: string,
	mode: SkillConflictMode
): Promise<{ success: boolean; error?: string }> {
	const bridge = getBridge();
	if (bridge) {
		const result = await bridge.resolveskillconflict(name, mode);
		return parseResult(result);
	}
	return { success: false, error: 'Bridge not available' };
}

export async function rescanSkills(): Promise<SkillSyncReport> {
	const bridge = getBridge();
	if (bridge) {
		const result = await bridge.rescanskills();
		return parseResult(result);
	}
	return {
		installed: 0,
		updated: 0,
		noOp: 0,
		userEditsKept: 0,
		conflicts: 0,
		orphansRemoved: 0,
		orphansKept: 0,
		errors: []
	};
}

export async function openSkillFile(name: string, upstream: boolean): Promise<void> {
	const bridge = getBridge();
	if (bridge) {
		await bridge.openskillfile(name, upstream);
	}
}

// ── Notification Settings ──────────────────────────────────────

export type NotificationSettings = {
	onlyWhenUnfocused: boolean;
	notifyOnComplete: boolean;
	flashTaskbar: boolean;
	playSound: boolean;
	soundVolume: number;
	completionSound: string;
	errorSound: string;
	playPermissionSound: boolean;
	permissionSoundVolume: number;
	permissionRequestSound: string;
};

const defaultNotificationSettings: NotificationSettings = {
	onlyWhenUnfocused: false,
	notifyOnComplete: true,
	flashTaskbar: true,
	playSound: true,
	soundVolume: 1.0,
	completionSound: '',
	errorSound: '',
	playPermissionSound: false,
	permissionSoundVolume: 1.0,
	permissionRequestSound: ''
};

export async function getNotificationSettings(): Promise<NotificationSettings> {
	const bridge = getBridge();
	if (bridge) {
		const result = await bridge.getnotificationsettings();
		const parsed = parseResult<Partial<NotificationSettings>>(result);
		return { ...defaultNotificationSettings, ...parsed };
	}
	return { ...defaultNotificationSettings };
}

export async function setNotificationSetting(key: string, value: string): Promise<void> {
	const bridge = getBridge();
	if (bridge) {
		await bridge.setnotificationsetting(key, value);
	}
}

export type SoundAsset = {
	/** FSoftObjectPath string, e.g. "/Game/Sounds/MySound.MySound" */
	path: string;
	/** Bare asset name, e.g. "MySound" */
	name: string;
	/** Package path (folder), e.g. "/Game/Sounds" */
	folder: string;
	/** Class name, e.g. "SoundWave" or "SoundCue" */
	className: string;
};

export async function listSoundAssets(query: string = ''): Promise<SoundAsset[]> {
	const bridge = getBridge();
	if (bridge) {
		const result = await bridge.listsoundassets(query);
		const parsed = parseResult<{ sounds?: SoundAsset[] }>(result);
		return parsed?.sounds ?? [];
	}
	return [];
}

export async function previewNotificationSound(soundPath: string, volume: number = 1.0): Promise<void> {
	const bridge = getBridge();
	if (bridge) {
		await bridge.previewnotificationsound(soundPath, volume);
	}
}

export async function soundAssetExists(soundPath: string): Promise<boolean> {
	if (!soundPath) return false;
	const bridge = getBridge();
	if (bridge) {
		const result = await bridge.soundassetexists(soundPath);
		const raw = (result && typeof result === 'object' && 'ReturnValue' in result)
			? result.ReturnValue
			: result;
		return !!raw;
	}
	return true; // dev mode (no UE) — assume valid so picker doesn't show false-positive "missing"
}

// ── Agent Execution Settings ────────────────────────────────────────

export type AgentExecutionSettings = {
	systemPromptAppend: string;
	toolTimeout: number;
	agentResponseTimeout: number;
};

export async function getAgentExecutionSettings(): Promise<AgentExecutionSettings> {
	const bridge = getBridge();
	if (bridge) {
		const result = await bridge.getagentexecutionsettings();
		const parsed = parseResult<AgentExecutionSettings>(result);
		return {
			systemPromptAppend: parsed?.systemPromptAppend ?? '',
			toolTimeout: parsed?.toolTimeout ?? 60,
			agentResponseTimeout: parsed?.agentResponseTimeout ?? 0
		};
	}
	return { systemPromptAppend: '', toolTimeout: 60, agentResponseTimeout: 0 };
}

export async function setAgentExecutionSetting(key: 'systemPromptAppend' | 'toolTimeout' | 'agentResponseTimeout', value: string): Promise<void> {
	const bridge = getBridge();
	if (bridge) {
		await bridge.setagentexecutionsetting(key, value);
	}
}

// ── Issue Report Settings ───────────────────────────────────────────

export type IssueReportSettings = {
	disabled: boolean;
};

export async function getIssueReportSettings(): Promise<IssueReportSettings> {
	const bridge = getBridge();
	if (bridge) {
		const result = await bridge.getissuereportsettings();
		const parsed = parseResult<IssueReportSettings>(result);
		return parsed ?? { disabled: false };
	}
	return { disabled: false };
}

export async function setIssueReportDisabled(disabled: boolean): Promise<void> {
	const bridge = getBridge();
	if (bridge) {
		await bridge.setissuereportdisabled(disabled);
	}
}

// ── AI Generation Settings ──────────────────────────────────────────

export type GenerationSettings = {
	imageModel: string;
	meshyArtStyle: string;
	meshyApiKey: string;
	tripoApiKey: string;
	elevenLabsApiKey: string;
	falApiKey: string;
	openAIApiKey: string;
};

export type GenerationSettingKey = keyof GenerationSettings;

export async function getGenerationSettings(): Promise<GenerationSettings> {
	const bridge = getBridge();
	if (bridge) {
		const result = await bridge.getgenerationsettings();
		const parsed = parseResult<GenerationSettings>(result);
		if (parsed) return parsed;
	}
	return {
		imageModel: '',
		meshyArtStyle: 'realistic',
		meshyApiKey: '',
		tripoApiKey: '',
		elevenLabsApiKey: '',
		falApiKey: '',
		openAIApiKey: ''
	};
}

export async function setGenerationSetting(key: GenerationSettingKey, value: string): Promise<void> {
	const bridge = getBridge();
	if (bridge) {
		await bridge.setgenerationsetting(key, value);
	}
}

// ── Agent Discovery ─────────────────────────────────────────────────

/** Get list of available agents from the backend */
export async function getAgents(): Promise<AgentInfo[]> {
	if (currentTransport === 'remote') return relayCall<AgentInfo[]>('getAgents');
	const bridge = getBridge();
	if (bridge) {
		const result = await bridge.getagents();
		return parseResult<AgentInfo[]>(result);
	}
	return [];
}

/** Create a new chat session */
export async function createSession(agentName: string): Promise<{ sessionId: string; agentName: string }> {
	if (currentTransport === 'remote') return relayCall('createSession', agentName);
	const bridge = getBridge();
	if (bridge) {
		const result = await bridge.createsession(agentName);
		return parseResult(result);
	}
	return { sessionId: createUUID(), agentName };
}

/** Get all sessions (saved + active) */
export async function getSessions(): Promise<SessionInfo[]> {
	if (currentTransport === 'remote') return relayCall<SessionInfo[]>('getSessions');
	const bridge = getBridge();
	if (bridge) {
		const result = await bridge.getsessions();
		return parseResult(result);
	}
	return [];
}

/** Resume a saved session — loads from disk, connects agent, resumes external session */
export async function resumeSession(sessionId: string): Promise<{ success: boolean; agentName?: string; error?: string }> {
	const bridge = getBridge();
	if (bridge) {
		const result = await bridge.resumesession(sessionId);
		return parseResult(result);
	}
	return { success: false, error: 'Not in UE' };
}

export type SessionTerminalResumeResult = {
	supported: boolean;
	command?: string;
	agentName?: string;
	registryId?: string;
	error?: string;
};

/** Shell command to resume this chat in the embedded terminal (Claude Code, Gemini, Copilot, Codex). */
export async function getSessionTerminalResumeCommand(
	sessionId: string
): Promise<SessionTerminalResumeResult> {
	const bridge = getBridge();
	if (bridge) {
		const result = await bridge.getsessionterminalresumecommand(sessionId);
		return parseResult(result);
	}
	return { supported: false, error: 'Not in UE' };
}

/** Get messages for a session */
export async function getSessionMessages(sessionId: string): Promise<ChatMessage[]> {
	if (currentTransport === 'remote') return relayCall<ChatMessage[]>('getSessionMessages', sessionId);
	const bridge = getBridge();
	if (bridge) {
		const result = await bridge.getsessionmessages(sessionId);
		return parseResult(result);
	}
	return [];
}

/** Rename a session (sets custom title that survives remote sync) */
export async function renameSession(sessionId: string, newTitle: string): Promise<{ success: boolean }> {
	const bridge = getBridge();
	if (bridge) {
		const result = await bridge.renamesession(sessionId, newTitle);
		return parseResult(result);
	}
	return { success: false };
}

/** Delete a session */
export async function deleteSession(sessionId: string): Promise<{ success: boolean }> {
	const bridge = getBridge();
	if (bridge) {
		const result = await bridge.deletesession(sessionId);
		return parseResult(result);
	}
	return { success: false };
}

/** Export a loaded session to a Markdown file via native save dialog */
export async function exportSessionToMarkdown(sessionId: string): Promise<ExportSessionResult> {
	const bridge = getBridge();
	if (bridge) {
		const result = await bridge.exportsessiontomarkdown(sessionId);
		return parseResult(result);
	}
	return { success: false, error: 'Not in UE' };
}

/** Send a prompt to a session */
export async function sendPrompt(sessionId: string, text: string): Promise<void> {
	if (currentTransport === 'remote') {
		await relayCall('sendPrompt', sessionId, text);
		return;
	}
	const bridge = getBridge();
	if (!bridge) {
		throw new Error('UE bridge unavailable');
	}
	await bridge.sendprompt(sessionId, text);
}

/** Cancel current prompt in a session */
export async function cancelPrompt(sessionId: string): Promise<void> {
	if (currentTransport === 'remote') {
		await relayCall('cancelPrompt', sessionId);
		return;
	}
	const bridge = getBridge();
	if (bridge) {
		await bridge.cancelprompt(sessionId);
	}
}

// ── Agent Setup ─────────────────────────────────────────────────────

export type AgentInstallInfo = {
	agentName: string;
	baseExecutableName: string;
	installCommand: string;
	installUrl: string;
	requiresAdapter: boolean;
	requiresBaseCLI: boolean;
};

/** Get install info for an agent (install command, download URL, requirements) */
export async function getAgentInstallInfo(agentName: string): Promise<AgentInstallInfo> {
	const bridge = getBridge();
	if (bridge) {
		const result = await bridge.getagentinstallinfo(agentName);
		return parseResult(result);
	}
	return { agentName, baseExecutableName: '', installCommand: '', installUrl: '', requiresAdapter: false, requiresBaseCLI: false };
}

/** Start async agent installation. Listen for progress via onInstallProgress/onInstallComplete. */
export async function installAgent(agentName: string): Promise<void> {
	const bridge = getBridge();
	if (bridge) {
		await bridge.installagent(agentName);
	}
}

/** Register callback for install progress updates */
export function onInstallProgress(callback: (agentName: string, message: string) => void): void {
	const bridge = getBridge();
	if (bridge) {
		bridge.bindoninstallprogress(callback);
	}
}

/** Register callback for install completion */
export function onInstallComplete(callback: (agentName: string, success: boolean, errorMessage: string) => void): void {
	const bridge = getBridge();
	if (bridge) {
		bridge.bindoninstallcomplete(callback);
	}
}

/** Refresh an agent's status (invalidates cache, re-checks). Returns updated status. */
export async function refreshAgentStatus(agentName: string): Promise<{ status: AgentStatus; statusMessage: string }> {
	const bridge = getBridge();
	if (bridge) {
		const result = await bridge.refreshagentstatus(agentName);
		return parseResult(result);
	}
	return { status: 'unknown', statusMessage: '' };
}

// ── ACP Registry ────────────────────────────────────────────────────

export type RegistryAgent = {
	id: string;
	name: string;
	version: string;
	description: string;
	license: string;
	icon: string; // SVG markup (pre-fetched by C++ backend, supports currentColor)
	repository: string;
	authors: string[];
	hasBinary: boolean;
	hasNpx: boolean;
	hasUvx: boolean;
	npxPackage?: string;
	uvxPackage?: string;
	// Install status
	isInstalled: boolean;
	installedVersion?: string;
	latestVersion?: string;
	updateAvailable?: boolean;
	installMethod: string; // "binary" | "npx" | "uvx" | ""
};

/** Get all agents from the ACP registry (cached, platform-filtered) */
export async function getRegistryAgents(): Promise<RegistryAgent[]> {
	const bridge = getBridge();
	if (bridge) {
		const result = await bridge.getregistryagents();
		return parseResult<RegistryAgent[]>(result);
	}
	return [];
}

/** Force refresh the ACP registry from the CDN */
export async function refreshRegistry(): Promise<void> {
	const bridge = getBridge();
	if (bridge) {
		await bridge.refreshregistry();
	}
}

/** Install a registry agent. Method: "binary" | "npx" | "uvx" | "auto" */
export async function installRegistryAgent(agentId: string, method: string = 'auto'): Promise<void> {
	const bridge = getBridge();
	if (bridge) {
		await bridge.installregistryagent(agentId, method);
	}
}

/** Uninstall a registry agent (removes downloaded binaries) */
export async function uninstallRegistryAgent(agentId: string): Promise<void> {
	const bridge = getBridge();
	if (bridge) {
		await bridge.uninstallregistryagent(agentId);
	}
}

/** Agent update info */
export type AgentUpdateInfo = {
	agentId: string;
	agentName: string;
	installedVersion: string;
	latestVersion: string;
	isNpx: boolean;
};

/** Get list of installed agents that have updates available */
export async function getAgentUpdates(): Promise<AgentUpdateInfo[]> {
	const bridge = getBridge();
	if (bridge) {
		const result = await bridge.getagentupdates();
		return parseResult<AgentUpdateInfo[]>(result);
	}
	return [];
}

/** Trigger update for a binary agent (removes old version, downloads new on next use) */
export async function updateRegistryAgent(agentId: string): Promise<void> {
	const bridge = getBridge();
	if (bridge) {
		await bridge.updateregistryagent(agentId);
	}
}

// ── Prerequisites ───────────────────────────────────────────────────

export type PrerequisiteTool = {
	found: boolean;
	path: string;
	version?: string;
};

export type PrerequisiteStatus = {
	node: PrerequisiteTool;
	npm: PrerequisiteTool;
	npx: PrerequisiteTool;
	git: PrerequisiteTool;
	uv: PrerequisiteTool;
	uvx: PrerequisiteTool;
	bun: PrerequisiteTool;
};

/** Check which prerequisite tools are installed on the system */
export async function getPrerequisiteStatus(): Promise<PrerequisiteStatus> {
	const bridge = getBridge();
	if (bridge) {
		const result = await bridge.getprerequisitestatus();
		return parseResult<PrerequisiteStatus>(result);
	}
	const empty: PrerequisiteTool = { found: false, path: '' };
	return { node: empty, npm: empty, npx: empty, git: empty, uv: empty, uvx: empty, bun: empty };
}

export type McpConnectionInfo = {
	serverName: string;
	port: number;
	isRunning: boolean;
	recommendedUrl: string;
	localhostUrl: string;
	legacySseUrl: string;
	legacyMessageUrl: string;
	transport: 'streamable_http' | string;
};

/** Get the live MCP server endpoints for local client configuration. */
export async function getMcpConnectionInfo(): Promise<McpConnectionInfo> {
	const bridge = getBridge();
	if (bridge) {
		const result = await bridge.getmcpconnectioninfo();
		return parseResult<McpConnectionInfo>(result);
	}
	return {
		serverName: 'unreal-editor',
		port: 9315,
		isRunning: false,
		recommendedUrl: 'http://127.0.0.1:9315/mcp',
		localhostUrl: 'http://localhost:9315/mcp',
		legacySseUrl: 'http://127.0.0.1:9315/sse',
		legacyMessageUrl: 'http://127.0.0.1:9315/message',
		transport: 'streamable_http'
	};
}

/** Copy text to system clipboard */
export async function copyToClipboard(text: string): Promise<void> {
	const bridge = getBridge();
	if (bridge) {
		await bridge.copytoclipboard(text);
	}
}

/** Read text from system clipboard */
export async function getClipboardText(): Promise<string> {
	const bridge = getBridge();
	if (bridge) {
		const result = await bridge.getclipboardtext();
		const raw = (result && typeof result === 'object' && 'ReturnValue' in result)
			? result.ReturnValue
			: result;
		return (raw as string) || '';
	}
	return '';
}

/** Open URL in system browser */
export async function openUrl(url: string): Promise<void> {
	const bridge = getBridge();
	if (bridge) {
		await bridge.openurl(url);
	}
}

/** Open an asset or source file in UE. Handles /Game/ paths, filesystem paths, file:line format. */
export async function openPath(path: string, line: number = 0): Promise<void> {
	const bridge = getBridge();
	if (!bridge) {
		console.warn('[AIK] openPath: bridge not available');
		return;
	}
	if (typeof bridge.openpath !== 'function') {
		console.warn('[AIK] openPath: bridge.openpath is not a function, available methods:', Object.keys(bridge));
		return;
	}
	try {
		await bridge.openpath(path, line);
	} catch (e) {
		console.error('[AIK] openPath failed:', e);
	}
}

/** Open the plugin settings panel in UE Project Settings */
export async function openPluginSettings(): Promise<void> {
	const bridge = getBridge();
	if (bridge) {
		await bridge.openpluginsettings();
	}
}

/** Restart Unreal Editor (prompts to save unsaved work) */
export async function restartEditor(): Promise<void> {
	const bridge = getBridge();
	if (bridge) {
		await bridge.restarteditor();
	}
}

/** Trigger an async plugin update check in UE */
export async function checkForPluginUpdate(): Promise<void> {
	const bridge = getBridge();
	if (bridge) {
		await bridge.checkforpluginupdate();
	}
}

// ── Model & Reasoning ───────────────────────────────────────────────

export type ModelInfo = {
	id: string;
	name: string;
	description: string;
	supportsReasoning: boolean;
	provider?: string;
	providerDisplayName?: string;
};

export type ModelState = {
	models: ModelInfo[];
	currentModelId: string;
};

/** Get available models for an agent */
export async function getModels(agentName: string): Promise<ModelState> {
	if (currentTransport === 'remote') return relayCall<ModelState>('getModels', agentName);
	const bridge = getBridge();
	if (bridge) {
		const result = await bridge.getmodels(agentName);
		return parseResult(result);
	}
	return { models: [], currentModelId: '' };
}

/** Get full model list for an agent when the backend supports it. */
export async function getAllModels(agentName: string): Promise<ModelState> {
	const bridge = getBridge();
	if (bridge) {
		const result = await bridge.getallmodels(agentName);
		return parseResult(result);
	}
	return { models: [], currentModelId: '' };
}

/** Set the active model for an agent */
export async function setModel(agentName: string, modelId: string): Promise<void> {
	if (currentTransport === 'remote') { await relayCall('setModel', agentName, modelId); return; }
	const bridge = getBridge();
	if (bridge) {
		await bridge.setmodel(agentName, modelId);
	}
}

/** Get current reasoning effort level for an agent */
export async function getReasoningLevel(agentName: string): Promise<string> {
	const bridge = getBridge();
	if (bridge) {
		const result = await bridge.getreasoninglevel(agentName);
		// ReturnValue is a plain string, not JSON
		const raw = (result && typeof result === 'object' && 'ReturnValue' in result)
			? result.ReturnValue
			: result;
		return (raw as string) || 'medium';
	}
	return '';
}

/** Set reasoning effort level for an agent */
export async function setReasoningLevel(agentName: string, level: string): Promise<void> {
	const bridge = getBridge();
	if (bridge) {
		await bridge.setreasoninglevel(agentName, level);
	}
}

/** Register callback for streaming message updates */
export function onMessage(callback: (sessionId: string, update: StreamingUpdate) => void): void {
	if (currentTransport === 'remote') {
		onRelayEvent('onMessage', (data: any) => {
			callback(data.sessionId, data.update);
		});
		return;
	}
	const bridge = getBridge();
	if (bridge) {
		bridge.bindonmessage((sessionId: string, updateJson: string) => {
			const update: StreamingUpdate = JSON.parse(updateJson);
			callback(sessionId, update);
		});
	}
}

/** Register callback for agent state changes */
export function onStateChanged(callback: (sessionId: string, agentName: string, state: string, message: string) => void): void {
	if (currentTransport === 'remote') {
		onRelayEvent('onStateChanged', (data: any) => {
			callback(data.sessionId, data.agentName, String(data.state), data.message);
		});
		return;
	}
	const bridge = getBridge();
	if (bridge) {
		bridge.bindonstatechanged(callback);
	}
}

/** Register callback for MCP tool readiness status: "waiting" | "ready" | "timeout" */
export function onMcpStatus(callback: (sessionId: string, status: string) => void): void {
	const bridge = getBridge();
	if (bridge) {
		bridge.bindonmcpstatus(callback);
	}
}

/** Register callback for session list updates from agents */
export function onSessionListUpdated(callback: (agentName: string, sessions: SessionInfo[]) => void): void {
	const bridge = getBridge();
	if (bridge) {
		bridge.bindonsessionlistupdated((agentName: string, sessionsJson: string) => {
			const sessions: SessionInfo[] = JSON.parse(sessionsJson).map((s: any) => ({
				...s,
				agentName,
				isConnected: false
			}));
			callback(agentName, sessions);
		});
	}
}

/** Manually refresh session lists from all agents. Returns how many agents are being connected. */
export async function refreshSessionList(): Promise<{ connectingCount: number }> {
	const bridge = getBridge();
	if (bridge) {
		const result = await bridge.refreshsessionlist();
		return parseResult(result);
	}
	return { connectingCount: 0 };
}

// ── Permissions ─────────────────────────────────────────────────────

export type PermissionOption = {
	optionId: string;
	name: string;
	kind: 'allow_always' | 'allow_once' | 'reject_once';
};

export type PermissionToolCall = {
	toolCallId: string;
	title: string;
	rawInput: string;
};

export type QuestionOption = {
	label: string;
	description: string;
};

export type Question = {
	question: string;
	header: string;
	options: QuestionOption[];
	multiSelect: boolean;
};

export type PermissionRequest = {
	agentName: string;
	// JSON-RPC id from the agent — string round-tripped to preserve UUID-style ids.
	// (Was `number` historically; coercion lost non-numeric ids → response id:0 → agent dropped reply.)
	requestId: string;
	options: PermissionOption[];
	toolCall: PermissionToolCall;
	isAskUserQuestion: boolean;
	questions: Question[];
};

/** Register callback for permission/consent requests */
export function onPermissionRequest(callback: (sessionId: string, request: PermissionRequest) => void): void {
	if (currentTransport === 'remote') {
		onRelayEvent('onPermissionRequest', (data: any) => {
			callback(data.sessionId, data);
		});
		return;
	}
	const bridge = getBridge();
	if (bridge) {
		bridge.bindonpermissionrequest((sessionId: string, requestJson: string) => {
			const request: PermissionRequest = JSON.parse(requestJson);
			callback(sessionId, request);
		});
	}
}

/** Respond to a permission request */
export async function respondToPermission(
	sessionId: string,
	agentName: string,
	requestId: string,
	optionId: string,
	outcomeMeta?: Record<string, unknown>
): Promise<void> {
	const bridge = getBridge();
	if (bridge) {
		const metaJson = outcomeMeta ? JSON.stringify(outcomeMeta) : '';
		if (bridge.respondtopermissionforsession && sessionId) {
			await bridge.respondtopermissionforsession(sessionId, agentName, requestId, optionId, metaJson);
		} else {
			await bridge.respondtopermission(agentName, requestId, optionId, metaJson);
		}
	}
}

// ── Modes ───────────────────────────────────────────────────────────

export type ModeInfo = {
	id: string;
	name: string;
	description: string;
};

export type ModeState = {
	modes: ModeInfo[];
	currentModeId: string;
};

/** Get available modes for an agent */
export async function getModes(agentName: string): Promise<ModeState> {
	if (currentTransport === 'remote') return relayCall<ModeState>('getModes', agentName);
	const bridge = getBridge();
	if (bridge) {
		const result = await bridge.getmodes(agentName);
		return parseResult(result);
	}
	return { modes: [], currentModeId: '' };
}

/** Set the active mode for an agent */
export async function setMode(agentName: string, modeId: string): Promise<void> {
	if (currentTransport === 'remote') { await relayCall('setMode', agentName, modeId); return; }
	const bridge = getBridge();
	if (bridge) {
		await bridge.setmode(agentName, modeId);
	}
}

/** Register callback for mode availability updates */
export function onModesAvailable(callback: (agentName: string, modeState: ModeState) => void): void {
	if (currentTransport === 'remote') {
		onRelayEvent('onModesAvailable', (data: any) => {
			callback(data.agentName, data);
		});
		return;
	}
	const bridge = getBridge();
	if (bridge) {
		bridge.bindonmodesavailable((agentName: string, modesJson: string) => {
			const modeState: ModeState = JSON.parse(modesJson);
			callback(agentName, modeState);
		});
	}
}

/** Register callback for mode change notifications */
export function onModeChanged(callback: (agentName: string, modeId: string) => void): void {
	const bridge = getBridge();
	if (bridge) {
		bridge.bindonmodechanged(callback);
	}
}

/** Register callback for model availability updates (async push from agents like Codex) */
export function onModelsAvailable(callback: (agentName: string, modelState: ModelState) => void): void {
	if (currentTransport === 'remote') {
		onRelayEvent('onModelsAvailable', (data: any) => {
			callback(data.agentName, data);
		});
		return;
	}
	const bridge = getBridge();
	if (bridge) {
		bridge.bindonmodelsavailable((agentName: string, modelsJson: string) => {
			const modelState: ModelState = JSON.parse(modelsJson);
			callback(agentName, modelState);
		});
	}
}

// ── Slash Commands ──────────────────────────────────────────────────

export type SlashCommand = {
	name: string;
	description: string;
	inputHint: string;
};

/** Register callback for slash commands availability updates */
export function onCommandsAvailable(callback: (sessionId: string, commands: SlashCommand[]) => void): void {
	const bridge = getBridge();
	if (bridge) {
		bridge.bindoncommandsavailable((sessionId: string, commandsJson: string) => {
			const commands: SlashCommand[] = JSON.parse(commandsJson);
			callback(sessionId, commands);
		});
	}
}

// ── Plan/Todo ───────────────────────────────────────────────────────

export type PlanEntry = {
	content: string;
	activeForm: string;
	priority: 'high' | 'medium' | 'low';
	status: 'pending' | 'in_progress' | 'completed';
};

export type PlanUpdate = {
	entries: PlanEntry[];
	completedCount: number;
	totalCount: number;
};

/** Register callback for plan/todo updates */
export function onPlanUpdate(callback: (sessionId: string, plan: PlanUpdate) => void): void {
	if (currentTransport === 'remote') {
		onRelayEvent('onPlanUpdate', (data: any) => {
			callback(data.sessionId, data);
		});
		return;
	}
	const bridge = getBridge();
	if (bridge) {
		bridge.bindonplanupdate((sessionId: string, planJson: string) => {
			const plan: PlanUpdate = JSON.parse(planJson);
			callback(sessionId, plan);
		});
	}
}

// ── Attachments ─────────────────────────────────────────────────────

export type AttachmentInfo = {
	id: string;
	type: 'blueprint_node' | 'blueprint' | 'image' | 'file';
	displayName: string;
	mimeType?: string;
	width?: number;
	height?: number;
	sizeBytes?: number;
	hasExtractedText?: boolean;
	thumbnail?: string;
};

/** Paste image from system clipboard into attachments */
export async function pasteClipboardImage(): Promise<{ success: boolean; error?: string }> {
	const bridge = getBridge();
	if (bridge) {
		const result = await bridge.pasteclipboardimage();
		return parseResult(result);
	}
	return { success: false, error: 'Not in UE' };
}

/** Open native file picker for attachments (images + common docs) */
export async function openImagePicker(): Promise<{ success: boolean; count: number }> {
	const bridge = getBridge();
	if (bridge) {
		const result = await bridge.openimagepicker();
		return parseResult(result);
	}
	return { success: false, count: 0 };
}

/** Add an image from base64 data (for JS-side drag-drop) */
export async function addImageFromBase64(
	base64: string, mimeType: string, width: number, height: number, displayName: string
): Promise<{ success: boolean; attachmentId?: string }> {
	const bridge = getBridge();
	if (bridge) {
		const result = await bridge.addimagefrombase64(base64, mimeType, width, height, displayName);
		return parseResult(result);
	}
	return { success: false };
}

/** Add a generic file from base64 data (for JS-side drag-drop) */
export async function addFileFromBase64(
	base64: string, mimeType: string, displayName: string
): Promise<{ success: boolean; attachmentId?: string }> {
	const bridge = getBridge();
	if (bridge) {
		const result = await bridge.addfilefrombase64(base64, mimeType, displayName);
		return parseResult(result);
	}
	return { success: false };
}

/** Remove an attachment by its GUID */
export async function removeAttachment(id: string): Promise<void> {
	const bridge = getBridge();
	if (bridge) {
		await bridge.removeattachment(id);
	}
}

/** Get current attachments (metadata only) */
export async function getAttachments(): Promise<AttachmentInfo[]> {
	const bridge = getBridge();
	if (bridge) {
		const result = await bridge.getattachments();
		return parseResult(result);
	}
	return [];
}

/** Register callback for attachment list changes */
export function onAttachmentsChanged(callback: (attachments: AttachmentInfo[]) => void): void {
	const bridge = getBridge();
	if (bridge) {
		bridge.bindonattachmentschanged((attachmentsJson: string) => {
			const attachments: AttachmentInfo[] = JSON.parse(attachmentsJson);
			callback(attachments);
		});
	}
}

// ── Context Mentions ────────────────────────────────────────────────

export type ContextItem = {
	name: string;
	path: string;
	category: string;
	type: string;
	icon?: string; // Raw SVG string from engine (Starship class icons)
};

/** Search for assets/files to attach via @ mention */
export async function searchContextItems(query: string): Promise<ContextItem[]> {
	const bridge = getBridge();
	if (bridge) {
		const result = await bridge.searchcontextitems(query);
		return parseResult(result);
	}
	return [];
}

// ── Agent Authentication ────────────────────────────────────────────

export type AuthMethod = {
	id: string;
	name: string;
	description: string;
	isTerminalAuth: boolean;
};

/** Get available auth methods for an agent */
export async function getAuthMethods(agentName: string): Promise<AuthMethod[]> {
	const bridge = getBridge();
	if (bridge) {
		const result = await bridge.getauthmethods(agentName);
		return parseResult(result);
	}
	return [];
}

/** Start agent login with a specific auth method */
export async function startAgentLogin(agentName: string, methodId: string): Promise<void> {
	const bridge = getBridge();
	if (bridge) {
		await bridge.startagentlogin(agentName, methodId);
	}
}

/** Register callback for login completion */
export function onLoginComplete(callback: (agentName: string, success: boolean, errorMessage: string) => void): void {
	const bridge = getBridge();
	if (bridge) {
		bridge.bindonlogincomplete(callback);
	}
}

// ── Agent Usage / Rate Limits ──────────────────────────────────────

export type RateLimitWindow = {
	usedPercent: number;
	resetsAt: string;
	windowDurationMinutes: number;
	hasData: boolean;
};

export type ExtraUsage = {
	isEnabled: boolean;
	usedAmount: number;
	limitAmount: number;
	currencyCode: string;
	hasData: boolean;
};

export type MeshyBalance = {
	configured: boolean;
	balance: number;
	isLoading: boolean;
	error: string;
};

export type AgentRateLimitData = {
	hasData: boolean;
	isLoading: boolean;
	errorMessage: string;
	agentName: string;
	planType: string;
	lastUpdated: string;
	primary: RateLimitWindow;
	secondary: RateLimitWindow;
	modelSpecific: RateLimitWindow;
	modelSpecificLabel: string;
	extraUsage: ExtraUsage;
	meshy: MeshyBalance;
};

/** Get cached rate limit data for an agent (triggers background fetch if needed) */
export async function getAgentUsage(agentName: string): Promise<AgentRateLimitData> {
	const bridge = getBridge();
	if (bridge) {
		const result = await bridge.getagentusage(agentName);
		return parseResult(result);
	}
	return { hasData: false, isLoading: false, errorMessage: '', agentName, planType: '', lastUpdated: '', primary: { usedPercent: 0, resetsAt: '', windowDurationMinutes: 0, hasData: false }, secondary: { usedPercent: 0, resetsAt: '', windowDurationMinutes: 0, hasData: false }, modelSpecific: { usedPercent: 0, resetsAt: '', windowDurationMinutes: 0, hasData: false }, modelSpecificLabel: '', extraUsage: { isEnabled: false, usedAmount: 0, limitAmount: 0, currencyCode: '', hasData: false }, meshy: { configured: false, balance: -1, isLoading: false, error: '' } };
}

/** Force-refresh usage data for an agent */
export async function refreshAgentUsage(agentName: string): Promise<void> {
	const bridge = getBridge();
	if (bridge) {
		await bridge.refreshagentusage(agentName);
	}
}

/** Register callback for agent usage/rate-limit updates */
export function onUsageUpdated(callback: (agentName: string, data: AgentRateLimitData) => void): void {
	const bridge = getBridge();
	if (bridge) {
		bridge.bindonusageupdated((agentName: string, usageJson: string) => {
			const data: AgentRateLimitData = JSON.parse(usageJson);
			callback(agentName, data);
		});
	}
}

// ── Project Indexing ────────────────────────────────────────────────

export type IndexingScopeBreakdown = {
	blueprints: number;
	cppFiles: number;
	assets: number;
	levels: number;
	config: number;
	documents: number;
};

export type IndexingSettings = {
	provider: 'openrouter' | 'custom';
	endpointUrl: string;
	apiKey: string;
	model: string;
	dimensions: number;
	autoIndex: boolean;
	scope: {
		blueprints: boolean;
		cppFiles: boolean;
		assets: boolean;
		levels: boolean;
		config: boolean;
		documents: boolean;
	};
	hasOpenRouterKey: boolean;
};

export type IndexingStatus = {
	state: 'idle' | 'indexing' | 'ready' | 'error';
	totalChunks: number;
	indexedChunks: number;
	lastIndexedAt: string;
	indexSizeBytes: number;
	errorMessage: string;
	breakdown: IndexingScopeBreakdown;
	embeddingModel: string;
	embeddingDimensions: number;
};

export async function getIndexingSettings(): Promise<IndexingSettings> {
	const bridge = getBridge();
	if (bridge) {
		const result = await bridge.getindexingsettings();
		return parseResult(result);
	}
	return {
		provider: 'openrouter', endpointUrl: '', apiKey: '', model: 'google/gemini-embedding-001',
		dimensions: 768, autoIndex: false,
		scope: { blueprints: true, cppFiles: true, assets: true, levels: true, config: false, documents: true },
		hasOpenRouterKey: false
	};
}

export async function getIndexingStatus(): Promise<IndexingStatus> {
	const bridge = getBridge();
	if (bridge) {
		const result = await bridge.getindexingstatus();
		return parseResult(result);
	}
	return { state: 'idle', totalChunks: 0, indexedChunks: 0, lastIndexedAt: '', indexSizeBytes: 0, errorMessage: '', breakdown: { blueprints: 0, cppFiles: 0, assets: 0, levels: 0, config: 0, documents: 0 }, embeddingModel: '', embeddingDimensions: 0 };
}

export async function setIndexingProvider(provider: 'openrouter' | 'custom'): Promise<void> {
	const bridge = getBridge();
	if (bridge) await bridge.setindexingprovider(provider);
}

export async function setIndexingEndpointUrl(url: string): Promise<void> {
	const bridge = getBridge();
	if (bridge) await bridge.setindexingendpointurl(url);
}

export async function setIndexingApiKey(key: string): Promise<void> {
	const bridge = getBridge();
	if (bridge) await bridge.setindexingapikey(key);
}

export async function setIndexingModel(model: string): Promise<void> {
	const bridge = getBridge();
	if (bridge) await bridge.setindexingmodel(model);
}

export async function setIndexingDimensions(dims: number): Promise<void> {
	const bridge = getBridge();
	if (bridge) await bridge.setindexingdimensions(dims);
}

export async function setAutoIndex(enabled: boolean): Promise<void> {
	const bridge = getBridge();
	if (bridge) await bridge.setautoindex(enabled);
}

export async function setIndexingScopeEnabled(scope: string, enabled: boolean): Promise<void> {
	const bridge = getBridge();
	if (bridge) await bridge.setindexingscopeenabled(scope, enabled);
}

export async function startIndexing(): Promise<void> {
	const bridge = getBridge();
	if (bridge) await bridge.startindexing();
}

export async function clearIndex(): Promise<void> {
	const bridge = getBridge();
	if (bridge) await bridge.clearindex();
}

// ── Source Control ──────────────────────────────────────────────────

export type SourceControlStatus = {
	enabled: boolean;
	provider: string;
	branch: string;
	changesCount: number;
	connected: boolean;
};

/** Get current source control status (branch, changes, provider) */
export async function getSourceControlStatus(): Promise<SourceControlStatus> {
	const bridge = getBridge();
	if (bridge) {
		const result = await bridge.getsourcecontrolstatus();
		return parseResult(result);
	}
	return { enabled: false, provider: '', branch: '', changesCount: -1, connected: false };
}

/** Open the UE source control changelists tab */
export async function openSourceControlChangelist(): Promise<void> {
	const bridge = getBridge();
	if (bridge) {
		await bridge.opensourcecontrolchangelist();
	}
}

/** Open the UE check-in/submit dialog */
export async function openSourceControlSubmit(): Promise<void> {
	const bridge = getBridge();
	if (bridge) {
		await bridge.opensourcecontrolsubmit();
	}
}

// ── Terminal ────────────────────────────────────────────────────────

/** Start a new terminal session. Returns the terminal ID. */
export async function startTerminal(workingDir: string = '', shell: string = ''): Promise<{ terminalId?: string; error?: string }> {
	const bridge = getBridge();
	if (bridge) {
		const result = await bridge.startterminal(workingDir, shell);
		return parseResult(result);
	}
	return { error: 'Not in UE' };
}

/** Write input data to a terminal (raw string from xterm.js onData) */
export async function writeTerminal(terminalId: string, data: string): Promise<void> {
	const bridge = getBridge();
	if (bridge) {
		await bridge.writeterminal(terminalId, data);
	}
}

/** Resize terminal PTY */
export async function resizeTerminal(terminalId: string, cols: number, rows: number): Promise<void> {
	const bridge = getBridge();
	if (bridge) {
		await bridge.resizeterminal(terminalId, cols, rows);
	}
}

/** Close a terminal session */
export async function closeTerminal(terminalId: string): Promise<void> {
	const bridge = getBridge();
	if (bridge) {
		await bridge.closeterminal(terminalId);
	}
}

const terminalOutputListeners = new Set<(terminalId: string, base64Data: string) => void>();
const terminalExitListeners = new Set<(terminalId: string, exitCode: number) => void>();
let terminalOutputBridgeBound = false;
let terminalExitBridgeBound = false;

function dispatchTerminalOutput(terminalId: string, base64Data: string): void {
	for (const listener of terminalOutputListeners) {
		try {
			listener(terminalId, base64Data);
		} catch (e) {
			console.warn('subscribeTerminalOutput listener error:', e);
		}
	}
}

function dispatchTerminalExit(terminalId: string, exitCode: number): void {
	for (const listener of terminalExitListeners) {
		try {
			listener(terminalId, exitCode);
		} catch (e) {
			console.warn('subscribeTerminalExit listener error:', e);
		}
	}
}

/**
 * Subscribe to PTY output for all terminal sessions. Multiple xterm instances must each subscribe —
 * UE only allows one bridge callback; this multicasts.
 * @returns Unsubscribe (call on component destroy).
 */
export function subscribeTerminalOutput(
	callback: (terminalId: string, base64Data: string) => void
): () => void {
	terminalOutputListeners.add(callback);
	const bridge = getBridge();
	if (bridge && !terminalOutputBridgeBound) {
		bridge.bindonterminaloutput(dispatchTerminalOutput);
		terminalOutputBridgeBound = true;
	}
	return () => {
		terminalOutputListeners.delete(callback);
	};
}

/** @returns Unsubscribe (call on component destroy). */
export function subscribeTerminalExit(
	callback: (terminalId: string, exitCode: number) => void
): () => void {
	terminalExitListeners.add(callback);
	const bridge = getBridge();
	if (bridge && !terminalExitBridgeBound) {
		bridge.bindonterminalexit(dispatchTerminalExit);
		terminalExitBridgeBound = true;
	}
	return () => {
		terminalExitListeners.delete(callback);
	};
}

// ── Studio / Generative Providers ──────────────────────────────────

export type GenerativeActionDescriptor = {
	actionId: string;
	description: string;
	inputHints: string[];
	outputHints: string[];
	creditCost: string;
	isSynchronous: boolean;
	paramsSchema?: {
		type: string;
		properties: Record<string, {
			type: string;
			description?: string;
			enum?: string[];
			default?: unknown;
			minimum?: number;
			maximum?: number;
		}>;
		required?: string[];
	};
};

export type GenerativeProviderInfo = {
	id: string;
	displayName: string;
	website: string;
	actions: GenerativeActionDescriptor[];
};

export type GenerativeJobInfo = {
	providerId: string;
	actionId: string;
	jobId: string;
	status: 'pending' | 'running' | 'succeeded' | 'failed' | 'cancelled';
	progress: number;
	resultUrl: string;
	thumbnailUrl: string;
	extraUrls: Record<string, string>;
	imageUrls: string[];
	error: string;
};

export type GenerativeJobResult = {
	success: boolean;
	job?: GenerativeJobInfo;
	error?: string;
};

export type GenerativeBalanceResult = {
	success: boolean;
	balance: number;
	error?: string;
};

/** Get all registered generative providers with their actions and parameter schemas */
export async function getGenerativeProviders(): Promise<GenerativeProviderInfo[]> {
	const bridge = getBridge();
	if (bridge) {
		const result = await bridge.getgenerativeproviders();
		return parseResult<GenerativeProviderInfo[]>(result);
	}
	return [];
}

/** Submit a generation job to a provider */
export async function submitGenerativeJob(
	providerId: string,
	actionId: string,
	params: Record<string, unknown>
): Promise<GenerativeJobResult> {
	const bridge = getBridge();
	if (bridge) {
		const result = await bridge.submitgenerativejob(providerId, actionId, JSON.stringify(params));
		return parseResult<GenerativeJobResult>(result);
	}
	return { success: false, error: 'Bridge not available' };
}

/** Check status of a generation job */
export async function checkGenerativeJobStatus(
	providerId: string,
	jobId: string,
	actionId: string
): Promise<GenerativeJobResult> {
	const bridge = getBridge();
	if (bridge) {
		const result = await bridge.checkgenerativejobstatus(providerId, jobId, actionId);
		return parseResult<GenerativeJobResult>(result);
	}
	return { success: false, error: 'Bridge not available' };
}

/** Get credit balance for a generative provider */
export async function getGenerativeBalance(providerId: string): Promise<GenerativeBalanceResult> {
	const bridge = getBridge();
	if (bridge) {
		const result = await bridge.getgenerativebalance(providerId);
		return parseResult<GenerativeBalanceResult>(result);
	}
	return { success: false, balance: -1, error: 'Bridge not available' };
}

// ── Crash Reporting ─────────────────────────────────────────────────

export type CrashRecord = {
	crashId: string;
	timestamp: string;
	errorMessage: string;
	crashType: string;
	callstackSummary: string;
	basicReported: boolean;
	fullLogSent: boolean;
	fullLogDeclined: boolean;
	manuallyReported: boolean;
};

/** Get crash history from local crash_history.json */
export async function getCrashHistory(): Promise<CrashRecord[]> {
	if (currentTransport === 'remote') return relayCall<CrashRecord[]>('getCrashHistory');
	const bridge = getBridge();
	if (bridge) {
		const result = await bridge.getcrashhistory();
		return parseResult<CrashRecord[]>(result);
	}
	return [];
}

/** Manually send a crash report for a previously declined crash */
export async function reportCrash(crashId: string): Promise<{ success: boolean }> {
	if (currentTransport === 'remote') return relayCall<{ success: boolean }>('reportCrash', crashId);
	const bridge = getBridge();
	if (bridge) {
		const result = await bridge.reportcrash(crashId);
		return parseResult<{ success: boolean }>(result);
	}
	return { success: false };
}

// ── NeoStack Sign-in (Device Authorization Grant) ────────────────────

export type DeviceAuthStatus =
	| 'idle'
	| 'requesting'
	| 'waiting'
	| 'polling'
	| 'redeeming'
	| 'success'
	| 'error';

export interface DeviceAuthState {
	status: DeviceAuthStatus;
	message: string;
	verificationUri: string;
}

/** Kick off the OAuth device flow against neostack.dev. Progress updates
 *  arrive on the callback registered via onDeviceAuthStatusChanged. */
export async function startNeoStackDeviceAuth(): Promise<void> {
	const bridge = getBridge();
	if (bridge) {
		await bridge.startneostackdeviceauth();
	}
}

/** Abort the in-progress flow. Safe to call when nothing is running. */
export async function cancelNeoStackDeviceAuth(): Promise<void> {
	const bridge = getBridge();
	if (bridge) {
		await bridge.cancelneostackdeviceauth();
	}
}

/** Register a listener for device-auth status updates. */
export function onDeviceAuthStatusChanged(callback: (state: DeviceAuthState) => void): void {
	const bridge = getBridge();
	if (bridge) {
		bridge.bindondeviceauthstatuschanged((statusJson: string) => {
			try {
				const state = JSON.parse(statusJson) as DeviceAuthState;
				callback(state);
			} catch {
				// Bridge sent malformed JSON — surface as an error state.
				callback({
					status: 'error',
					message: 'Bad status payload from editor.',
					verificationUri: '',
				});
			}
		});
	}
}
