<script lang="ts">
	import { Streamdown, type StreamdownProps } from "svelte-streamdown";
	import { cn } from "$lib/utils";

	// Plain Streamdown, no Shiki. Code blocks render as unstyled <pre><code>
	// which is dramatically cheaper than the full Shiki pipeline (saves ~1 MB
	// from the initial chunk and removes per-token highlight work during
	// streaming). Syntax colors in chat are not worth the startup cost.

	type Props = StreamdownProps & {
		class?: string;
	};

	let { class: className, ...restProps }: Props = $props();
</script>

<Streamdown
	class={cn("size-full [&>*:first-child]:mt-0 [&>*:last-child]:mb-0", className)}
	baseTheme="shadcn"
	animation={restProps.parseIncompleteMarkdown
		? { enabled: true, type: 'fade', duration: 80, tokenize: 'word', animateOnMount: false }
		: { enabled: false }}
	{...restProps}
/>
