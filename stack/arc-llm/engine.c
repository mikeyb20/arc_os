/*
 * engine.c — arc-llm's use of llama.h (engine.h).
 *
 * Cost per loaded model: the mmapped weights (page cache, shared, read-only) and one context:
 * KV cache n_ctx x layers x KV width, plus compute buffers; llama.cpp logs their sizes at load
 * and we pass those lines to stderr. Per request: one prompt token buffer of at most n_ctx
 * tokens, freed at the end; per generated token one decode and one sampler call.
 */
#define _GNU_SOURCE
#include "engine.h"

#include <inttypes.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "arcutil.h"
#include "llama.h"

static arc_llm_config engine_config;

/* fail: format the error message for the caller; returns `status`. */
static engine_status fail(engine_status status, char *err, size_t errlen, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, errlen, fmt, ap);
    va_end(ap);
    return status;
}

/* engine_log: warnings and errors, and the buffer sizes llama.cpp reports at load (the memory a model costs). */
static void engine_log(enum ggml_log_level level, const char *text, void *user) {
    (void)user;
    if (level == GGML_LOG_LEVEL_WARN || level == GGML_LOG_LEVEL_ERROR ||
        (level == GGML_LOG_LEVEL_INFO && strstr(text, "buffer size =") != NULL)) {
        fputs(text, stderr);
    }
}

/* engine_abort: llama.cpp asks between compute steps; SIGTERM stops a long prompt early. */
static bool engine_abort(void *user) {
    (void)user;
    return arc_llm_stopping != 0;
}

void engine_init(const arc_llm_config *config) {
    engine_config = *config;
    llama_log_set(engine_log, NULL);
    llama_backend_init();
}

void engine_shutdown(void) {
    llama_backend_free();
}

engine_status engine_load(const llm_registry *r, llm_model *m, char *err, size_t errlen) {
    if (m->model != NULL) {
        return ENGINE_OK;
    }
    uint64_t loaded = 0;
    for (const llm_model *o = r->models; o != NULL; o = o->next) {
        if (o->model != NULL) {
            loaded += (uint64_t)o->ram_mb;
        }
    }
    uint64_t ram = arcutil_ram_mib();
    uint64_t need = (uint64_t)m->ram_mb + loaded + engine_config.reserve_mb;
    if (need > ram) {
        return fail(ENGINE_NO_RAM, err, errlen,
                    "%s@%s needs %" PRId64 " MiB; with %" PRIu64 " MiB of loaded models and %" PRIu64
                    " MiB reserved that is %" PRIu64 " MiB, more than the %" PRIu64 " MiB of RAM",
                    m->name, m->version, m->ram_mb, loaded, engine_config.reserve_mb, need, ram);
    }

    struct llama_model_params mp = llama_model_default_params();
    mp.n_gpu_layers = 0;
    mp.load_mode = LLAMA_LOAD_MODE_MMAP;
    m->model = llama_model_load_from_file(m->gguf_path, mp);
    if (m->model == NULL) {
        return fail(ENGINE_FAILED, err, errlen, "%s@%s: cannot load %s", m->name, m->version, m->gguf_path);
    }

    uint32_t n_ctx = engine_config.ctx_cap;
    if (m->context_length < (int64_t)n_ctx) {
        n_ctx = (uint32_t)m->context_length;
    }
    struct llama_context_params cp = llama_context_default_params();
    cp.n_ctx = n_ctx;
    cp.n_seq_max = 1;
    cp.n_threads = engine_config.threads;
    cp.n_threads_batch = engine_config.threads;
    cp.abort_callback = engine_abort;
    cp.abort_callback_data = NULL;
    if (m->embedder) {
        /* One input is one batch: non-causal embedders see the whole text at once. */
        cp.embeddings = true;
        cp.n_batch = n_ctx;
        cp.n_ubatch = n_ctx;
    } else if (cp.n_batch > n_ctx) {
        cp.n_batch = n_ctx;
    }
    m->ctx = llama_init_from_model(m->model, cp);
    if (m->ctx == NULL) {
        llama_model_free(m->model);
        m->model = NULL;
        return fail(ENGINE_FAILED, err, errlen, "%s@%s: cannot create a context of %u tokens", m->name, m->version,
                    n_ctx);
    }
    fprintf(stderr, "arc-llm: loaded %s@%s (%s), n_ctx %u, %d threads\n", m->name, m->version,
            m->embedder ? "embeddings" : "generation", llama_n_ctx(m->ctx), engine_config.threads);
    return ENGINE_OK;
}

void engine_unload(llm_model *m) {
    if (m->ctx != NULL) {
        llama_free(m->ctx);
        m->ctx = NULL;
    }
    if (m->model != NULL) {
        llama_model_free(m->model);
        m->model = NULL;
    }
}

/* clear_context: forget the previous request's tokens. */
static void clear_context(llm_model *m) {
    llama_memory_t mem = llama_get_memory(m->ctx);
    if (mem != NULL) {
        llama_memory_clear(mem, true);
    }
}

/*
 * tokenize: `len` bytes of `text` into a malloc'd array, special tokens parsed and BOS/EOS added
 * as the vocabulary says (llama-tokenize, llama-embedding and llama-server do the same).
 */
static engine_status tokenize(const llm_model *m, const char *text, size_t len, int32_t limit, llama_token **out,
                              int32_t *n_out, char *err, size_t errlen) {
    const struct llama_vocab *vocab = llama_model_get_vocab(m->model);
    if (len > INT32_MAX - 8) {
        return fail(ENGINE_BAD_INPUT, err, errlen, "text too long");
    }
    int32_t cap = (int32_t)len + 8; /* usually enough: about a byte per token at most, plus BOS/EOS/SEP */
    llama_token *tokens = malloc((size_t)cap * sizeof(*tokens));
    if (tokens == NULL) {
        return fail(ENGINE_FAILED, err, errlen, "out of memory");
    }
    int32_t n = llama_tokenize(vocab, text, (int32_t)len, tokens, cap, true, true);
    if (n < 0 && n != INT32_MIN) { /* more tokens than bytes: -n is the count it needs */
        llama_token *bigger = realloc(tokens, (size_t)-n * sizeof(*tokens));
        if (bigger != NULL) {
            tokens = bigger;
            cap = -n;
            n = llama_tokenize(vocab, text, (int32_t)len, tokens, cap, true, true);
        }
    }
    if (n < 0) {
        free(tokens);
        return fail(ENGINE_FAILED, err, errlen, "tokenizer failed (%d)", n);
    }
    if (limit > 0 && n > limit) {
        free(tokens);
        return fail(ENGINE_BAD_INPUT, err, errlen, "input is %d tokens; this model takes at most %d", n, limit);
    }
    *out = tokens;
    *n_out = n;
    return ENGINE_OK;
}

/* build_prompt: the chat template applied to the messages, malloc'd. */
static engine_status build_prompt(const llm_model *m, const engine_message *msgs, size_t n_msgs, char **prompt,
                                  char *err, size_t errlen) {
    const char *tmpl = m->chat_template ? m->chat_template : llama_model_chat_template(m->model, NULL);
    if (tmpl == NULL) {
        return fail(ENGINE_FAILED, err, errlen, "%s@%s has no chat template (model.json or GGUF)", m->name, m->version);
    }
    struct llama_chat_message *chat = calloc(n_msgs, sizeof(*chat));
    size_t total = 0;
    for (size_t i = 0; chat != NULL && i < n_msgs; i++) {
        chat[i].role = msgs[i].role;
        chat[i].content = msgs[i].content;
        total += strlen(msgs[i].role) + strlen(msgs[i].content);
    }
    if (chat == NULL || total > INT32_MAX / 2 - 1024) {
        free(chat);
        return fail(ENGINE_BAD_INPUT, err, errlen, "messages too long");
    }
    int32_t cap = (int32_t)(total * 2 + 1024);
    char *buf = malloc((size_t)cap);
    int32_t n = buf ? llama_chat_apply_template(tmpl, chat, n_msgs, true, buf, cap) : -1;
    if (buf != NULL && n >= cap) { /* the template added more than we allowed for */
        char *bigger = realloc(buf, (size_t)n + 1);
        n = bigger ? llama_chat_apply_template(tmpl, chat, n_msgs, true, bigger, n + 1) : -1;
        buf = bigger ? bigger : buf;
    }
    free(chat);
    if (n < 0) {
        free(buf);
        return fail(ENGINE_FAILED, err, errlen,
                    "%s@%s: llama.cpp does not recognise its chat template (only its built-in templates work)", m->name,
                    m->version);
    }
    buf[n] = '\0';
    *prompt = buf;
    return ENGINE_OK;
}

/* make_sampler: greedy at temperature 0, else llama.cpp's default chain (top-k 40, top-p 0.95, min-p 0.05). */
static struct llama_sampler *make_sampler(const engine_gen_params *p) {
    struct llama_sampler *chain = llama_sampler_chain_init(llama_sampler_chain_default_params());
    if (p->temperature <= 0.0f) {
        llama_sampler_chain_add(chain, llama_sampler_init_greedy());
        return chain;
    }
    llama_sampler_chain_add(chain, llama_sampler_init_top_k(40));
    llama_sampler_chain_add(chain, llama_sampler_init_top_p(0.95f, 1));
    llama_sampler_chain_add(chain, llama_sampler_init_min_p(0.05f, 1));
    llama_sampler_chain_add(chain, llama_sampler_init_temp(p->temperature));
    llama_sampler_chain_add(chain, llama_sampler_init_dist(p->seed));
    return chain;
}

/* decode_prompt: feed the prompt in n_batch pieces. */
static bool decode_prompt(llm_model *m, llama_token *tokens, int32_t n) {
    int32_t batch = (int32_t)llama_n_batch(m->ctx);
    for (int32_t i = 0; i < n; i += batch) {
        int32_t take = n - i < batch ? n - i : batch;
        if (llama_decode(m->ctx, llama_batch_get_one(tokens + i, take)) != 0) {
            return false;
        }
    }
    return true;
}

engine_status engine_chat(llm_model *m, const engine_message *msgs, size_t n_msgs, const engine_gen_params *p,
                          engine_piece_fn fn, void *user, engine_gen_result *res, char *err, size_t errlen) {
    memset(res, 0, sizeof(*res));
    if (m->embedder) {
        return fail(ENGINE_BAD_INPUT, err, errlen, "%s@%s is an embedding model", m->name, m->version);
    }
    char *prompt = NULL;
    engine_status st = build_prompt(m, msgs, n_msgs, &prompt, err, errlen);
    if (st != ENGINE_OK) {
        return st;
    }
    int32_t n_ctx = (int32_t)llama_n_ctx(m->ctx);
    llama_token *tokens = NULL;
    int32_t n_prompt = 0;
    st = tokenize(m, prompt, strlen(prompt), 0, &tokens, &n_prompt, err, errlen);
    free(prompt);
    if (st != ENGINE_OK) {
        return st;
    }
    res->prompt_tokens = n_prompt;
    if (n_prompt == 0 || n_prompt >= n_ctx) {
        free(tokens);
        return fail(ENGINE_BAD_INPUT, err, errlen, "prompt is %d tokens; the context holds %d", n_prompt, n_ctx);
    }

    clear_context(m);
    if (!decode_prompt(m, tokens, n_prompt)) {
        free(tokens);
        if (arc_llm_stopping) {
            res->finish_reason = "cancelled";
            return ENGINE_OK;
        }
        return fail(ENGINE_FAILED, err, errlen, "decoding the prompt failed");
    }
    free(tokens);

    const struct llama_vocab *vocab = llama_model_get_vocab(m->model);
    struct llama_sampler *sampler = make_sampler(p);
    int32_t limit = n_ctx - n_prompt;
    if (p->max_tokens > 0 && p->max_tokens < limit) {
        limit = p->max_tokens;
    }
    res->finish_reason = "length";
    int32_t piece_cap = 256;
    char *piece = malloc((size_t)piece_cap);
    while (piece != NULL && res->completion_tokens < limit) {
        llama_token tok = llama_sampler_sample(sampler, m->ctx, -1);
        if (llama_vocab_is_eog(vocab, tok)) {
            res->finish_reason = "stop";
            break;
        }
        int32_t n = llama_token_to_piece(vocab, tok, piece, piece_cap, 0, false);
        if (n < 0) { /* a piece longer than the buffer: -n is the size it needs */
            char *bigger = realloc(piece, (size_t)-n);
            if (bigger == NULL) {
                break;
            }
            piece = bigger;
            piece_cap = -n;
            n = llama_token_to_piece(vocab, tok, piece, piece_cap, 0, false);
        }
        res->completion_tokens++;
        if (n < 0 || !fn(user, piece, (size_t)n) || arc_llm_stopping) {
            res->finish_reason = "cancelled";
            break;
        }
        if (res->completion_tokens < limit && llama_decode(m->ctx, llama_batch_get_one(&tok, 1)) != 0) {
            res->finish_reason = "cancelled";
            break;
        }
    }
    free(piece);
    llama_sampler_free(sampler);
    return ENGINE_OK;
}

int32_t engine_n_embd(const llm_model *m) {
    return llama_model_n_embd_out(m->model);
}

engine_status engine_embed(llm_model *m, const char *text, size_t len, float *out, int32_t *n_tokens, char *err,
                           size_t errlen) {
    if (!m->embedder) {
        return fail(ENGINE_BAD_INPUT, err, errlen, "%s@%s is not an embedding model", m->name, m->version);
    }
    llama_token *tokens = NULL;
    int32_t n = 0;
    engine_status st = tokenize(m, text, len, (int32_t)llama_n_batch(m->ctx), &tokens, &n, err, errlen);
    if (st != ENGINE_OK) {
        return st;
    }
    if (n == 0) {
        free(tokens);
        return fail(ENGINE_BAD_INPUT, err, errlen, "empty input");
    }
    struct llama_batch batch = llama_batch_init(n, 0, 1);
    for (int32_t i = 0; i < n; i++) {
        batch.token[i] = tokens[i];
        batch.pos[i] = i;
        batch.n_seq_id[i] = 1;
        batch.seq_id[i][0] = 0;
        batch.logits[i] = 1;
    }
    batch.n_tokens = n;
    free(tokens);
    clear_context(m);
    int32_t rc = llama_decode(m->ctx, batch);
    llama_batch_free(batch);
    const float *e = rc == 0 ? llama_get_embeddings_seq(m->ctx, 0) : NULL;
    if (e == NULL) {
        return fail(ENGINE_FAILED, err, errlen, rc == 0 ? "%s@%s gives no pooled embedding" : "%s@%s: decode failed",
                    m->name, m->version);
    }
    int32_t dim = engine_n_embd(m);
    double sum = 0.0; /* as llama.cpp's common_embd_normalize (Euclidean) */
    for (int32_t i = 0; i < dim; i++) {
        sum += e[i] * e[i];
    }
    float scale = sum > 0.0 ? (float)(1.0 / sqrt(sum)) : 0.0f;
    for (int32_t i = 0; i < dim; i++) {
        out[i] = e[i] * scale;
    }
    *n_tokens = n;
    return ENGINE_OK;
}

engine_status engine_tokenize(llm_model *m, const char *text, size_t len, int32_t **tokens, int32_t *n_tokens,
                              char *err, size_t errlen) {
    return tokenize(m, text, len, 0, tokens, n_tokens, err, errlen);
}
