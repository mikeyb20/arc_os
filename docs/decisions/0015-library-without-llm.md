# ADR-0015: The library works without the LLM

- **Status:** Accepted
- **Date:** 2026-11-07

## Decision
- Search (retriever) and the article reader are standalone programs and libraries that do not depend on `arc-llm`.
- The assistant is a layer on top: it uses the same retriever and adds generated, cited answers.
- If no model is installed, the model is too slow, or `arc-llm` is down, every pack is still searchable and readable.

## Consequences
- Rules out: features that only exist through the LLM for reading or finding content.

## Verification
- M3 done-test includes searching and reading with `arc-llm` stopped.
