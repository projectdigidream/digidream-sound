//
// The tool's icons: flat line drawings, never colour emoji (the toolkit's rule
// since 2026-10-17). Copied from uikit's base skin (uikit/skins/claude/icons,
// catalog groups `sfx` and `status`) with the stroke made `currentColor`, so
// they take the text colour of whatever they sit in, in any theme.
// Regenerate after uikit redraws them: the snippet in DEVELOPING.md.
//
const SVG = {
  'gunshot': '<svg aria-hidden="true" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.75" stroke-linecap="round" stroke-linejoin="round"><path d="M3.5 8h13a1 1 0 0 1 1 1v2a1 1 0 0 1-1 1H11l-1.5 6.5h-4L7 12H3.5z"/><path d="M19.5 8.5 21.5 7M20 11h2"/></svg>',
  'explosion': '<svg aria-hidden="true" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.75" stroke-linecap="round" stroke-linejoin="round"><path d="M12 3l1.8 5.2 5.2-2-2 5.2L22 13l-5.2 1.3 1.4 5.4-5-2.6L12 22l-1.2-4.9-5 2.6 1.4-5.4L2 13l5-1.6-2-5.2 5.2 2z"/></svg>',
  'laser': '<svg aria-hidden="true" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.75" stroke-linecap="round" stroke-linejoin="round"><path d="M3.5 18 15 6.5"/><path d="M15 6.5l2-2M17.5 9l2.5-.5M14.5 4l.5-2.5"/><path d="M6.5 18H3.5v-3"/></svg>',
  'hit': '<svg aria-hidden="true" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.75" stroke-linecap="round" stroke-linejoin="round"><path d="M8 13V9a1.5 1.5 0 0 1 3 0V8a1.5 1.5 0 0 1 3 0v1a1.5 1.5 0 0 1 3 0v6a5 5 0 0 1-5 5h-1a5 5 0 0 1-5-5v-2a1.5 1.5 0 0 1 2-1.4"/><path d="M4 4l2 2M12 2.5v2M19.5 4l-2 2"/></svg>',
  'bell': '<svg aria-hidden="true" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.75" stroke-linecap="round" stroke-linejoin="round"><path d="M6 16.5V11a6 6 0 0 1 12 0v5.5l1.5 2h-15z"/><path d="M10 20.5a2 2 0 0 0 4 0"/></svg>',
  'pickup': '<svg aria-hidden="true" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.75" stroke-linecap="round" stroke-linejoin="round"><path d="M12 3.5l1.8 5.2 5.2 1.8-5.2 1.8-1.8 5.2-1.8-5.2L5 10.5l5.2-1.8z"/><path d="M18.5 16.5l.7 1.8 1.8.7-1.8.7-.7 1.8-.7-1.8-1.8-.7 1.8-.7z"/></svg>',
  'jingle': '<svg aria-hidden="true" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.75" stroke-linecap="round" stroke-linejoin="round"><path d="M9 17.5v-12l10-2v12"/><circle cx="7" cy="17.5" r="2"/><circle cx="17" cy="15.5" r="2"/><path d="M9 9l10-2"/></svg>',
  'door': '<svg aria-hidden="true" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.75" stroke-linecap="round" stroke-linejoin="round"><path d="M6 20.5v-16a1 1 0 0 1 1-1h10a1 1 0 0 1 1 1v16"/><path d="M4 20.5h16"/><path d="M14.5 12.5v.01"/></svg>',
  'machine': '<svg aria-hidden="true" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.75" stroke-linecap="round" stroke-linejoin="round"><circle cx="12" cy="12" r="3"/><circle cx="12" cy="12" r="6"/><path d="M12 3.5V6M12 18v2.5M3.5 12H6M18 12h2.5M6 6l1.8 1.8M16.2 16.2 18 18M6 18l1.8-1.8M16.2 7.8 18 6"/></svg>',
  'engine': '<svg aria-hidden="true" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.75" stroke-linecap="round" stroke-linejoin="round"><path d="M3.5 15.5v-3l2-4.5h11l3 4.5v3z"/><circle cx="7.5" cy="16.5" r="1.8"/><circle cx="16.5" cy="16.5" r="1.8"/><path d="M6.5 12h11"/></svg>',
  'monster': '<svg aria-hidden="true" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.75" stroke-linecap="round" stroke-linejoin="round"><path d="M6 8.5 4.5 3.5l4 3M18 8.5l1.5-5-4 3"/><path d="M12 6a6.5 6.5 0 0 0-6.5 6.5c0 4 3 7 6.5 7s6.5-3 6.5-7A6.5 6.5 0 0 0 12 6z"/><path d="M9 12l1.5 1M15 12l-1.5 1"/><path d="M9.5 16.5c1.5 1 3.5 1 5 0"/></svg>',
  'voice': '<svg aria-hidden="true" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.75" stroke-linecap="round" stroke-linejoin="round"><path d="M9 20.5V17a6.5 6.5 0 1 1 7-6.5l1.5 3H16v3a2 2 0 0 1-2 2h-1"/><path d="M19.5 7.5a4 4 0 0 1 0 4M21.5 5.5a7 7 0 0 1 0 8"/></svg>',
  'whoosh': '<svg aria-hidden="true" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.75" stroke-linecap="round" stroke-linejoin="round"><path d="M3 9h11a2.5 2.5 0 1 0-2.5-2.5"/><path d="M3 13h15a2.5 2.5 0 1 1-2.5 2.5"/><path d="M3 17h7"/></svg>',
  'randomize': '<svg aria-hidden="true" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.75" stroke-linecap="round" stroke-linejoin="round"><rect x="4.5" y="4.5" width="15" height="15" rx="2.5"/><path d="M8.5 8.5v.01M15.5 8.5v.01M12 12v.01M8.5 15.5v.01M15.5 15.5v.01"/></svg>',
  'vary': '<svg aria-hidden="true" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.75" stroke-linecap="round" stroke-linejoin="round"><path d="M8 4l1.2 3.3 3.3 1.2-3.3 1.2L8 13l-1.2-3.3-3.3-1.2 3.3-1.2zM16 11l1.4 3.6L21 16l-3.6 1.4L16 21l-1.4-3.6L11 16l3.6-1.4z"/></svg>',
  'match': '<svg aria-hidden="true" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.75" stroke-linecap="round" stroke-linejoin="round"><rect x="9" y="3.5" width="6" height="10" rx="3"/><path d="M6 11a6 6 0 0 0 12 0"/><path d="M12 17v3.5M9 20.5h6"/></svg>',
  'unsure': '<svg aria-hidden="true" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.75" stroke-linecap="round" stroke-linejoin="round"><circle cx="12" cy="12" r="8.5"/><path d="M9 9.5v.01M15 9.5v.01"/><path d="M8.5 15.5c1-1 2-1 3 0s2 1 3 0"/></svg>',
  'error': '<svg aria-hidden="true" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.75" stroke-linecap="round" stroke-linejoin="round"><circle cx="12" cy="12" r="8.5"/><path d="m14.8 9.2-5.6 5.6M9.2 9.2l5.6 5.6"/></svg>',
  'warning': '<svg aria-hidden="true" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.75" stroke-linecap="round" stroke-linejoin="round"><path d="M10.3 4.2 2.8 17.5A2 2 0 0 0 4.5 20.5h15a2 2 0 0 0 1.7-3L13.7 4.2a2 2 0 0 0-3.4 0z"/><path d="M12 9.5v4"/><circle cx="12" cy="16.8" r="1" fill="currentColor" stroke="none"/></svg>',
}

/** A <span class="icon"> holding the drawing for `id` (an `sfx` or `status` id). */
export function icon (id) {
  const s = document.createElement('span')
  s.className = 'icon'
  s.innerHTML = SVG[id] ?? ''
  return s
}
