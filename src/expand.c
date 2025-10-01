#include "expand.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>

/* Grammar symbol ids (sym_word, sym_string, etc.) */
#include "ts_symbols.h"

/* For the tester main() only */
#include "tree_sitter/tree-sitter-bash.h"
#include <tree_sitter/api.h>   


/* =========================
 * Local helpers
 * ========================= */

static char *slice_text(const char *input, TSNode node) {
    uint32_t start = ts_node_start_byte(node);
    uint32_t end   = ts_node_end_byte(node);
    if (end < start) end = start;
    size_t n = (size_t)(end - start);
    char *s = (char *)malloc(n + 1);
    if (!s) return NULL;
    memcpy(s, input + start, n);
    s[n] = '\0';
    return s;
}

/* Always return a heap-allocated empty string if possible. */
static char *empty_heap_string(void) {
    char *s = (char *)malloc(1);
    if (s) s[0] = '\0';
    return s ? s : NULL;
}

/* Append bytes onto a growable buffer; returns 0 on success, -1 on OOM. */
static int append_bytes(char **out, size_t *len, size_t *cap, const char *src, size_t n) {
    if (!src || n == 0) return 0;
    /* Guard against overflow: new_len = *len + n + 1 */
    if (n > (size_t)-1 - *len - 1) return -1;
    if (*len + n + 1 > *cap) {
        size_t newcap = (*cap ? *cap : 64);
        while (*len + n + 1 > newcap) {
            if (newcap > (size_t)-1 / 2) { newcap = (size_t)-1; break; }
            newcap *= 2;
        }
        char *tmp = (char *)realloc(*out, newcap);
        if (!tmp) return -1;
        *out = tmp;
        *cap = newcap;
    }
    memcpy(*out + *len, src, n);
    *len += n;
    (*out)[*len] = '\0';
    return 0;
}

static char *strip_outer_quotes_dup(const char *raw, char quote_ch) {
    size_t L = strlen(raw);
    if (L >= 2 && raw[0] == quote_ch && raw[L - 1] == quote_ch) {
        size_t inner = L - 2;
        char *out = (char *)malloc(inner + 1);
        if (!out) return NULL;
        memcpy(out, raw + 1, inner);
        out[inner] = '\0';
        return out;
    } else {
        char *dup = (char *)malloc(L + 1);
        if (!dup) return NULL;
        memcpy(dup, raw, L + 1);
        return dup;
    }
}

/* ========== Simple/brace expansions ========== */

static char *expand_simple(TSNode simple_expansion, const char *input, int last_status, int *out_err) {
    if (out_err) *out_err = EXPAND_OK;
    /* Simple forms: "$$", "$?", "$VAR" */
    char *txt = slice_text(input, simple_expansion);
    if (!txt) { if (out_err) *out_err = EXPAND_OOM; return empty_heap_string(); }

    /* Short-circuit $$ and $? by text */
    if (strcmp(txt, "$$") == 0) {
        free(txt);
        char buf[32];
        snprintf(buf, sizeof buf, "%d", (int)getpid());
        char *out = strdup(buf);
        if (!out) { if (out_err) *out_err = EXPAND_OOM; return empty_heap_string(); }
        return out;
    }
    if (strcmp(txt, "$?") == 0) {
        free(txt);
        char buf[32];
        snprintf(buf, sizeof buf, "%d", last_status);
        char *out = strdup(buf);
        if (!out) { if (out_err) *out_err = EXPAND_OOM; return empty_heap_string(); }
        return out;
    }

    /* Otherwise expect a variable_name child */
    TSNode v = ts_node_named_child(simple_expansion, 0);
    free(txt);
    if (!ts_node_is_null(v) && ts_node_symbol(v) == sym_variable_name) {
        char *vname = slice_text(input, v);
        if (!vname) { if (out_err) *out_err = EXPAND_OOM; return empty_heap_string(); }
        const char *val = getenv(vname);
        free(vname);
        char *out = strdup(val ? val : "");
        if (!out) { if (out_err) *out_err = EXPAND_OOM; return empty_heap_string(); }
        return out;
    }

    /* Fallback: return raw text */
    char *raw = slice_text(input, simple_expansion);
    if (!raw) { if (out_err) *out_err = EXPAND_OOM; return empty_heap_string(); }
    return raw;
}

static char *expand_brace(TSNode expansion, const char *input, int *out_err) {
    if (out_err) *out_err = EXPAND_OK;
    /* ${VAR} → first named child should be variable_name */
    TSNode v = ts_node_named_child(expansion, 0);
    if (!ts_node_is_null(v) && ts_node_symbol(v) == sym_variable_name) {
        char *vname = slice_text(input, v);
        if (!vname) { if (out_err) *out_err = EXPAND_OOM; return empty_heap_string(); }
        const char *val = getenv(vname);
        free(vname);
        char *out = strdup(val ? val : "");
        if (!out) { if (out_err) *out_err = EXPAND_OOM; return empty_heap_string(); }
        return out;
    }
    /* Fallback: raw text */
    char *raw = slice_text(input, expansion);
    if (!raw) { if (out_err) *out_err = EXPAND_OOM; return empty_heap_string(); }
    return raw;
}

/* ========== Command substitution $( ... ) ========== */
/* We execute the *text inside* $( ... ) via /bin/sh -c "<inner>" and capture stdout. */

/* ========== Command substitution $( ... ) ========== */
/* We execute the *text inside* $( ... ) via /bin/sh -c "<inner>" and capture stdout. */
static char *capture_command_subst(TSNode sub, const char *input, int *out_err) {
    if (out_err) *out_err = EXPAND_OK;

    /* Extract raw node text and strip outer "$(" ... ")" */
    char *raw = slice_text(input, sub);
    if (!raw) { if (out_err) *out_err = EXPAND_OOM; return empty_heap_string(); }

    size_t L = strlen(raw);
    char *inner = NULL;
    if (L >= 3 && raw[0] == '$' && raw[1] == '(' && raw[L - 1] == ')') {
        size_t in_len = L - 3;
        inner = (char *)malloc(in_len + 1);
        if (!inner) { free(raw); if (out_err) *out_err = EXPAND_OOM; return empty_heap_string(); }
        memcpy(inner, raw + 2, in_len); /* skip "$(" */
        inner[in_len] = '\0';
    } else {
        /* Fallback: use raw as-is (probably still works) */
        inner = raw; raw = NULL;
    }
    free(raw);

    int fds[2];
    if (pipe(fds) != 0) {
        if (out_err) *out_err = EXPAND_SUBST_FAIL;
        free(inner);
        return empty_heap_string();
    }

    pid_t pid = fork();
    if (pid < 0) {
        if (out_err) *out_err = EXPAND_SUBST_FAIL;
        close(fds[0]); close(fds[1]);
        free(inner);
        return empty_heap_string();
    }
    if (pid == 0) {
        /* child: write to fds[1] */
        close(fds[0]);
        dup2(fds[1], STDOUT_FILENO);
        /* Optionally redirect stderr too? We'll leave it as-is. */
        close(fds[1]);

        /* exec /bin/sh -c "<inner>" */
        execl("/bin/sh", "sh", "-c", inner, (char *)NULL);
        _exit(127);
    }

    close(fds[1]);
    free(inner);

    /* parent: read all stdout */
    char *buf = NULL;
    size_t len = 0, cap = 0;
    char tmp[4096];
    ssize_t n;
    while ((n = read(fds[0], tmp, sizeof tmp)) > 0) {
        if (append_bytes(&buf, &len, &cap, tmp, (size_t)n) != 0) {
            close(fds[0]);
            (void)waitpid(pid, NULL, 0);
            if (out_err) *out_err = EXPAND_OOM;
            return empty_heap_string();
        }
    }
    close(fds[0]);
    (void)waitpid(pid, NULL, 0);

    if (!buf) {
        /* No output */
        return empty_heap_string();
    }

    /* Trim trailing newlines (bash behavior) */
    while (len > 0 && buf[len - 1] == '\n') {
        buf[--len] = '\0';
    }
    return buf;
}

/* ========== Render parts inside double quotes ========== */

static char *render_dq_string(TSNode s, const char *input, int last_status, int *out_err) {
    if (out_err) *out_err = EXPAND_OK;

    uint32_t m = ts_node_named_child_count(s);

    /* If no named children, strip outer quotes and preserve interior */
    if (m == 0) {
        char *raw = slice_text(input, s);
        if (!raw) { if (out_err) *out_err = EXPAND_OOM; return empty_heap_string(); }
        char *out = strip_outer_quotes_dup(raw, '"');
        free(raw);
        if (!out) { if (out_err) *out_err = EXPAND_OOM; return empty_heap_string(); }
        return out;
    }

    char *out = NULL;
    size_t len = 0, cap = 0;

    for (uint32_t j = 0; j < m; j++) {
        TSNode part = ts_node_named_child(s, j);
        int sym = ts_node_symbol(part);
        if (sym == sym_string_content) {
            char *seg = slice_text(input, part);
            if (!seg) { if (out_err) *out_err = EXPAND_OOM; free(out); return empty_heap_string(); }
            if (append_bytes(&out, &len, &cap, seg, strlen(seg)) != 0) {
                if (out_err) *out_err = EXPAND_OOM;
                free(seg); free(out);
                return empty_heap_string();
            }
            free(seg);
        } else if (sym == sym_expansion) { /* ${VAR} */
            int e = EXPAND_OK;
            char *v = expand_brace(part, input, &e);
            if (!v) { if (out_err) *out_err = EXPAND_OOM; free(out); return empty_heap_string(); }
            if (append_bytes(&out, &len, &cap, v, strlen(v)) != 0) {
                if (out_err) *out_err = EXPAND_OOM;
                free(v); free(out);
                return empty_heap_string();
            }
            free(v);
        } else if (sym == sym_simple_expansion) { /* $VAR, $?, $$ */
            int e = EXPAND_OK;
            char *v = expand_simple(part, input, last_status, &e);
            if (!v) { if (out_err) *out_err = EXPAND_OOM; free(out); return empty_heap_string(); }
            if (append_bytes(&out, &len, &cap, v, strlen(v)) != 0) {
                if (out_err) *out_err = EXPAND_OOM;
                free(v); free(out);
                return empty_heap_string();
            }
            free(v);
        } else if (sym == sym_command_substitution) { /* $(...) */
            int e = EXPAND_OK;
            char *v = capture_command_subst(part, input, &e);
            if (!v) { if (out_err) *out_err = EXPAND_OOM; free(out); return empty_heap_string(); }
            if (append_bytes(&out, &len, &cap, v, strlen(v)) != 0) {
                if (out_err) *out_err = EXPAND_OOM;
                free(v); free(out);
                return empty_heap_string();
            }
            free(v);
            if (e != EXPAND_OK && out_err) *out_err = e; /* propagate non-fatal info */
        } else {
            /* Unknown part inside quotes → include raw text literally */
            char *raw = slice_text(input, part);
            if (!raw) { if (out_err) *out_err = EXPAND_OOM; free(out); return empty_heap_string(); }
            if (append_bytes(&out, &len, &cap, raw, strlen(raw)) != 0) {
                if (out_err) *out_err = EXPAND_OOM;
                free(raw); free(out);
                return empty_heap_string();
            }
            free(raw);
        }
    }

    if (!out) return empty_heap_string();
    return out;
}

/* ========== Top-level single-arg expansion ========== */

char *expand_one_arg(TSNode node, const char *input, int last_status, int *out_err) {
    if (out_err) *out_err = EXPAND_OK;

    int sym = ts_node_symbol(node);
    switch (sym) {
        case sym_word: {
            char *raw = slice_text(input, node);
            if (!raw) { if (out_err) *out_err = EXPAND_OOM; return empty_heap_string(); }
            return raw;
        }
        case sym_raw_string: {
            char *raw = slice_text(input, node);
            if (!raw) { if (out_err) *out_err = EXPAND_OOM; return empty_heap_string(); }
            char *stripped = strip_outer_quotes_dup(raw, '\'');
            free(raw);
            if (!stripped) { if (out_err) *out_err = EXPAND_OOM; return empty_heap_string(); }
            return stripped;
        }
        case sym_string:
            return render_dq_string(node, input, last_status, out_err);
        case sym_simple_expansion:
            return expand_simple(node, input, last_status, out_err);
        case sym_expansion:
            return expand_brace(node, input, out_err);
        case sym_command_substitution:
            return capture_command_subst(node, input, out_err);
        default: {
            /* Unsupported → empty */
            char *s = empty_heap_string();
            if (!s && out_err) *out_err = EXPAND_OOM;
            return s ? s : strdup(""); /* last-ditch */
        }
    }
}

/* =========================
 * ARGV builder
 * ========================= */
static int node_is_argumenty(TSNode n) {
    int sym = ts_node_symbol(n);
    return (sym == sym_word ||
            sym == sym_raw_string ||
            sym == sym_string ||
            sym == sym_simple_expansion ||
            sym == sym_expansion ||
            sym == sym_command_substitution);
}

static int node_is_skip(TSNode n) {
    int sym = ts_node_symbol(n);
    return (sym == sym_file_redirect ||
            sym == sym_variable_assignment);
}

/* Prefer explicit command_name child if present; otherwise find first "argumenty" child. */
/* Prefer explicit command_name child if present; otherwise find first "argumenty" child. */
static TSNode find_program_name_node(TSNode command_node) {
    uint32_t n = ts_node_named_child_count(command_node);

    /* 1) Look inside 'command_name' for an argument-like token (word/string/expansion/etc.) */
    for (uint32_t i = 0; i < n; i++) {
        TSNode ch = ts_node_named_child(command_node, i);
        if (ts_node_symbol(ch) == sym_command_name) {
            uint32_t m = ts_node_named_child_count(ch);
            for (uint32_t j = 0; j < m; j++) {
                TSNode inner = ts_node_named_child(ch, j);
                if (node_is_argumenty(inner))
                    return inner;  /* good: actual token node */
            }
            /* No usable token inside command_name → fall through to step 2. */
            break;
        }
    }

    /* 2) Fallback: first argument-like child at the command level (skip redirects/assignments/command_name) */
    for (uint32_t i = 0; i < n; i++) {
        TSNode ch = ts_node_named_child(command_node, i);
        if (ts_node_symbol(ch) == sym_command_name) continue;
        if (!node_is_skip(ch) && node_is_argumenty(ch))
            return ch;
    }

    /* 3) Nothing suitable */
    TSNode null_node = (TSNode){0};
    return null_node;
}


char **expand_to_argv(TSNode command_node,
                      const char *input,
                      int last_status,
                      int *out_argc,
                      int *out_err) {
    if (out_err) *out_err = EXPAND_OK;
    if (out_argc) *out_argc = 0;

    if (ts_node_symbol(command_node) != sym_command) {
        /* Not a command node: signal failure (choose your policy; keeping OOM here is fine for now). */
        if (out_err) *out_err = EXPAND_OOM;
        return NULL;
    }

    TSNode prog_node = find_program_name_node(command_node);
    if (ts_node_is_null(prog_node)) {
        if (out_err) *out_err = EXPAND_OOM;
        return NULL;
    }

    /* Count argv entries: argv[0] for program + each argument-like child (excluding command_name container). */
    uint32_t n = ts_node_named_child_count(command_node);
    int argc = 1; /* program */
    for (uint32_t i = 0; i < n; i++) {
        TSNode ch = ts_node_named_child(command_node, i);
        if (ts_node_symbol(ch) == sym_command_name) continue; /* skip container entirely */
        if (node_is_skip(ch)) continue;
        if (node_is_argumenty(ch)) argc++;
    }

    char **argv = (char **)calloc((size_t)argc + 1, sizeof *argv);
    if (!argv) { if (out_err) *out_err = EXPAND_OOM; return NULL; }

    int idx = 0;

    /* argv[0] = expanded program name */
    int e0 = EXPAND_OK;
    argv[idx] = expand_one_arg(prog_node, input, last_status, &e0);
    if (!argv[idx]) {
        if (out_err) *out_err = EXPAND_OOM;
        free(argv);
        return NULL;
    }
    idx++;

    /* remaining args (skip command_name container; do NOT try to compare node equality with prog_node’s parent) */
    for (uint32_t i = 0; i < n; i++) {
        TSNode ch = ts_node_named_child(command_node, i);
        if (ts_node_symbol(ch) == sym_command_name) continue; /* skip container */
        if (node_is_skip(ch)) continue;
        if (!node_is_argumenty(ch)) continue;

        int e = EXPAND_OK;
        char *arg = expand_one_arg(ch, input, last_status, &e);
        if (!arg) {
            if (out_err) *out_err = EXPAND_OOM;
            /* free partial argv */
            for (int k = 0; k < idx; k++) free(argv[k]);
            free(argv);
            return NULL;
        }
        argv[idx++] = arg;
        if (e != EXPAND_OK && out_err) *out_err = e; /* propagate non-fatal info */
    }

    argv[idx] = NULL;
    if (out_argc) *out_argc = idx;
    return argv;
}

void free_argv(char **argv) {
    if (!argv) return;
    for (int i = 0; argv[i]; i++) free(argv[i]);
    free(argv);
}

