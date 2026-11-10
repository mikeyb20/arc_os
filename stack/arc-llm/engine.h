/*
 * engine.c — everything that touches llama.h: loading with the fit check, one context per
 * model, generation, embeddings, tokenization. Other parts of arc-llm never include llama.h.
 */
#ifndef ARCHOS_STACK_ARC_LLM_ENGINE_H
#define ARCHOS_STACK_ARC_LLM_ENGINE_H

#include "arc_llm.h"

typedef enum {
    ENGINE_OK,
    ENGINE_BAD_INPUT, /* the request can't be served by this model (400) */
    ENGINE_NO_RAM,    /* the fit check refused the load (507) */
    ENGINE_FAILED,    /* load or compute failed, or the model is misconfigured (500) */
} engine_status;

/* One chat message; strings are NUL-terminated and contain no NUL. */
typedef struct {
    const char *role;
    const char *content;
} engine_message;

#define ENGINE_RANDOM_SEED 0xFFFFFFFFu /* llama.cpp's LLAMA_DEFAULT_SEED: a random seed */

typedef struct {
    int32_t max_tokens; /* 0 = until the context is full */
    float temperature;  /* 0 = greedy */
    uint32_t seed;      /* for temperature > 0; ENGINE_RANDOM_SEED for a random one */
} engine_gen_params;

typedef struct {
    int32_t prompt_tokens;
    int32_t completion_tokens;
    const char *finish_reason; /* "stop" (end of generation), "length", or "cancelled" */
} engine_gen_result;

/* engine_piece_fn: receives each generated piece of text; returns false to stop (client gone). */
typedef bool (*engine_piece_fn)(void *user, const char *piece, size_t len);

/* engine_init: start llama.cpp's backend and route its log to stderr (warnings, errors, buffer sizes). */
void engine_init(const arc_llm_config *config);

/* engine_shutdown: free llama.cpp's backend; models must already be unloaded. */
void engine_shutdown(void);

/*
 * engine_load: load `m` if it isn't loaded. Fit check first: m->ram_mb + ram_mb of the models
 * already loaded in `r` + reserve_mb must not exceed RAM (sysinfo); else ENGINE_NO_RAM with the
 * numbers in `err`. Weights are mmapped read-only; one context with n_ctx = min(context_length, ctx_cap).
 */
engine_status engine_load(const llm_registry *r, llm_model *m, char *err, size_t errlen);

/* engine_unload: free the model's context and weights. */
void engine_unload(llm_model *m);

/*
 * engine_chat: build the prompt with the model's chat template, then generate, passing each
 * piece to `fn` until end of generation, max_tokens, a full context, `fn` returning false or
 * arc_llm_stopping. The context is cleared first.
 */
engine_status engine_chat(llm_model *m, const engine_message *msgs, size_t n_msgs, const engine_gen_params *p,
                          engine_piece_fn fn, void *user, engine_gen_result *res, char *err, size_t errlen);

/* engine_n_embd: the length of one embedding of `m` (loaded). */
int32_t engine_n_embd(const llm_model *m);

/* engine_embed: the L2-normalised embedding of `len` bytes of `text` into out[engine_n_embd(m)]. */
engine_status engine_embed(llm_model *m, const char *text, size_t len, float *out, int32_t *n_tokens, char *err,
                           size_t errlen);

/*
 * engine_tokenize: token IDs of `text` as llama-tokenize gives them (special tokens parsed, BOS if
 * the vocabulary adds one). *tokens is malloc'd; the caller frees it.
 */
engine_status engine_tokenize(llm_model *m, const char *text, size_t len, int32_t **tokens, int32_t *n_tokens,
                              char *err, size_t errlen);

#endif /* ARCHOS_STACK_ARC_LLM_ENGINE_H */
