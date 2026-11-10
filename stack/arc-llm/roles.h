/*
 * roles.c — the model registry: <root>/models/roles.json (schemas/model-roles.schema.json) and the
 * model.json (schemas/model.schema.json) of every model it names, at <root>/models/<name>/<version>/.
 */
#ifndef ARCHOS_STACK_ARC_LLM_ROLES_H
#define ARCHOS_STACK_ARC_LLM_ROLES_H

#include "arc_llm.h"

/* roles_init: an empty registry for system root `root`; the first roles_refresh reads the files. */
void roles_init(llm_registry *r, const char *root);

/* roles_free: unload every model and free the registry. */
void roles_free(llm_registry *r);

/*
 * roles_refresh: stat roles.json (one stat per request); if it appeared, vanished or changed
 * (device, inode, size or mtime), re-read it and the model.json files it names. Models still
 * named keep their loaded state; models no role names any more are unloaded. A missing or
 * invalid file leaves no roles and sets r->error.
 */
void roles_refresh(llm_registry *r);

/*
 * roles_lookup: the model for `want`, a role (`chat`, `expert:medicine`) or else a model name
 * named by some role; NULL if neither. *why is set when that role or model can't be used
 * (bad model.json, or a role its model.json doesn't list).
 */
llm_model *roles_lookup(const llm_registry *r, const char *want, const char **why);

#endif /* ARCHOS_STACK_ARC_LLM_ROLES_H */
