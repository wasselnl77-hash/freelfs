# Notes on native/spec of EHS-Studio/dssl (v0.16.3), from writing libdssl

Found while implementing chapters 2 to 11 in C and checking the result byte for byte against the
reference engine (commit 0d7ad1f69d35d45f2be5fac16fb3b4e2b29350cf, `public/dssl/*.js`) over 703
inputs. Useful to send to the DSSL developers. Each note says what the spec says, what the engine
does, a one-line input that shows it, and what we did. "Engine" always means the JS files of that
commit. Reproduce with `dssl2css --json` against the engine, or `make check`.

Kinds: **D** the spec and the engine disagree, **G** a gap or ambiguity in the spec, **B** an engine
bug the spec does not mention, **M** something promised by the spec that is not in the checkout.

## M: missing from the checkout

**1. The conformance corpus (M).** Chapter 10.4 and DOC-13 to DOC-15, and `native/README.md`, describe
`scripts/native-dump.mjs`, `native/expected/` (committed expected JSON), `native/corpus/EXCLUDED.md`,
`native/tests/differential.rs` and a generated `native/PARITY.md`. None of them is in 0.16.3
(the crate is a stub and `native/` holds only `data/`, `spec/` and `src/`). We built our own corpus and
dumper (`tools/`); the expected files are the engine's output in the canonical form of 10.3. If the
real corpus appears, `make check` can be pointed at it by adding the file name mapping in
`tests/conform.c`.

## D: the spec and the engine disagree

**2. A nested `tokens` block (TOK-2).** The spec says a nested `tokens` block adds the header text
`tokens` to the key path. The engine joins `child.sel`, which is undefined for a `tokens` node, so
`tokens { x { tokens { b: 2 } } }` defines the key `x.undefined.b`, and at the top level
`tokens { tokens { a: 1 } }` defines `a` (the inner block is transparent). Case:
`h-tokens-nested-tokens`. We follow the engine. The sensible fix is to follow the spec.

**3. An unclosed `raw { }` (SYN-19).** The spec says the CSS captured so far is discarded. The engine
reports "discarded" but the node keeps its `css`, and the resolver emits it. `raw {\n .x { color: red; }`
yields a `raws` entry `.x { color: red; }` plus the error. We follow the engine. Case
`h-syntax-raw-unclosed`.

**4. `behavior` and `motion` must be top-level (PRP-37, MOT-1).** The engine's check is
`if (ctx.depth)`, and `depth` is never incremented, so it never fires. A `behavior` or `motion` inside a
selector is hoisted by the first pass and registered globally, without a word. Case
`h-behavior-top-level`. Either the check should work or the spec should say they are allowed anywhere.

**5. `@media needs a condition` (CAS-17) cannot be reached.** The header is trimmed before it is classified
(2.5), and `@media ` with the trailing space is what the classifier looks for, so an empty `@media` is
"unknown at-rule" (`.a { @media { color: red } }` gives `unknown at-rule "@media"`). The same holds for
`@signal`. For `@container` the engine substitutes `(min-width: 0)` itself and reports nothing, where
CAS-22 reads as if the empty-condition error applied. Case `h-media-empty`.

**6. `!important` detection (VAL-15).** The spec says the text `!important` (any case, any space after `!`)
outside a string. The engine tests `!` followed by the regexp `\s*important` without an anchor, so a `!`
followed anywhere later by the word important matches: `margin: 1 ! x important` is rejected. Harmless in
practice, wrong in principle. Case `h-values-important`.

**7. `@layer tokens { ... }` (CAS-4).** The spec says the layer `tokens` is used only for token rules. The
engine accepts ordinary rules in it (`@layer tokens { .b { color: red } }` gives a rule `.b` in layer
`tokens`). Case `h-layers`.

**8. The CSS lane's `@keyframes` (INT-6).** `static.js` turns the camelCase property names of the frames
back into kebab-case with `/([a-z0-9])([A-Z])/`, which loses the leading dash of vendor properties:
`-webkit-text-fill-color` is camelized to `WebkitTextFillColor` and comes back as
`webkit-text-fill-color`, invalid CSS. libdssl keeps the original name in CSS and the camelCase name in the
rule objects (which is what 7.1 MOT-4 says).

## G: gaps and ambiguities in the spec

**9. Where lowered layers sit against unlayered CSS (11.3, INT-6).** The engine writes inline styles, so
today DSSL beats every stylesheet of the page (except `!important`), and `yield:` (6.7) exists to hand
properties back. The lowered CSS lives in cascade layers, where every normal (unlayered) page stylesheet
beats it, whatever the specificity. So the same page changes meaning when it moves from the JS engine to the
lowered CSS: DSSL-versus-CSS conflicts flip, and `yield:` becomes a no-op. The spec says "the result must equal
the cascade of chapter 5", but chapter 5 only orders DSSL rules among themselves (CAS-44 covers the page's
inline styles, nothing else). It needs a decision: native DSSL loses to the page's own CSS (and `yield:` goes),
or the lowering is unlayered so that source order decides, or DSSL gets a hook that is stronger than author
CSS. Until then FreeBWS must treat a page that mixes DSSL and its own CSS as a risk (README, Integration).

**10. `raw { }` in the lowered CSS.** `raw` is emitted unlayered, so it beats every DSSL rule except
`pin` (which is `!important`). In the JS lane `raw` CSS is an ordinary stylesheet and loses to DSSL's inline
styles. Same cause as 9; the spec does not say where `raw` belongs in the cascade (CAS-11 only gives it an
order number).

**11. White space (SYN-1, SYN-7).** The spec says `\r` is not special and "values are trimmed", but not what
white space is. The engine's `trim()` and `\s` are Unicode-aware (NBSP, U+2028/2029, U+FEFF, the U+2000 block),
which a C or Rust port must decide on. libdssl treats only ASCII white space as white space.

**12. "First 40 characters" (SYN-14, DIA messages).** Messages cut text with `slice(0, 40)`, which counts
UTF-16 units. "Characters" is ambiguous for non-BMP text. libdssl cuts on UTF-16 units at a character boundary.

**13. Columns (SYN-2).** Diagnostics have a line only. For editors and for a browser console a column is
needed; libdssl adds one (not part of the canonical JSON).

**14. The registry rule (TOK-9, D-6).** Every token of the map, with its last definition, as one unconditional
rule. Within one file this is harmless, across files it lets a later file's registry rule override an earlier
file's `@media dark` value (the open D-6). libdssl's CSS lane gives the first sheet of a page every token and
later sheets only the tokens that are new, which avoids it; the JSON keeps the spec's full registry rule. The
registry rule also duplicates, in the CSS, every token that a `tokens` block defines (once in the registry,
once in the block); the 11.3 example shows one. Dropping registry entries that a block of the same sheet
redefines unconditionally would remove the duplicates.

**15. `@layer a, b;` (2.5).** The CSS statement form of `@layer` is not DSSL, and `@layer base, components;` gives
`expected ":" in "@layer base, components"` (a declaration without a block). The spec could say so, since
authors with CSS habits will write it.

**16. Alpha modifier and the white space after it (VAL-8).** The spec says "optional white space and an optional
`%`". The engine's pattern `^\s*\/\s*([0-9.]+)\s*%?` also swallows the white space after the number when no `%`
follows, so `border: 1 solid color.a/50 dashed` becomes `...50%, transparent)dashed` (no space). Case
`h-alpha-spacing`. Probably unintended; the spec should either say the trailing space is consumed or the
engine should keep it.

**17. `transition:` with an easing call that contains spaces (PRP-21).** The part is split on white space,
so `transition: 2s cubic-bezier(0.1, 0.2, 0.3, 1) opacity` takes `cubic-bezier(0.1,` as the easing and
reports `unknown transition property "0.2, 0.3, 1) opacity"`. `steps(4, end)` has the same problem. The
spec's grammar allows the calls; the engine does not. Case `h-semantic-transition`.

**18. An unclosed parenthesis in a value (VAL-5, VAL-7).** The argument text is the slice up to the matching
parenthesis; when there is none the engine still cuts one character off the end (`calc(1 + 23` at the end of a
file resolves to `calc(1px + 2px)`). The spec does not say what an unclosed call does.

**19. `fluid` with missing arguments (VAL-12).** `font-size: fluid 16` reports `fluid: "undefined" is not a size`
(the word comes from a JavaScript `undefined` in a template string), and `fluid 16 24 2000` reports
`"2000 undefined" needs the larger viewport second`. Case `h-fluid-bad-bounds`. The messages are compared
byte for byte (DIA-1), so the spec either has to quote them or the engine to fix them.

**20. `row:` and `column:` after `gap` without a value (PRP-8).** The spec says `gap needs a value` is an
error; it does not say that the engine then stops reading the declaration (`break`), dropping every word after
the `gap`. Case `h-semantic-layout` (`.r5`).

**21. A bad number after `delay`, `jitter` or `iterations` (MOT-9).** The error is reported and the bad word is
then read again as the next part, because the index is not advanced: `animate: rise delay wat` reports
`delay needs a number` and also `unknown animate part "wat"`. Unspecified; we follow the engine. Case
`h-animate`.

**22. `motionNames` order (10.2).** The resolver does not return a `motionNames` field; it is derived as
`Object.keys(motions)`. The spec says "in source order", but JavaScript puts integer-like names
(`motion 2 { }`) first. libdssl keeps source order (not covered by the corpus).

**23. Selector words that are close to a state word (CAS-8).** The state words are matched case-sensitively
(`Hover { }` is a selector), but the warning's did-you-mean and the HTML element exemption work on the
lower-cased word, so `.menu { LI { } }` is quiet and `.menu { Hove { } }` warns. The spec does not say.

**24. Number parsing in `glass:` and `tilt:` (FX-29, FX-31).** "A number" is the engine's `Number(word)`: hex
(`0x10`), binary, octal, exponent (`1e1`), `Infinity` and padded text all count, and `glass: Infinity` writes
`blur(Infinitypx)`. Case `h-glass-number-forms`. The spec should say what a number is here, or the engine
should be stricter.

**25. Plain objects as dictionaries (PRP-1, PRP-38, MOT-11).** Property, behavior, motion and place names are
looked up in plain JavaScript objects, so names of `Object.prototype` members behave strangely:
`.a { constructor: 1 }` is accepted and produces a declaration with no property; `.a { use constructor }` and
`.a { animate: constructor 400 }` throw a TypeError (`children is not iterable`) and the whole sheet is lost.
A conforming implementation should treat them as unknown names; libdssl does.

**26. Limits.** The spec has none (nesting depth, expansion of behaviors that use each other twice, size of the
cross product of selector lists, input size). A chain of 40 behaviors that each use the next one twice would
expand to 2^40 steps in the engine (`use` replays the children every time, nothing is cached). A native
implementation needs limits; libdssl has a nesting limit, a step budget and a memory cap (README).

**27. `raw { }` and `fonts { }` inside a block (SYN-18).** The error is reported and the balanced block
skipped, but the empty node stays in the syntax tree (visible at level 1, e.g. `.a { raw { .b { } } }` leaves a
`raw` child without `css`). A level 1 implementation has to keep it to match; the spec does not say.

**28. Media conditions: where numbers get `px` (CAS-20).** Only a bare number after `>`, `<` or `=` is
completed, so the common spelling `@media (min-width: 768)` stays unitless (and is invalid CSS: the query never
matches), as do `(max-width: 90)` and the first number of `(400 <= width <= 800)`. In addition the engine's
regular expression backtracks through a decimal: `(width >= 1.5px)` becomes `(width >= 1px.5px)`. Case
`h-media-sugar`. The spec could say that the colon form is not completed, and the pattern should require the
end of the number.

## Smaller observations

- Appendix A lists `enabled`, `indeterminate`, `first-of-type`, `last-of-type`, `link` and `visited` as state
  words (they are accepted as `state` blocks) but the `states` field (CAS-29) is computed from a different list
  of 20 words that omits them, so `.a { link { } }` has `states: []`.
- `radius: xl` (D-4): the warning text says "will not apply"; the declaration is still emitted with
  `var(--dssl-radius-xl)`, which makes `border-radius` invalid at computed-value time and therefore `0`.
  The text could say that.
- A semantic property whose value is empty still emits a declaration with an empty value (`radius:` gives
  `border-radius: ''`, `shadow:` gives `box-shadow: ''`), which the CSS lane writes as `border-radius: ;`.
- `@container` queries go through the same normaliser as `@media`: a named container passes through with its
  name in front (`@container card (width > 20)` becomes `card (width > 20px)`); the spec's CAS-23 lists only the
  bounds.
