# VALIDATION comments

A VALIDATION comment is a greppable link from code to the document
clause that grounds the code's behavior. It replaces prose citations,
which age silently as documents evolve. The version token makes
staleness detectable: when a document's version changes, its tags can
be found, re-verified, and re-stamped.

## Provenance chain

Cite the most authoritative source that actually grounds the claim:

1. `Spec` — NMRA LCS-9.10.1 (`docs/lcs-9.10.1_cmrinet_v1.1.pdf`).
   The authoritative truth. Cite it when the spec states the behavior
   unambiguously.
2. `Interop` — `docs/cmrinet-interop-profile-and-errata.md`.
   Cite it when the spec is silent, contradicts itself, or fielded
   behavior diverges from the spec text. Each Interop rule and erratum
   carries the chain onward: it cites the spec by page and the
   evidence reviews by file.
3. `Design` — `docs/DESIGN.md`.
   Cite it for project decisions the spec does not determine: naming,
   allocation policy, injected time, geometry knobs, seam placement.

A claim grounded in a higher tier must not cite a lower one. A `Design`
tag asserts that the behavior is a project choice, not a protocol
obligation.

## Form

    // VALIDATION: <Doc> v<version> <clause>: <fact>

- `<Doc>` — `Spec`, `Interop`, or `Design` (see the chain above).
- `<version>` — the cited document's version: the spec's own version
  (`v1.1`), or the `Version:` line of the project document, at the time
  the tag was written or last verified.
- `<clause>` — a spec page or section (`p.5 §D.a`), an erratum or rule
  id (`E1`, `2.2.6`), a decision id (`D4`), or a quoted section name.
- `<fact>` — one or two sentences that state the behavior. The comment
  must stand alone if the document moves or the clause is renumbered.

Example:

    // VALIDATION: Interop v1.1 2.2.6: a receiver abandons a partial
    // frame when the inter-byte gap exceeds a local limit. The spec is
    // silent on recovery (erratum E6).

Use a tag where code makes a claim a document backs. Do not use tags
for work narration ("implemented per D3") — that history belongs in
the issue or PR, not in code.

## Maintenance

When a document's version changes:

1. Find its tags: `grep -rn "VALIDATION: Design" src tests`
2. Re-verify each tagged behavior against the new document text.
3. Re-stamp the tag's version, or change the code and the tag together.

A tag whose version lags the document marks unverified code, not
necessarily wrong code. The spec version changes only when the NMRA
publishes a revision; a spec revision obsoletes `Spec` tags and may
obsolete the Interop rules built on the old text.
