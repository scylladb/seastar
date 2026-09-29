---
name: seastar-review
description: Seastar-specific code review rules. Use when reviewing a Seastar patch, commit, branch or pull request, or when self-checking a change before submitting it. Covers source-level backward compatibility, exporting public symbols from the C++ module (src/seastar.cppm), keeping internal symbols in an internal sub-namespace, and Doxygen documentation of public symbols.
---

# Seastar code review rules

These rules apply on top of general code review (correctness, style,
tests). They encode project policy that a reviewer unfamiliar with Seastar
would not know. Each rule says what to look for, and what the fix is.

Terminology used below:

- **Public header**: any header under `include/seastar/`. Headers under
  `src/`, `tests/`, `apps/` and `demos/` are not public.
- **Public symbol**: a namespace-scope entity (class, struct, union, enum,
  type alias, concept, function, function template, variable) declared in a
  public header, in a namespace other than an internal one (see rule 3).
- **Internal namespace**: a namespace named `internal` at any level, e.g.
  `seastar::internal`, `seastar::memory::internal`,
  `seastar::metrics::internal`. The `api_vN` sub-namespaces (API level
  machinery) are also internal.

See also `doc/compatibility.md`, which is the authoritative policy.

## 1. Source-level backward compatibility

Seastar is a library. Application code that builds against the current
version must keep building against the new one. Link (ABI) compatibility is
*not* maintained, so do not flag changes that are ABI-only (adding data
members, changing class layout, changing inline function bodies, changing a
function's mangled name without changing how it is called).

Binary protocols that Seastar exposes (e.g. the RPC wire format) must also
stay compatible; a change to what goes on the wire needs negotiation
(e.g. an RPC feature flag) so old and new peers interoperate.

Flag a change to a public symbol that can break existing callers, for
example:

- Removing or renaming a public symbol, a public/protected member, or a
  public header.
- Changing a function signature so that existing calls no longer compile or
  silently change meaning: removing a parameter, adding a parameter without
  a default, changing a parameter or return type to one that isn't
  compatible, changing a template parameter list, removing an overload,
  making a constructor or conversion operator `explicit`.
- Changing a type's capabilities: making it non-copyable or non-movable,
  removing a conversion, turning an unscoped `enum` into an `enum class`,
  removing or renumbering enumerators.
- Adding a pure virtual function, or changing the signature of a virtual
  function, in a class that applications derive from (e.g. `data_source_impl`,
  `data_sink_impl`, `file_impl`, `net::network_stack`). Existing
  implementations stop compiling or silently stop overriding.
- Tightening a concept or `requires` clause on a public template.
- Removing an `#include` from a public header that callers are likely to
  rely on transitively (lower severity; mention it, don't block on it).
- Changing documented behavior (semantics, ordering, thread/shard affinity,
  exception types thrown) even when the code still compiles.

Acceptable ways to evolve an API, which the reviewer should suggest:

- Add a new overload or a new function, and keep the old one.
- Mark the old API `[[deprecated("use X instead")]]` and keep it working for
  a transition period. The deprecation message should name the replacement.
- When moving a public header, leave a forwarding header at the old path.
- When a break is unavoidable, gate it behind a new `Seastar_API_LEVEL`
  using the `api_vN` sub-namespace mechanism, and update the API level list
  and the "API Level History" table in `doc/compatibility.md`.

Not subject to this rule (do not flag): symbols in internal namespaces,
`api_vN` namespaces, headers under `include/seastar/**/internal/`, anything
in `src/`, tests, apps and demos, and private class members.

## 2. Export public symbols from the C++ module

Seastar can be built as a C++20 module, defined in `src/seastar.cppm`. The
module exports symbols explicitly with using-declarations, so a new public
symbol that isn't listed there is invisible to module users.

When a change adds a public symbol (see Terminology), check that
`src/seastar.cppm` gains a matching line:

- Add `using seastar::<ns>::<name>;` in the `export namespace seastar::<ns> {`
  block for that namespace. Create the block if it doesn't exist.
- Keep namespace blocks and the using-declarations within each block sorted
  alphabetically, as the file already is.
- If the symbol lives in a public header not yet included by the module,
  add the `#include <seastar/...>` in the global module fragment (before
  `export module seastar;`), next to the other headers of the same
  directory.
- Specializations of `std` templates for Seastar types (e.g. `std::hash`)
  are declared in the `export namespace std` block.
- Members of an exported class don't need their own entry; only
  namespace-scope names do. Each overload set is exported by one
  using-declaration.
- When a change removes or renames a public symbol, the corresponding entry
  in `src/seastar.cppm` must be removed or renamed too, or the module build
  breaks.

Do not export internal symbols (rule 3). Templates in the module can still
use non-exported internal entities.

## 3. Internal symbols belong in an internal sub-namespace

Anything declared in a public header that applications are not meant to use
directly (implementation helpers, traits, base classes of public types,
free functions called only from other Seastar code) must be placed in an
internal namespace, so that it is clearly excluded from the compatibility
promise of rule 1 and from module exports of rule 2.

- Use `seastar::internal`, or `<subsystem>::internal` for code belonging to
  a subsystem namespace (e.g. `seastar::net::internal`,
  `seastar::memory::internal`). Don't put helpers directly in `seastar` or
  in a public subsystem namespace.
- For new code, prefer `internal` over the legacy `detail`, `details` and
  `impl` namespace names that some older headers still use. Don't ask for
  existing code to be renamed as part of an unrelated change.
- If every declaration in a new public header is internal, the header
  belongs in an `internal/` subdirectory, e.g.
  `include/seastar/core/internal/`. A header that mixes public and internal
  declarations is fine where it is.
- Internal declarations in public headers should be hidden from the Doxygen
  output, typically by wrapping them in `/// \cond internal` ...
  `/// \endcond`, following the surrounding file.
- Private class members, and code under `src/` (which can use an anonymous
  namespace or `internal`), are not covered by this rule.

## 4. Document public symbols with Doxygen

Every public symbol a change adds (see Terminology), and every public or
protected member of a public class, must have a Doxygen comment. When a
change modifies the signature or behavior of an existing public symbol, its
comment must be updated to match.

- Follow the comment style already used in the file (usually `///` lines;
  some files use `/** ... */`), and use the same commands as neighboring
  declarations (`\param`, `\return`, `\throws`, `\tparam`, `\brief`, ...).
- The comment should say what the symbol does and anything a caller must
  know that the signature doesn't express: preconditions, ownership and
  lifetime of arguments and returned objects, which shard it may be called
  on, whether it may block or yield, and what exceptions or failed futures
  it produces.
- A class comment should describe the class's purpose and usage; a function
  comment should document each parameter and the return value (including
  what a returned future resolves to) unless they're self-evident.
- If the file groups declarations into Doxygen modules (`\addtogroup`,
  `\ingroup`), new declarations should join the appropriate group.
- A deprecated symbol's comment should point to the replacement (in
  addition to the `[[deprecated]]` message).
- Internal symbols (rule 3) and private members don't need Doxygen
  comments, and should be excluded from the Doxygen output.

## Reporting

For each finding, cite the file and line, name the rule it violates, and
propose the concrete fix (the using-declaration to add, the namespace to
move a symbol into, the deprecated overload to keep, and so on). If none of
these rules is violated, say so briefly instead of inventing findings.
