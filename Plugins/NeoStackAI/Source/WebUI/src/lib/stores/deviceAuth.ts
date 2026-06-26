import { writable, get } from 'svelte/store';
import {
	cancelNeoStackDeviceAuth as bridgeCancelDeviceAuth,
	onDeviceAuthStatusChanged,
	startNeoStackDeviceAuth as bridgeStartDeviceAuth,
	type DeviceAuthState,
	type DeviceAuthStatus
} from '$lib/bridge.js';

// Single source of truth for the device-auth dialog. The previous design
// stored this per `NeoStackSignInButton` instance and let each instance
// bind its own callback against the bridge's single-slot channel; the
// last mount silently won the slot, so any earlier button's modal was
// orphaned and stuck at "Opening browser…". One store + one global
// listener + one singleton dialog avoids the whole class of bug.

const IDLE: DeviceAuthState = { status: 'idle', message: '', verificationUri: '' };

export const deviceAuthState = writable<DeviceAuthState>(IDLE);

let listenerBound = false;

export function isActiveStatus(s: DeviceAuthStatus): boolean {
	return s === 'requesting' || s === 'waiting' || s === 'polling' || s === 'redeeming';
}

/** Subscribe the store to bridge broadcasts. Idempotent — call once at app
 *  mount alongside the other listener binders. */
export function bindDeviceAuthListener(): void {
	if (listenerBound) return;
	listenerBound = true;
	onDeviceAuthStatusChanged((next) => {
		deviceAuthState.set(next);
	});
}

/** Start the device-auth flow. Resolves with the *terminal* state
 *  (`success`, `error`, or `idle` from cancellation) so callers can wait
 *  for a definitive outcome before firing their own `onsuccess` callback. */
export function startDeviceAuth(): Promise<DeviceAuthState> {
	// Optimistic — open the dialog instantly without waiting for the
	// C++ flow's first broadcast.
	deviceAuthState.set({ status: 'requesting', message: '', verificationUri: '' });

	const terminal = waitForTerminal();

	// Fire-and-forget; the listener pushes updates into the store. If the
	// bridge call itself throws, surface that as an error on the store too.
	bridgeStartDeviceAuth().catch((e) => {
		deviceAuthState.set({
			status: 'error',
			message: e instanceof Error ? e.message : 'Couldn\u2019t start sign-in.',
			verificationUri: ''
		});
	});

	return terminal;
}

/** Cancel the in-flight flow if any, and reset the store. */
export async function cancelDeviceAuth(): Promise<void> {
	try {
		await bridgeCancelDeviceAuth();
	} catch {
		// Ignore — the C++ side handles missing-flow cleanup.
	}
	deviceAuthState.set(IDLE);
}

/** Returns a promise resolving with the first non-active state seen after
 *  this call. Used by callers that want to await a terminal outcome. */
function waitForTerminal(): Promise<DeviceAuthState> {
	return new Promise((resolve) => {
		const unsubscribe = deviceAuthState.subscribe((s) => {
			if (!isActiveStatus(s.status)) {
				// Defer unsubscribe so the subscribe() callback finishes first.
				queueMicrotask(() => unsubscribe());
				resolve(s);
			}
		});
	});
}

export function getDeviceAuthSnapshot(): DeviceAuthState {
	return get(deviceAuthState);
}
