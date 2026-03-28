# /doc-decision — Record Architectural Decision

## Instructions

You are recording an architectural decision using the ADR (Architecture
Decision Record) format. ADRs are immutable once written — if a decision
is reversed, a new ADR supersedes the old one rather than editing it.

## Step 1: Determine Next Number

```bash
ls docs/decisions/ 2>/dev/null | sort -n | tail -1
```

Increment the highest number. If no decisions directory exists, create it
and start at 001.

## Step 2: Write ADR

Create `docs/decisions/<NNN>-<short-title>.md`:

```markdown
# <NNN>. <Title>

**Date:** <YYYY-MM-DD>
**Status:** accepted
**Related plan:** <link to plan file if applicable>

## Context

<What is the situation that requires a decision? What forces are at play?
What constraints exist?>

## Decision

<What is the decision? State it clearly and directly.>

## Alternatives Considered

### <Alternative 1>
- Pros: <list>
- Cons: <list>
- Why rejected: <reason>

### <Alternative 2>
<same format>

## Consequences

### Positive
- <what this enables or improves>

### Negative
- <tradeoffs accepted, costs incurred>

### Neutral
- <side effects that are neither good nor bad>
```

## Step 3: Cross-Reference

- If a plan file drove this decision, add a reference in the plan
- If this supersedes a previous ADR, add "Supersedes: <NNN>" to the status
  and update the old ADR's status to "superseded by <this NNN>"
- Commit: `docs: ADR <NNN> - <short title>`
