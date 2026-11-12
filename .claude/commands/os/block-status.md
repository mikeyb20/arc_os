Show where the build stands, from the block tables in `docs/plans/work-blocks.md` §4. Read only that file.

1. Count blocks per milestone by status (`todo`, `doing`, `review`, `done`) and session units done vs total
   (S = 0.5, M = 1, L = 2).
2. List blocks in `doing` or `review` (with PR numbers).
3. List the next blocks that can start: status `todo` and every block in Needs is `done` (or, for a stacked start,
   one same-lane parent in `review`; work-blocks §5). Show each block's lane (KA, KB, L, C, U, P, H). Mark which are on the
   critical path (§5) and their tier (R or D), so the right model is picked.
4. List open items for Michael from §6 whose block is next or in progress.

Report as one short table per section. Don't open block files or code.
