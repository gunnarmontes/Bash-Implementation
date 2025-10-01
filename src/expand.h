#pragma once
#include <tree_sitter/api.h>

/* Non-fatal expansion diagnostics. */
typedef enum {
  EXPAND_OK = 0,
  EXPAND_OOM,
  EXPAND_SUBST_FAIL
} ExpandErr;

/* Expand a single argument-like node to a malloc'ed C string.
   Supports: word, raw_string ('...'), string ("..." with mixed parts),
             simple_expansion ($VAR, $?, $$), expansion (${VAR}),
             command_substitution ($( ... )).
   - Never returns NULL in normal operation; on internal OOM, attempts to return an empty heap string ("").
   - Caller must free the returned pointer.
   - If out_err != NULL, sets it to EXPAND_OK, EXPAND_OOM, or EXPAND_SUBST_FAIL
     (EXPAND_SUBST_FAIL is used only when spawning/pipe for $(...) fails). */
char *expand_one_arg(TSNode node, const char *input, int last_status, int *out_err);

/* Expand a full command node to a NULL-terminated argv array.
   - On success, returns argv and sets *out_argc to argc (if provided).
     Caller must free with free_argv().
   - Includes empty-string arguments when expansions yield "".
   - On hard failure (e.g., OOM while building argv), returns NULL and sets *out_err=EXPAND_OOM (if provided). */
char **expand_to_argv(TSNode command_node,
                      const char *input,
                      int last_status,
                      int *out_argc,
                      int *out_err);

/* Free a NULL-terminated argv previously returned by expand_to_argv().
   Safe to call with NULL. */
void free_argv(char **argv);
