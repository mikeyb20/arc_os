# /docs — Generate and Sync Documentation

## Instructions

You are performing a comprehensive documentation pass. This covers two tasks:
adding inline documentation to code, and syncing external architecture
documentation with the codebase.

## Part 1: Inline API Documentation

### Step 1.1: Identify Undocumented Interfaces

Scan the specified files (or recent diff if no files specified) for public
interfaces lacking documentation:
- C: public (non-static) functions in `.h` files without `/*` or `//` comment

### Step 1.2: Write Inline Documentation

For each undocumented interface, add documentation following the project's
existing style:

```c
/* <One-line summary of what this does>.
 *
 * <Longer description if behavior is non-obvious>
 *
 * @param <name>  <what it is, valid range, constraints>
 * @return <what is returned and under what conditions>
 */
```

## Part 2: Architecture Documentation Sync

### Step 2.1: Inventory Documentation

Scan `docs/` and README files for architecture-related content.

### Step 2.2: Compare Against Code

For each documentation file:
1. Verify documented claims against actual codebase
2. Check file paths, module responsibilities, interface signatures
3. Identify stale, missing, and inaccurate content

### Step 2.3: Update

- Remove or correct stale content
- Add documentation for new modules/components
- Update file paths, command examples, interface descriptions
- Match existing documentation style and granularity

## Part 3: Verify and Commit

### Step 3.1: Verify
- All file paths mentioned in docs exist
- All commands work
- All module names match actual names
- Style is consistent

### Step 3.2: Commit

```bash
git add <all documentation changes>
git commit -m "docs: update inline API docs and sync architecture documentation

Inline docs added/updated for:
- <module 1>
- <module 2>

Architecture docs synced:
- <doc file>: <what changed>"
```
