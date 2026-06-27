// Follows the best practices established in https://shiki.matsu.io/guide/best-performance
// Langs are loaded on-demand via `ensureLanguage()` rather than eagerly at startup.
import { createJavaScriptRegexEngine } from "shiki/engine/javascript";
import { createHighlighterCore, type HighlighterCore } from "shiki/core";

const bundledLanguages = {
	bash: () => import("@shikijs/langs/bash"),
	diff: () => import("@shikijs/langs/diff"),
	javascript: () => import("@shikijs/langs/javascript"),
	json: () => import("@shikijs/langs/json"),
	svelte: () => import("@shikijs/langs/svelte"),
	typescript: () => import("@shikijs/langs/typescript"),
	python: () => import("@shikijs/langs/python"),
	tsx: () => import("@shikijs/langs/tsx"),
	jsx: () => import("@shikijs/langs/jsx"),
	css: () => import("@shikijs/langs/css"),
	text: () => import("@shikijs/langs/markdown"),
};

/** The languages configured for the highlighter */
export type SupportedLanguage = keyof typeof bundledLanguages;

/** A preloaded highlighter instance. Langs/themes are loaded on demand. */
export const highlighter: Promise<HighlighterCore> = createHighlighterCore({
	themes: [
		import("@shikijs/themes/github-light-default"),
		import("@shikijs/themes/github-dark-default"),
	],
	langs: [],
	engine: createJavaScriptRegexEngine(),
});

const loadedLanguages = new Set<string>();

/** Load a language into the highlighter on first use. Safe to call repeatedly. */
export async function ensureLanguage(lang: string): Promise<void> {
	if (loadedLanguages.has(lang)) return;
	const loader = bundledLanguages[lang as SupportedLanguage];
	if (!loader) return;
	loadedLanguages.add(lang);
	try {
		const hl = await highlighter;
		await hl.loadLanguage(await loader());
	} catch {
		loadedLanguages.delete(lang);
	}
}
