/*
 * api.c — see api.h. Per request: one arcjson parse of the body into one arena and one response
 * writer, both freed before the next request; the generated text (non-streaming) in one growing
 * buffer. Model lookup scans the roles table, a handful of entries.
 */
#define _GNU_SOURCE
#include "api.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "arcjson.h"
#include "engine.h"
#include "roles.h"

#define ERR_MAX 512

static unsigned long api_request_id;

/* A byte buffer that grows: the generated text, and the bytes of a split UTF-8 character. */
typedef struct {
    char *p;
    size_t len;
    size_t cap;
    bool failed;
} text_buf;

static void text_put(text_buf *b, const char *s, size_t n) {
    if (b->failed || n == 0) {
        return;
    }
    if (b->len + n > b->cap) {
        size_t cap = b->cap ? b->cap : 256;
        while (cap < b->len + n) {
            cap *= 2;
        }
        char *p = realloc(b->p, cap);
        if (p == NULL) {
            b->failed = true;
            return;
        }
        b->p = p;
        b->cap = cap;
    }
    memcpy(b->p + b->len, s, n);
    b->len += n;
}

static const char *error_type(int status) {
    switch (status) {
    case 404:
        return "not_found_error";
    case 500:
        return "server_error";
    case 501:
        return "not_implemented_error";
    case 507:
        return "insufficient_memory_error";
    default:
        return "invalid_request_error";
    }
}

void api_error(http_conn *c, int status, const char *message) {
    arcjson_writer w;
    arcjson_writer_init(&w);
    arcjson_write_object_begin(&w);
    arcjson_write_key(&w, "error");
    arcjson_write_object_begin(&w);
    arcjson_write_key(&w, "message");
    arcjson_write_string(&w, message);
    arcjson_write_key(&w, "type");
    arcjson_write_string(&w, error_type(status));
    arcjson_write_object_end(&w);
    arcjson_write_object_end(&w);
    size_t len = 0;
    const char *body = arcjson_writer_result(&w, &len);
    if (body == NULL) {
        body = "{\"error\":{\"message\":\"out of memory\",\"type\":\"server_error\"}}";
        len = strlen(body);
    }
    http_respond(c, status, "application/json", body, len);
    arcjson_writer_free(&w);
}

/* send_json: the writer's document as a 200 response, or a 500 if building it failed. */
static void send_json(http_conn *c, arcjson_writer *w) {
    size_t len = 0;
    const char *body = arcjson_writer_result(w, &len);
    if (body == NULL) {
        api_error(c, 500, "out of memory building the response");
        return;
    }
    http_respond(c, 200, "application/json", body, len);
}

/* opt: member `key` of the request, or NULL if absent or null. */
static const arcjson_value *opt(const arcjson_value *req, const char *key) {
    const arcjson_value *v = arcjson_get(req, key);
    return v != NULL && v->type != ARCJSON_NULL ? v : NULL;
}

/* parse_request: the body as a JSON object, or NULL after sending 400. */
static const arcjson_value *parse_request(http_conn *c, arcjson_arena *arena) {
    arcjson_error jerr = {0, NULL};
    const arcjson_value *req = arcjson_parse(arena, c->body ? c->body : "", c->body_len, ARC_LLM_MAX_BODY, &jerr);
    char msg[ERR_MAX];
    if (req == NULL) {
        snprintf(msg, sizeof(msg), "invalid JSON at byte %zu: %s", jerr.offset, jerr.message);
        api_error(c, 400, msg);
        return NULL;
    }
    if (req->type != ARCJSON_OBJECT) {
        api_error(c, 400, "the request body must be a JSON object");
        return NULL;
    }
    return req;
}

/* status_of: the HTTP status for an engine result. */
static int status_of(engine_status st) {
    switch (st) {
    case ENGINE_OK:
        return 200;
    case ENGINE_BAD_INPUT:
        return 400;
    case ENGINE_NO_RAM:
        return 507;
    default:
        return 500;
    }
}

/* pick_model: the model the request names (role or name; `def` if none), loaded; NULL after an error response. */
static llm_model *pick_model(http_conn *c, llm_registry *r, const arcjson_value *req, const char *def) {
    const arcjson_value *v = opt(req, "model");
    const char *want = v ? arcjson_string(v) : def;
    char msg[ERR_MAX];
    if (want == NULL) {
        api_error(c, 400, "`model` must be a string: a role such as chat, or a model name");
        return NULL;
    }
    const char *why = NULL;
    llm_model *m = roles_lookup(r, want, &why);
    if (m == NULL) {
        snprintf(msg, sizeof(msg), "no model has role or name `%s`%s%s", want, r->error ? ": " : "",
                 r->error ? r->error : "");
        api_error(c, 404, msg);
        return NULL;
    }
    if (why != NULL) {
        api_error(c, 500, why);
        return NULL;
    }
    engine_status st = engine_load(r, m, msg, sizeof(msg));
    if (st != ENGINE_OK) {
        api_error(c, status_of(st), msg);
        return NULL;
    }
    return m;
}

/* write_model_fields: "model" and "model_version" of a response. */
static void write_model_fields(arcjson_writer *w, const llm_model *m) {
    arcjson_write_key(w, "model");
    arcjson_write_string(w, m->name);
    arcjson_write_key(w, "model_version");
    arcjson_write_string(w, m->version);
}

/* Chat: request fields, and the state of one generation. */
typedef struct {
    http_conn *c;
    const llm_model *m;
    bool stream;
    bool started;     /* stream: headers and the role chunk are sent */
    text_buf text;    /* non-streaming: the whole reply; streaming: a split UTF-8 character held back */
    text_buf scratch; /* streaming: held-back bytes plus the new piece */
    arcjson_writer w;
    char id[32];
    long long created;
} chat_state;

/* chat_chunk_begin: the fields every stream chunk starts with, up to the choice's "delta" value. */
static void chat_chunk_begin(chat_state *s) {
    arcjson_writer_reset(&s->w);
    arcjson_write_object_begin(&s->w);
    arcjson_write_key(&s->w, "id");
    arcjson_write_string(&s->w, s->id);
    arcjson_write_key(&s->w, "object");
    arcjson_write_string(&s->w, "chat.completion.chunk");
    arcjson_write_key(&s->w, "created");
    arcjson_write_int(&s->w, s->created);
    write_model_fields(&s->w, s->m);
    arcjson_write_key(&s->w, "choices");
    arcjson_write_array_begin(&s->w);
    arcjson_write_object_begin(&s->w);
    arcjson_write_key(&s->w, "index");
    arcjson_write_int(&s->w, 0);
    arcjson_write_key(&s->w, "delta");
}

/* chat_chunk_send: close the chunk with its finish_reason (NULL = null) and send it as an event. */
static void chat_chunk_send(chat_state *s, const char *finish_reason) {
    arcjson_write_key(&s->w, "finish_reason");
    if (finish_reason) {
        arcjson_write_string(&s->w, finish_reason);
    } else {
        arcjson_write_null(&s->w);
    }
    arcjson_write_object_end(&s->w);
    arcjson_write_array_end(&s->w);
    arcjson_write_object_end(&s->w);
    size_t len = 0;
    const char *json = arcjson_writer_result(&s->w, &len);
    if (json == NULL) {
        s->c->gone = true; /* out of memory: stop rather than send a broken stream */
        return;
    }
    http_sse_event(s->c, json, len);
}

/* chat_send_content: one stream chunk carrying `len` bytes of text (and the role on the first). */
static void chat_send_content(chat_state *s, const char *text, size_t len) {
    if (!s->started) {
        s->started = true;
        http_stream_begin(s->c, 200, "text/event-stream");
        chat_chunk_begin(s);
        arcjson_write_object_begin(&s->w);
        arcjson_write_key(&s->w, "role");
        arcjson_write_string(&s->w, "assistant");
        arcjson_write_key(&s->w, "content");
        arcjson_write_string(&s->w, "");
        arcjson_write_object_end(&s->w);
        chat_chunk_send(s, NULL);
    }
    if (len == 0) {
        return;
    }
    chat_chunk_begin(s);
    arcjson_write_object_begin(&s->w);
    arcjson_write_key(&s->w, "content");
    arcjson_write_string_n(&s->w, text, len);
    arcjson_write_object_end(&s->w);
    chat_chunk_send(s, NULL);
}

/* chat_piece: engine callback. Streams whole UTF-8 characters, holding back a split one. */
static bool chat_piece(void *user, const char *piece, size_t len) {
    chat_state *s = user;
    if (!s->stream) {
        text_put(&s->text, piece, len);
        return !s->text.failed;
    }
    s->scratch.len = 0;
    text_put(&s->scratch, s->text.p, s->text.len);
    text_put(&s->scratch, piece, len);
    if (s->scratch.failed) {
        return false;
    }
    size_t ready = arcjson_utf8_complete_len(s->scratch.p, s->scratch.len);
    s->text.len = 0;
    text_put(&s->text, s->scratch.p + ready, s->scratch.len - ready);
    if (ready > 0) {
        chat_send_content(s, s->scratch.p, ready);
    }
    return !s->c->gone;
}

/* chat_finish_stream: held-back bytes, the finish chunk with usage, then [DONE]. */
static void chat_finish_stream(chat_state *s, const engine_gen_result *res) {
    chat_send_content(s, s->text.p, s->text.len);
    chat_chunk_begin(s);
    arcjson_write_object_begin(&s->w);
    arcjson_write_object_end(&s->w);
    chat_chunk_send(s, res->finish_reason);
    http_sse_event(s->c, "[DONE]", 6);
    http_stream_end(s->c);
}

/* write_usage: the "usage" member. */
static void write_usage(arcjson_writer *w, int32_t prompt, int32_t completion, bool with_completion) {
    arcjson_write_key(w, "usage");
    arcjson_write_object_begin(w);
    arcjson_write_key(w, "prompt_tokens");
    arcjson_write_int(w, prompt);
    if (with_completion) {
        arcjson_write_key(w, "completion_tokens");
        arcjson_write_int(w, completion);
    }
    arcjson_write_key(w, "total_tokens");
    arcjson_write_int(w, (int64_t)prompt + completion);
    arcjson_write_object_end(w);
}

/* chat_finish_whole: the non-streaming response. */
static void chat_finish_whole(chat_state *s, const engine_gen_result *res) {
    arcjson_writer *w = &s->w;
    arcjson_writer_reset(w);
    arcjson_write_object_begin(w);
    arcjson_write_key(w, "id");
    arcjson_write_string(w, s->id);
    arcjson_write_key(w, "object");
    arcjson_write_string(w, "chat.completion");
    arcjson_write_key(w, "created");
    arcjson_write_int(w, s->created);
    write_model_fields(w, s->m);
    arcjson_write_key(w, "choices");
    arcjson_write_array_begin(w);
    arcjson_write_object_begin(w);
    arcjson_write_key(w, "index");
    arcjson_write_int(w, 0);
    arcjson_write_key(w, "message");
    arcjson_write_object_begin(w);
    arcjson_write_key(w, "role");
    arcjson_write_string(w, "assistant");
    arcjson_write_key(w, "content");
    arcjson_write_string_n(w, s->text.p ? s->text.p : "", s->text.len);
    arcjson_write_object_end(w);
    arcjson_write_key(w, "finish_reason");
    arcjson_write_string(w, res->finish_reason);
    arcjson_write_object_end(w);
    arcjson_write_array_end(w);
    write_usage(w, res->prompt_tokens, res->completion_tokens, true);
    arcjson_write_object_end(w);
    if (s->text.failed) {
        api_error(s->c, 500, "out of memory collecting the reply");
        return;
    }
    send_json(s->c, w);
}

/* chat_params: max_tokens, temperature, seed and stream; false after a 400. */
static bool chat_params(http_conn *c, const arcjson_value *req, engine_gen_params *p, bool *stream) {
    const arcjson_value *v;
    int64_t n = 0;
    p->max_tokens = 0;
    p->temperature = 0.8f; /* llama.cpp's default */
    p->seed = ENGINE_RANDOM_SEED;
    *stream = false;
    if ((v = opt(req, "max_tokens")) != NULL) {
        if (!arcjson_int64(v, &n) || n < 1 || n > INT32_MAX) {
            api_error(c, 400, "`max_tokens` must be an integer >= 1");
            return false;
        }
        p->max_tokens = (int32_t)n;
    }
    if ((v = opt(req, "temperature")) != NULL) {
        if (v->type != ARCJSON_NUMBER || v->u.number < 0.0 || v->u.number > 2.0) {
            api_error(c, 400, "`temperature` must be a number from 0 to 2");
            return false;
        }
        p->temperature = (float)v->u.number;
    }
    if ((v = opt(req, "seed")) != NULL) {
        if (!arcjson_int64(v, &n) || n < 0 || n >= (int64_t)ENGINE_RANDOM_SEED) {
            api_error(c, 400, "`seed` must be an integer from 0 to 4294967294");
            return false;
        }
        p->seed = (uint32_t)n;
    }
    if ((v = opt(req, "stream")) != NULL) {
        if (v->type != ARCJSON_TRUE && v->type != ARCJSON_FALSE) {
            api_error(c, 400, "`stream` must be true or false");
            return false;
        }
        *stream = v->type == ARCJSON_TRUE;
    }
    return true;
}

/* chat_messages: the `messages` array as engine messages (malloc'd); NULL after a 400. */
static engine_message *chat_messages(http_conn *c, const arcjson_value *req, size_t *n) {
    const arcjson_value *msgs = arcjson_get(req, "messages");
    if (msgs == NULL || msgs->type != ARCJSON_ARRAY || msgs->u.children.count == 0) {
        api_error(c, 400, "`messages` must be a non-empty array");
        return NULL;
    }
    engine_message *out = calloc(msgs->u.children.count, sizeof(*out));
    if (out == NULL) {
        api_error(c, 500, "out of memory");
        return NULL;
    }
    size_t i = 0;
    for (const arcjson_value *m = msgs->u.children.first; m != NULL; m = m->next, i++) {
        const arcjson_value *role = arcjson_get(m, "role");
        const arcjson_value *content = arcjson_get(m, "content");
        if (role == NULL || role->type != ARCJSON_STRING || content == NULL || content->type != ARCJSON_STRING ||
            strlen(role->u.string.ptr) != role->u.string.len ||
            strlen(content->u.string.ptr) != content->u.string.len) {
            free(out);
            api_error(c, 400, "each message needs a string `role` and `content`, without NUL characters");
            return NULL;
        }
        out[i].role = role->u.string.ptr;
        out[i].content = content->u.string.ptr;
    }
    *n = i;
    return out;
}

static void handle_chat(http_conn *c, llm_registry *r, const arcjson_value *req) {
    engine_gen_params params;
    chat_state s;
    memset(&s, 0, sizeof(s));
    size_t n_msgs = 0;
    if (!chat_params(c, req, &params, &s.stream)) {
        return;
    }
    engine_message *msgs = chat_messages(c, req, &n_msgs);
    s.m = msgs ? pick_model(c, r, req, "chat") : NULL;
    if (s.m == NULL) {
        free(msgs);
        return;
    }
    s.c = c;
    snprintf(s.id, sizeof(s.id), "chatcmpl-%lu", ++api_request_id);
    s.created = (long long)time(NULL);
    arcjson_writer_init(&s.w);
    engine_gen_result res;
    char err[ERR_MAX];
    engine_status st = engine_chat((llm_model *)s.m, msgs, n_msgs, &params, chat_piece, &s, &res, err, sizeof(err));
    if (st != ENGINE_OK && !s.started) {
        api_error(c, status_of(st), err);
    } else if (st != ENGINE_OK) {
        http_stream_end(c); /* headers went out already; a cut stream without [DONE] tells the client */
    } else if (s.stream) {
        chat_finish_stream(&s, &res);
    } else {
        chat_finish_whole(&s, &res);
    }
    if (strcmp(res.finish_reason ? res.finish_reason : "", "cancelled") == 0) {
        fprintf(stderr, "arc-llm: %s cancelled after %d tokens\n", s.id, res.completion_tokens);
    }
    arcjson_writer_free(&s.w);
    free(s.text.p);
    free(s.scratch.p);
    free(msgs);
}

/* embed_one: one input's embedding object into w; false after an error response. */
static bool embed_one(http_conn *c, llm_model *m, arcjson_writer *w, const arcjson_value *in, int64_t index, float *vec,
                      int64_t *tokens) {
    char err[ERR_MAX];
    int32_t n_tok = 0;
    engine_status st = engine_embed(m, in->u.string.ptr, in->u.string.len, vec, &n_tok, err, sizeof(err));
    if (st != ENGINE_OK) {
        char msg[ERR_MAX + 32];
        snprintf(msg, sizeof(msg), "input %lld: %s", (long long)index, err);
        api_error(c, status_of(st), msg);
        return false;
    }
    *tokens += n_tok;
    arcjson_write_object_begin(w);
    arcjson_write_key(w, "object");
    arcjson_write_string(w, "embedding");
    arcjson_write_key(w, "index");
    arcjson_write_int(w, index);
    arcjson_write_key(w, "embedding");
    arcjson_write_array_begin(w);
    for (int32_t i = 0, dim = engine_n_embd(m); i < dim; i++) {
        arcjson_write_float(w, vec[i]);
    }
    arcjson_write_array_end(w);
    arcjson_write_object_end(w);
    return true;
}

static void handle_embeddings(http_conn *c, llm_registry *r, const arcjson_value *req) {
    const arcjson_value *input = arcjson_get(req, "input");
    bool ok = input != NULL &&
              (input->type == ARCJSON_STRING || (input->type == ARCJSON_ARRAY && input->u.children.count > 0));
    for (const arcjson_value *v = ok && input->type == ARCJSON_ARRAY ? input->u.children.first : NULL; v != NULL;
         v = v->next) {
        ok = ok && v->type == ARCJSON_STRING;
    }
    if (!ok) {
        api_error(c, 400, "`input` must be a string or a non-empty array of strings");
        return;
    }
    llm_model *m = pick_model(c, r, req, "embed");
    if (m == NULL) {
        return;
    }
    if (!m->embedder) {
        char msg[ERR_MAX];
        snprintf(msg, sizeof(msg), "%s@%s is not an embedding model", m->name, m->version);
        api_error(c, 400, msg);
        return;
    }
    float *vec = malloc((size_t)engine_n_embd(m) * sizeof(*vec));
    arcjson_writer w;
    arcjson_writer_init(&w);
    arcjson_write_object_begin(&w);
    arcjson_write_key(&w, "object");
    arcjson_write_string(&w, "list");
    arcjson_write_key(&w, "data");
    arcjson_write_array_begin(&w);
    int64_t tokens = 0, index = 0;
    const arcjson_value *v = input->type == ARCJSON_STRING ? input : input->u.children.first;
    for (; v != NULL && ok; v = input->type == ARCJSON_STRING ? NULL : v->next) {
        ok = vec != NULL && embed_one(c, m, &w, v, index++, vec, &tokens);
    }
    if (vec == NULL) {
        api_error(c, 500, "out of memory");
    } else if (ok) {
        arcjson_write_array_end(&w);
        write_model_fields(&w, m);
        write_usage(&w, (int32_t)tokens, 0, false);
        arcjson_write_object_end(&w);
        send_json(c, &w);
    }
    arcjson_writer_free(&w);
    free(vec);
}

static void handle_tokenize(http_conn *c, llm_registry *r, const arcjson_value *req) {
    const arcjson_value *content = arcjson_get(req, "content");
    if (content == NULL || content->type != ARCJSON_STRING) {
        api_error(c, 400, "`content` must be a string");
        return;
    }
    llm_model *m = pick_model(c, r, req, "chat");
    if (m == NULL) {
        return;
    }
    int32_t *tokens = NULL, n = 0;
    char err[ERR_MAX];
    engine_status st = engine_tokenize(m, content->u.string.ptr, content->u.string.len, &tokens, &n, err, sizeof(err));
    if (st != ENGINE_OK) {
        api_error(c, status_of(st), err);
        return;
    }
    arcjson_writer w;
    arcjson_writer_init(&w);
    arcjson_write_object_begin(&w);
    arcjson_write_key(&w, "tokens");
    arcjson_write_array_begin(&w);
    for (int32_t i = 0; i < n; i++) {
        arcjson_write_int(&w, tokens[i]);
    }
    arcjson_write_array_end(&w);
    arcjson_write_object_end(&w);
    send_json(c, &w);
    arcjson_writer_free(&w);
    free(tokens);
}

static void handle_models(http_conn *c, llm_registry *r) {
    arcjson_writer w;
    arcjson_writer_init(&w);
    arcjson_write_object_begin(&w);
    arcjson_write_key(&w, "object");
    arcjson_write_string(&w, "list");
    arcjson_write_key(&w, "data");
    arcjson_write_array_begin(&w);
    for (const llm_model *m = r->models; m != NULL; m = m->next) {
        arcjson_write_object_begin(&w);
        arcjson_write_key(&w, "id");
        arcjson_write_string(&w, m->name);
        arcjson_write_key(&w, "object");
        arcjson_write_string(&w, "model");
        arcjson_write_key(&w, "version");
        arcjson_write_string(&w, m->version);
        arcjson_write_key(&w, "roles");
        arcjson_write_array_begin(&w);
        for (size_t i = 0; i < r->n_roles; i++) {
            if (r->roles[i].model == m) {
                arcjson_write_string(&w, r->roles[i].role);
            }
        }
        arcjson_write_array_end(&w);
        arcjson_write_key(&w, "ram_mb");
        arcjson_write_int(&w, m->ram_mb);
        arcjson_write_key(&w, "loaded");
        arcjson_write_bool(&w, m->model != NULL);
        if (m->error != NULL) {
            arcjson_write_key(&w, "error");
            arcjson_write_string(&w, m->error);
        }
        arcjson_write_object_end(&w);
    }
    arcjson_write_array_end(&w);
    arcjson_write_object_end(&w);
    send_json(c, &w);
    arcjson_writer_free(&w);
}

void api_handle(http_conn *c, llm_registry *r) {
    static const struct {
        const char *path;
        const char *method;
    } routes[] = {
        {"/v1/chat/completions", "POST"}, {"/v1/embeddings", "POST"}, {"/v1/tokenize", "POST"}, {"/v1/models", "GET"}};
    size_t route = 0;
    while (route < sizeof(routes) / sizeof(routes[0]) && strcmp(c->path, routes[route].path) != 0) {
        route++;
    }
    if (route == sizeof(routes) / sizeof(routes[0])) {
        api_error(c, 404, "unknown path");
        return;
    }
    if (strcmp(c->method, routes[route].method) != 0) {
        api_error(c, 405, routes[route].method[0] == 'G' ? "use GET" : "use POST");
        return;
    }
    roles_refresh(r);
    if (route == 3) {
        handle_models(c, r);
        return;
    }
    arcjson_arena arena;
    arcjson_arena_init(&arena);
    const arcjson_value *req = parse_request(c, &arena);
    if (req != NULL && route == 0) {
        handle_chat(c, r, req);
    } else if (req != NULL && route == 1) {
        handle_embeddings(c, r, req);
    } else if (req != NULL) {
        handle_tokenize(c, r, req);
    }
    arcjson_arena_free(&arena);
}
