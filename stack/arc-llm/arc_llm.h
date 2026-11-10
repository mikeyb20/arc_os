/*
 * arc-llm — the model service (ADR-0012, M2-04). Shared types of its parts:
 *
 *   main.c    arguments, the socket, signals, the accept loop
 *   http.c    one HTTP/1.1 request per connection; plain and chunked responses
 *   roles.c   /models/roles.json and each model's model.json (the registry)
 *   engine.c  llama.h: load, fit check, generate, embed, tokenize
 *   api.c     the four endpoints, JSON in and out (arcjson)
 *
 * One request at a time, on the main thread; llama.cpp's thread pool does the compute.
 */
#ifndef ARCHOS_STACK_ARC_LLM_H
#define ARCHOS_STACK_ARC_LLM_H

#include <signal.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#include <time.h>

#define ARC_LLM_MAX_BODY (1u << 20) /* request bodies larger than this get 413 */

struct llama_model;
struct llama_context;

/* Set by SIGTERM/SIGINT (main.c); generation stops at the next token. */
extern volatile sig_atomic_t arc_llm_stopping;

/* Settings from the command line. */
typedef struct {
    const char *root;    /* installed system root; models live in <root>/models */
    int threads;         /* llama.cpp compute threads */
    uint32_t ctx_cap;    /* n_ctx = min(context_length, ctx_cap) */
    uint64_t reserve_mb; /* RAM kept back for the OS and page cache in the fit check */
} arc_llm_config;

/* One installed model named by roles.json; engine state is NULL until first use. */
typedef struct llm_model {
    char *name;
    char *version;
    char *gguf_path;     /* <root>/models/<name>/<version>/<file> */
    char *chat_template; /* model.json chat_template, NULL if absent (GGUF's own is used) */
    int64_t ram_mb;
    int64_t context_length;
    bool embedder;       /* model.json has `embedder`: loaded with an embeddings context */
    char *listed_roles;  /* model.json `roles` as "\nchat\nrouter\n" */
    char *error;         /* why model.json is unusable, NULL if fine */
    struct llama_model *model;
    struct llama_context *ctx;
    struct llm_model *next;
} llm_model;

/* A role from roles.json and the model filling it. */
typedef struct {
    char *role;
    llm_model *model;
    char *error; /* the model's model.json doesn't list this role; NULL if fine */
} llm_role;

/* roles.json and the models it names, re-read when the file changes. */
typedef struct {
    char *root;
    char *roles_path;
    bool seen; /* the file's identity below was recorded (it existed at the last check) */
    dev_t dev;
    ino_t ino;
    off_t size;
    struct timespec mtime;
    bool checked; /* at least one check has run */
    llm_role *roles;
    size_t n_roles;
    llm_model *models; /* each model once, in roles.json order */
    char *error;       /* why roles.json is unusable, NULL if fine */
} llm_registry;

#endif /* ARCHOS_STACK_ARC_LLM_H */
