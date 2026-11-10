/*
 * api.c — arc-llm's endpoints (ADR-0012): POST /v1/chat/completions, POST /v1/embeddings,
 * POST /v1/tokenize, GET /v1/models. OpenAI-style JSON; errors are
 * {"error":{"message":…,"type":…}} with a fitting status.
 */
#ifndef ARCHOS_STACK_ARC_LLM_API_H
#define ARCHOS_STACK_ARC_LLM_API_H

#include "arc_llm.h"
#include "http.h"

/* api_handle: answer the request read into `c`, after refreshing the registry from roles.json. */
void api_handle(http_conn *c, llm_registry *r);

/* api_error: send an error response. */
void api_error(http_conn *c, int status, const char *message);

#endif /* ARCHOS_STACK_ARC_LLM_API_H */
