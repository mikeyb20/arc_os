# /fresh-eyes — Implementation-Blind Code Assessment

## Instructions

You are assessing code with no knowledge of the feature's intent, plan, or
implementation decisions. You may see CLAUDE.md content about the project's
general architecture — that's fine. But you do NOT know:
- What specific feature was just built
- What the plan or spec says
- What implementation choices were made or why
- What tradeoffs were considered

This is intentional. Your job is to assess the code purely on what it
communicates through its structure, naming, comments, and logic. If you
can't figure out what it does or why, that's a finding — the code has
a clarity problem that will affect future maintainers.

## Step 1: Read the Code

Read the specified files. Do NOT read plan files, scratchpad, or any
external documentation. Only read what is in the source files themselves
(including inline comments and doc comments).

## Step 2: Assess

### What Does This Code Do?
Describe in your own words what you believe this code does.
Be specific. If parts are unclear, say so — "I can't determine what
this function is supposed to return when X happens" is a valid finding.

### What Looks Good?
- Clear abstractions or well-named interfaces
- Robust error handling
- Good separation of concerns
- Effective use of the type system
- Self-documenting code

### What Looks Fragile?
- Code that would break easily if assumptions change
- Hidden coupling between components
- Magic numbers or unexplained constants
- Complex control flow that's hard to follow
- Missing error handling on likely failure paths
- Implicit ordering dependencies

### What Would You Change?
If you were taking over maintenance of this code tomorrow:
- What would you refactor first?
- What would you want documented that isn't?
- What would you add tests for?
- What naming would you change for clarity?

## Step 3: Present

Format as a narrative, not a checklist. The user wants to understand
how the code reads to an outsider, not just a list of nitpicks.

End with: "My understanding of this code is: <one paragraph summary>.
Is that correct?" — if the user says no, the gap between intent and
expression is a documentation and naming problem worth addressing.
