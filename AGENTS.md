# Nylon agent instructions

This file is the mandatory orientation for any coding agent working in this repository.

Before making architectural changes, read completely:

- `docs/PRODUCT_GOAL.md`
- `docs/ARCHITECTURE.md`
- `docs/CRITICAL_EDGE_CASES.md`
- `docs/HISTORICAL_AUDIT_NOTES.md`
- `docs/VALIDATION_NOTES.md`
- `README.md`

## Mandatory source style and implementation approach

The user approved the six-area organization and the actual Skeet coding conventions.
These instructions apply to ALL Nylon-owned C++: scanner, mapping, Atlas, image
operations, tests, utilities, and future code. Apply them to existing code as it is
reorganized. Third-party source and generated payload bytes are not style templates.
Do not rewrite unrelated code merely to make the entire tree uniform in one change.

The reference is `C:/skeet-reference/psylo/src`, reviewed at Skeet commit
`51e557a3b2fc9ec3c09db5722231ee1ec896becf`. Read its actual implementations when
available, especially `main.cpp`, `cheat/routines/context/context.hpp`,
`cheat/internal/instance/instance.hpp`, `core/threads/threads.cpp`,
`core/win32/win32.hpp`, and `gui/config/config.cpp`.
The concrete rules and examples below remain mandatory if that checkout is absent.
Do not substitute a model's preferred style or a generic formatter preset.

### Organization and implementation

- The final source areas are `core`, `image`, `scan`, `map`, `strip`, and `atlas`.
  This supersedes older target folder proposals, not their coverage/correctness rules.
- Use purposeful subject subfolders such as `scan/registry` and `scan/services`.
  Keep small features together; do not force a flat tree or create empty future files.
- Use lowercase folder/file names and matching `.hpp`/`.cpp` stems. A subject folder
  may have a main file of the same name, as in Skeet's `core/threads/threads.cpp`.
- Keep related data, operations, SQL and checks together in their owning area.
  Namespaces identify the area/subject (`nylon::scan::registry`), without reproducing
  purely organizational labels or inventing a namespace for every file.
- Prefer direct functions, plain records, explicit local state and visible execution
  order. Use small named operations when they simplify the caller's actual logic.
- Keep external API mechanics near their owner, like Skeet's `core::win32::read()`.
  A high-level scan should call a useful database operation rather than prepare and
  step SQLite statements itself. Do not wrap every external function mechanically.
- Keep resource ownership explicit. Existing move-only handles, database ownership,
  bounds checks, status/result errors and transaction guarantees must survive.
  Do not copy mutable global image state, unsafe ownership, bugs or game behavior.
- Use early error returns and readable loops. Preserve errors, provenance and explicit
  gap states. Never simplify a failed query into absence or a successful empty result.
- `namespace internal` is appropriate for implementation helpers, as in Skeet's
  configuration and thread code. It is not permission to add another service layer.
- A small shared `common.hpp` may hold common platform/standard declarations.
  Keep domain state, schemas, model state and implementation ownership out of it.
- Keep useful small/header-only functions inline and substantial implementations in
  their owning source files. Do not impose a universal header-only or class pattern.

### Names chosen in context

- Functions, variables, members and namespaces use lowercase snake case. Choose
  natural names within the function: `result`, `state`, `index`, `integrity_check`.
  The user explicitly dislikes cryptic names such as `rc` and inflated replacements
  such as `sqlite_result` or `integrity_passed`. Do not mechanically lengthen/shorten.
- Use `_object` for principal domain objects, as in `context_object`/`thread_object`;
  `_t` for supporting records such as `options_t`/`memory_t`; `_state` for state
  records; and `_type` where it describes an enum. Existing clear nouns such as
  `message` also occur. Do not append `_t` to every type or rename useful types blindly.
- Use plain member names, without a blanket `m_` prefix or trailing underscore rule.
  Necessary existing resource classes remain classes. Skeet has no class-definition
  examples: their new declarations use the same expanded layout, access labels one
  level inside the class and members one level below the labels.
- Named limits and general fixed configuration constants use uppercase snake case,
  as in `MAX_STRING_SIZE`/`DEFAULT_COLOR`. Field-like names in a dedicated format or
  offset namespace may be lowercase, as in `sdk::offsets`.
- New internal scoped enum members use the lowercase `recurring`/`continuous` pattern
  in `core/threads` and `utils/logs`. Preserve external and persisted spellings.
- Preserve external API names. Skeet's PascalCase `gui/custom` functions follow an
  ImGui-facing pattern; do not spread that naming into Nylon's general functions.
- Use clear explicit types for values and owned data. `auto` is appropriate for
  iterators, structured bindings, factory results and other locally obvious types.
  Keep necessary qualifiers, `[[nodiscard]]` and correct `noexcept` contracts.
- Use Skeet's `uint32_t`, `uint64_t`, `int64_t`, `size_t` spellings where declared.
  The pinned LLVM-MinGW compiler was tested with the project's warning flags and
  rejects `ui32`, `ui64`, `i64` suffixes. Use standard literals/suffixes or explicit
  conversions preserving width/sign; do not change toolchain flags to mimic MSVC.
- Use `nullptr`, named C++ casts, and fully written uppercase float suffixes such as
  `100.0F`. Preserve the types and values required by parsers and external formats.

### Exact presentation

- Hard tabs, displayed at four columns; indent namespace contents as well as bodies.
- Allman braces for namespaces, types, functions, conditionals, loops and scopes.
  Even single-return functions have a separate body and closing brace.
- No compact condition/body lines, unbraced branches, multiple executable statements
  per line, nested ternaries, or compressed loops/functions. Expand dense conditions
  into named intermediate values or readable multiline expressions when helpful.
- Use standalone indented `//` separators between logical steps, guard clauses,
  function definitions and declaration groups. They are intentional visual structure:
  NEVER delete them as cleanup or substitute blank lines/banners for them.
  Closely related declarations/assignments can remain consecutive, as in Skeet.
- Headers use path-derived uppercase include guards and `#endif // GUARD_NAME`.
  Use three standalone `//` lines after the guard, two before the main namespace,
  a separator inside the namespace at its beginning/end, and three before `#endif`.
- Source files normally include their matching header first, then two separators.
  Group additional includes by purpose with separators; retain two before the main
  namespace. Required preprocessor definitions may precede affected includes.
- Use quoted project includes, forward slashes, matching-header basenames and
  source-root paths for other project includes. No automatic alphabetic sorting.
- Empty parentheses stay empty: `initialize()`. Space inside nonempty parentheses:
  `read( address )`, `if ( !valid )`, `sizeof( value )`, including nested expressions.
- Space inside nonempty array brackets: `data[ index ]`, `char data[ 64 ]`.
  Empty brackets remain `[]`. Attach pointers/references to types: `const char* name`.
- Space around binary/assignment/conditional operators; attach unary operators to
  operands. Preserve `++index`, `!valid`, `&value`, and `*pointer` forms.
- Ordinary templates use `std::vector<entry_t>`; declarations use
  `template<typename type>` or the observed `template<typename... Args>` pack form.
  Named casts use `static_cast< uint32_t >( value )`, including interior angle spaces.
- Preserve the appropriate initialization forms: `= false`, `= nullptr`, `= {}`,
  `type{}`, `{ 0 }`, `{ first, second }` and `buffer( size )` all occur in Skeet.
- Expand long calls with the opening parenthesis on its own line, one argument per
  indented line and a closing `);`, as in `gui/overlay/overlay.cpp`.
  Expanded declarations can use the same arrangement. There is no arbitrary 80-column
  limit; never use that as an excuse to compress logic or obscure a simple expression.
- Lambda bodies are expanded; a callback invocation closes with `} );`.
  Switch cases are indented inside the switch and use expanded braced bodies, as in
  `gui/config/config.cpp`. `else`, `catch` and `default` start their own lines.
- Use short useful explanatory comments, field annotations and labeled local sections.
  Do not generate doc blocks, decorative banners or comments narrating every statement.
- Preserve CRLF in owned C++/batch files. Do not restyle SQL contents, persisted names,
  protocol values or diagnostic text merely because C++ identifiers are being improved.

### Actual reference examples

From Skeet `cheat/data/vector/vector.hpp` (unchanged):

```cpp
#ifndef CHEAT_DATA_VECTOR_HPP
#define CHEAT_DATA_VECTOR_HPP
//
//
//
#include "common.hpp"
//
//
namespace cheat::vector
{
	//
	struct memory_t
	{
		uint64_t start = 0ui64;
		uint64_t end = 0ui64;
		uint64_t capacity = 0ui64;
	};
	//
}
//
//
//
#endif // CHEAT_DATA_VECTOR_HPP
```

From Skeet `core/threads/threads.cpp` (unchanged):

```cpp
	bool is_running()
	{
		std::lock_guard<std::mutex> guard( threads_mutex );
		//
		if ( threads.empty() )
		{
			return false;
		}
		//
		for ( const auto& thread : threads )
		{
			if ( !thread->running.load() || !thread->handle.joinable() )
			{
				return false;
			}
		}
		//
		return true;
	}
```

### Resolving differences and reviewing changes

The dominant hand-written core/domain layout and the user's explicit instructions
resolve conflicting examples: use tabs despite the space-indented internal string
file; expand the compact lines in custom GUI/math code; use named casts and nullptr
rather than their GUI alternatives; retain semantic type-name distinctions.
Do not infer a style rule from misspellings, incidental mistakes or embedded bytes.

Before calling a change complete, inspect all changed C++ and compare it against the
examples and reference implementations. Check naming, logic, local state, ownership,
call flow, headers, separators and file placement, not only compilation.
Do not run a generic formatter until its exact output is verified on headers,
separators, namespaces, casts, lambdas, switches and long calls. Manual formatting is
required until that is demonstrated. Future sessions must follow this section even
when another model or formatter prefers a different convention.

## Authority and evidence

Use this hierarchy:

1. `AGENTS.md` defines engineering guardrails.
2. `docs/PRODUCT_GOAL.md` defines the product mission and completion semantics.
3. `docs/ARCHITECTURE.md` defines the intended long-term architecture.
4. `docs/CRITICAL_EDGE_CASES.md` defines edge cases that the architecture must account for to avoid false coverage/dependency claims.
5. `docs/HISTORICAL_AUDIT_NOTES.md` records prior evidence, useful Windows locations, and known failed approaches. It is historical input, not proof of current implementation.
6. `docs/VALIDATION_NOTES.md` records observed real-image/hardware behavior. It is evidence about those observed runs, not a universal guarantee or substitute for current source/CI inspection.
7. `README.md` documents current user-facing behavior.
8. Actual source, build scripts, CI/tests, and Git history establish what is implemented today.

Do not mistake a target requirement for an implemented feature. If documentation and implementation disagree, report the disagreement before changing architecture.

## Non-negotiable product goal

Nylon is an exhaustive, read-only, offline Windows-image evidence collector and semantic mapper. It is not a registry-only product.

The intended pipeline is:

```text
Windows ISO / WIM / ESD / SWM / expanded image
    -> source/media identity + selected-image preparation
    -> exhaustive Nylon evidence collection
    -> image.db (raw structural evidence)
    -> deterministic + local small-model semantic lifting
    -> image.map.db (derived readable semantic interpretation)
    -> Atlas native viewer (unrestricted whole-image exploration)
    -> safe portable export for a larger reasoning model
    -> evidence-backed stripped-image design
    -> modification + re-scan + verification
```

The final purpose is to understand an exact selected Windows image deeply enough to design the smallest practical stripped image while preserving explicitly retained functionality.

## Terminology that must stay precise

- **Original source/media evidence**: ISO/container/WIM metadata and the original selected-image content/metadata exposed by the source format.
- **Prepared image cache**: Nylon's immutable working view produced for fast local inspection. It is not automatically a lossless copy of every original metadata field.
- **`image.db`**: deterministic raw structural evidence and provenance. It must remain useful without an LLM.
- **`image.map.db`**: a derived semantic interpretation. It is not a replacement for raw evidence and must never be called infallible semantic truth.
- **Atlas**: a read-only representation/navigation layer over collected evidence. It does not define coverage.

The current cache is intentionally prepared with wimlib flags including `--no-acls --no-attributes --rpfix`. Therefore do not claim that the cache itself preserves original ACL/security-descriptor, attribute, or all reparse metadata fidelity. If those fields are required for whole-image coverage, collect them from an authoritative source representation or record them as not exposed/unsupported.

## Scope boundary: static image, not live host state

Nylon's image collector maps shipped/static composition of the selected offline image. Do not contaminate the audit with the host machine's runtime state.

Do not treat host/runtime concepts such as current PIDs, active sockets, live PnP devices, running services, current firewall/WFP runtime state, or post-install per-user state as facts about the offline image.

Files that physically exist in the image may still be inventoried; the rule is not to infer live runtime state from them.

## Source/media versus selected image

For ISO/container input:

- record source media/container identity and useful media/setup/boot metadata;
- record metadata for all available install images/indexes where exposed;
- perform exhaustive structural/semantic collection for the explicitly selected Windows image;
- do not silently expand every unrelated WIM index into a full semantic audit unless that becomes an explicit product requirement.

For a pre-expanded image, source-media facts that are unavailable must be marked unavailable rather than invented.

## Coverage rules

- Never call registry-only coverage `full image coverage`.
- `100%` must always name what is 100%: discovered objects represented, parse attempts accounted, semantic sources linked, etc.
- Raw-source accounting is not the same as full content interpretation. Linking a source reference proves representation/ownership accounting, not that every byte or field was semantically understood.
- A collector may declare complete only when its own discovery and representation invariant passes.
- Parse/semantic quality must be tracked separately from source accounting.
- Preserve failures and uncertainty explicitly (`PRESENT`, `NOT_PRESENT`, `QUERY_FAILED`, `ACCESS_DENIED`, `NOT_EXPOSED`, `UNSUPPORTED_FORMAT`, `PARSE_FAILED`, `UNRESOLVED`, or equivalent). Never convert a failed query into absence.
- Do not sacrifice raw-source coverage for semantic speed.
- Deterministic classification may replace model inference only when it preserves the raw evidence and does not invent unsupported meaning.
- The small model normalizes/labels evidence. It is not the authority for raw facts and is not the final removal-decision engine.
- Keep exact provenance from semantic entities and relationships back to raw evidence.
- Whole-image completion requires all required collector families to account for discovered evidence, including explicit gap states.

## Sensitive-data boundary

A full structural audit must not become a credential-harvesting system.

- Never intentionally extract password hashes, secrets, private keys, authentication tokens, or credential material merely to claim completeness.
- Treat SAM/SECURITY and other secret-bearing areas as sensitive evidence domains.
- If structural metadata from a sensitive area is needed, collect the minimum necessary facts and record redaction/omission explicitly.
- Do not send sensitive raw blobs to the semantic model unless an explicit reviewed policy says they are safe and necessary.
- The portable export intended for a larger/external model must exclude credential/secret material by default and must include a manifest of deliberate redactions/omissions.

The current implementation may not yet satisfy this policy everywhere. Treat that as a gap to identify, not permission to ignore it.

## Performance rules

Optimize primarily by avoiding unnecessary work:

- parse once and persist reusable facts;
- reuse immutable caches;
- do not rescan/replan unchanged evidence;
- build a foundational filesystem/source inventory once and let later collectors reference it rather than independently walking the same tree where practical;
- prefer deterministic parsers/recognizers over LLM calls when structure is authoritative;
- batch SQLite work;
- stream large datasets;
- use bounded buffers/worker pools;
- avoid duplicating payload bytes already present in the source/cache;
- hash selectively when hashes add provenance/relationship value rather than blindly hashing every payload;
- keep model/runtime resident when useful rather than restarting it;
- recompute only invalidated derived state;
- support resumable long-running work keyed to a verified source/audit identity;
- never add an arbitrary node/data cap merely to make the UI faster.

Pagination, viewport culling, spatial queries, level-of-detail and bounded batches are valid performance mechanisms because they change representation/work scheduling, not underlying reachability or coverage.

## Database and identity rules

Future whole-image work must not casually extend stale abstractions.

- Determine which current generic tables are active, legacy, or transitional before using them for new collectors.
- Prefer compact domain-specific tables when they are materially faster/clearer.
- Preserve canonical image-relative identities in addition to local row IDs.
- Design stable identifiers where practical for services, packages, components, tasks, GUID registrations, INF packages and relative file paths.
- Keep schema, collector, parser, model/prompt/ontology and derived-layout versions explicit.
- Derived databases/caches must be cryptographically or otherwise robustly bound to the exact source audit identity; path/size/timestamp alone must not be treated as a long-term content identity.
- Schema changes require an explicit compatibility/invalidation strategy.

## Parser and input-safety rules

Treat image contents as untrusted input even when the expected source is Microsoft media.

- never execute binaries/scripts from the image;
- never register offline COM servers, services, tasks, drivers, packages or AppX payloads;
- use bounds-checked parsers and explicit size/depth limits;
- disable external entity/network resolution in XML-style parsers;
- do not blindly follow reparse points;
- record reparse entries themselves as evidence where possible;
- use long-path-safe APIs;
- preserve original names/identities even when normalized comparison keys are also stored;
- encoding/conversion failures must become explicit evidence gaps, not silently dropped objects.

## Network behavior

Collection, parsing, Atlas viewing and local semantic inference should not require Internet access once explicitly bootstrapped dependencies/models are present.

- Do not add network lookups to improve evidence (for example online certificate revocation, web metadata or cloud enrichment) without an explicit separate feature/policy.
- Dependency/model/tool downloads belong to an explicit setup/bootstrap path, must be pinned/verified where the project already follows that pattern, and must not be confused with image collection.
- A scanner run must not contact Windows Update, package servers, certificate revocation services or other external endpoints merely because the scanned image references them.

## Viewer rules

The browser viewer is a prototype/transitional implementation, not the final product architecture.

The target Atlas viewer is a native Windows application:

- C++20
- Win32 window/input/lifecycle
- Direct3D 11 world renderer
- Direct2D overlays/UI
- DirectWrite text
- SQLite opened read-only
- deterministic hierarchical layout and level-of-detail
- spatial indexing / viewport-driven queries
- no continuous force physics
- no always-running render loop when the scene is idle
- no browser, HTTP server, JavaScript, Electron, Tauri, Qt, or WebView dependency in the final viewer
- no arbitrary node limit; all collected evidence must remain reachable through zoom, search, drill-down, pagination or raw inspection

Stable coordinates are preferred so the same component remains in the same place across sessions when its relevant source hierarchy has not changed.

Viewer/layout state must not mutate `image.db` or semantic evidence merely for UI convenience. Disposable layout/UI caches may be stored separately and versioned against the source/map identity.

## Build and active-code rules

`build.cmd`, `build-semantic.cmd`, `build-viewer.cmd` and CI workflows define the active portable build paths unless intentionally changed.

Do not infer that the highest-numbered or similarly named source file is active merely from its filename. Trace the build scripts/source-generation wrappers first. Old `semantic_v2.cpp`/`semantic_v3.cpp` or viewer wrapper files may remain as historical/transitional code while another translation unit is actually built.

Do not introduce another build system or heavyweight runtime merely for convenience without an architectural reason.

Keep dependency identities/pins/checksums reproducible. Do not silently upgrade third-party tools/models while implementing an unrelated milestone.

## Engineering workflow

Before coding a milestone:

1. establish the current active code path and existing invariant/tests;
2. state the exact completion invariant for the change;
3. preserve unrelated known-good behavior;
4. make the smallest coherent architectural change that satisfies the milestone;
5. build/test the actual affected targets;
6. inspect the resulting diff for accidental generated/binary/output files;
7. record any remaining gap explicitly.

For implementation work, prefer a clean branch/milestone boundary rather than mixing unrelated architecture changes together.

Do not:

- force-push/rewrite history without explicit user direction;
- run destructive repository cleanup/reset commands merely to solve a build problem;
- delete caches/output/user evidence unless the task explicitly requires it;
- commit generated binaries, model files, ISO caches or audit databases;
- change unrelated code while "cleaning up";
- claim CI success without actually observing the relevant run/result.

The first handoff/audit task in a new agent environment should be read-only until the current state has been reviewed.

## Validation levels

Keep validation claims precise:

- successful compilation proves compilation;
- synthetic CI smoke tests prove only the tested synthetic invariants;
- fake-model CI does not prove exact real-model grammar/quality or real GPU throughput;
- a real ISO/image run proves only what was observed on that image;
- a real hardware run proves only the exercised hardware/runtime path;
- whole-image completion is not proven until collector coverage manifests pass on the target image.

Performance work must be measured on representative real-image workloads when possible rather than inferred only from micro/synthetic tests.

## Current implementation reality

At present:

- ISO/selected-image preparation and a persistent immutable working cache exist;
- direct offline registry collection is the deeply implemented raw collector;
- registry semantic planning/lifting and exact imported-registry source accounting exist;
- the current web Atlas/progress UI is transitional;
- full filesystem/PE/servicing/task/AppX/etc. first-class collection is not yet complete;
- native Atlas has not replaced the web viewer.

Do not blur these facts in UI labels, documentation, progress indicators or completion claims.

## Known historical lessons

Read `docs/HISTORICAL_AUDIT_NOTES.md` before implementing new collector families.

In particular, earlier audit work found that most WinSxS `.manifest` files in the target Windows 11 family were not ordinary XML; an XML-only mapper left a large DCM/PA30 servicing-format gap. Do not repeat that assumption. Unsupported formats remain present evidence with explicit parse state.

Physical file/hardlink ownership, filename resemblance, thematic grouping and servicing/package ownership are different evidence classes. Do not collapse them into one claim.

## Modification safety

Collection and semantic mapping are read-only by default. Do not modify the source Windows image during audit/map phases.

The later stripping/modification system is a separate phase. Removal decisions must be based on evidence-backed dependency/ownership relationships and an explicit retained-function contract. Supported servicing/package removal must be distinguished from forced residual cleanup.

After modification, rerun Nylon against the produced image and compare the result to the source/retained-function contract. Never treat creation of a modified image as proof that the stripping plan was correct.