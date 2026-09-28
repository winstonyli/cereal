# Specs as preprocessor directives (brainstorm, not final)

Goal: VeriFast-style verification and UB elimination. Specs are written as
preprocessor directives instead of comments, with a `#pragma cereal …`
spelling that keeps files building with GCC and Clang.

## Why directives beat comments

1. **They are tokens.** The pp-lexer already tokenizes them, the index
   records their locations, and the skeleton pass sees them statically,
   including in inactive regions. Nothing new has to be parsed out of
   comment text.
2. **Macros can generate specs.** `_Pragma` inside a macro produces a spec
   at every expansion. Spec abstraction then comes from the preprocessor,
   which comment-based tools cannot offer:
   ```c
   #define OWNS_BUF(p, n) _Pragma(STR(cereal requires chars(p, n, _)))
   ```
3. **Conditional compilation composes.** Specs inside `#if` branches are
   per-configuration. Combined with the existing configuration-space
   analysis, a function can be verified in *every* configuration, not
   just the one being built.
4. **Macros themselves can have contracts.** A spec before a `#define` is
   checked at each expansion site. Hygiene rules become provable
   properties, for example "argument 1 of MAX must be side-effect free".

## Sketch

```c
#predicate buf(char *p, size_t n) = p != 0 &*& chars(p, n, _)

#requires buf(dst, n) &*& buf(src, n)
#ensures  buf(dst, n) &*& buf(src, n) &*& result == dst
char *copy(char *dst, const char *src, size_t n)
{
    #invariant i <= n &*& buf(dst, n) &*& buf(src, n)
    for (size_t i = 0; i < n; i++)
        dst[i] = src[i];
    return dst;
}

#verify strict                       /* UB policy, per function or region */
#ghost int calls = 0;                /* ghost state, erased by codegen */
#lemma void list_len_nonneg(struct node *l)
#requires list(l, ?len)
#ensures  list(l, len) &*& len >= 0
```

The portable spelling of each directive is `#pragma cereal requires …`.
`#ifdef __CEREAL__` guards are only needed for the bare-directive form.

## Design axes and open questions

| # | Question | Leaning |
|---|---|---|
| A | **Macro-expand spec expressions?** If yes, specs can use program constants (`BUF_SIZE`). The risk is a user macro clobbering a spec keyword. | Yes. Spec keywords (`result`, `old`, `forall`, `exists`, `_`) are recognized *before* expansion, the same way `defined` is in `#if`. |
| B | **Logic.** Full separation logic (VeriFast: `&*&`, points-to, predicates, open/close, lemmas) or first-order with a built-in memory model (Frama-C WP style)? | Separation logic. Out-of-bounds and use-after-free UB elimination need ownership anyway. |
| C | **Multi-line specs.** Backslash continuation, a block form `#spec … #endspec`, or both? | Both. Block form for predicates and lemmas. |
| D | **Attachment.** A spec applies to the next declaration or statement, like an attribute. Contracts on the prototype or the definition? | Either one; if both have contracts they must be identical, as with `#define`. |
| E | **Specs for code you can't edit** (libc and system headers). | Spec overlays: `-ispec DIR` with `stdio.h.spec` files naming declarations (`#contract fopen …`), plus `#trusted` for axiomatized functions. |
| F | **Operators.** VeriFast's `&*&` and `|->` already lex as C pp-tokens. For implication, `==>` (lexes as `==` `>`) or a word form (`implies`)? | `==>` in specs, reassembled from tokens. |
| G | **Macro contracts** (`#requires` before `#define`). | Yes; checked per expansion. |
| H | **Verify across configurations** (see "Conditional compilation composes" above). | Yes, opt in with `#verify all-configs`. |
| I | **Editor integration.** | Specs are indexed like macros: go-to-definition for predicates, the contract shown on hover at call sites, inlay hints for proven vs. runtime-checked obligations. |
