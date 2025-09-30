/*
 * minibash - an open-ended subset of bash
 *
 * Developed by Godmar Back for CS 3214 Fall 2025 
 * Virginia Tech.
 */
#define _GNU_SOURCE    1
#include <stdio.h>
#include <readline/readline.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <termios.h>
#include <sys/wait.h>
#include <assert.h>

#include <tree_sitter/api.h>
#include "tree_sitter/tree-sitter-bash.h"
#include "ts_symbols.h"
/* Since the handed out code contains a number of unused functions. */
#pragma GCC diagnostic ignored "-Wunused-function"

#include "hashtable.h"
#include "signal_support.h"
#include "utils.h"
#include "list.h"
#include "ts_helpers.h"

/* These are field ids suitable for use in ts_node_child_by_field_id for certain rules. 
   e.g., to obtain the body of a while loop, you can use:
    TSNode body = ts_node_child_by_field_id(child, bodyId);
*/
static TSFieldId bodyId, redirectId, destinationId, valueId, nameId, conditionId;
static TSFieldId variableId;
static TSFieldId leftId, operatorId, rightId;

static char *input;         // to avoid passing the current input around
static TSParser *parser;    // a singleton parser instance 
static tommy_hashdyn shell_vars;        // a hash table containing the internal shell variables

static void handle_child_status(pid_t pid, int status);
static char *read_script_from_fd(int readfd);
static void execute_script(char *script);


static void handle_command(TSNode command_node);

static int last_status = 0; // [020]

static void
usage(char *progname)
{
    printf("Usage: %s -h\n"
        " -h            print this help\n",
        progname);

    exit(EXIT_SUCCESS);
}

/* Build a prompt */
static char *
build_prompt(void)
{
    return strdup("minibash> ");
}

/* Possible job status's to use.
 *
 * Some are specific to interactive job control which may not be needed
 * for this assignment.
 */
enum job_status {
    FOREGROUND,     /* job is running in foreground.  Only one job can be
                       in the foreground state. */
    BACKGROUND,     /* job is running in background */
    STOPPED,        /* job is stopped via SIGSTOP */
    NEEDSTERMINAL,  /* job is stopped because it was a background job
                       and requires exclusive terminal access */
    TERMINATED_VIA_EXIT,    /* job terminated via normal exit. */
    TERMINATED_VIA_SIGNAL   /* job terminated via signal. */
};

struct job {
    struct list_elem elem;   /* Link element for jobs list. */
    int     jid;             /* Job id. */
    enum job_status status;  /* Job status. */ 
    int  num_processes_alive;   /* The number of processes that we know to be alive */

    /* Add additional fields here as needed. */
};

/* Utility functions for job list management.
 * We use 2 data structures: 
 * (a) an array jid2job to quickly find a job based on its id
 * (b) a linked list to support iteration
 */
#define MAXJOBS (1<<16)
static struct list job_list;

static struct job *jid2job[MAXJOBS];

/* Return job corresponding to jid */
static struct job *
get_job_from_jid(int jid)
{
    if (jid > 0 && jid < MAXJOBS && jid2job[jid] != NULL)
        return jid2job[jid];

    return NULL;
}

/* Allocate a new job, optionally adding it to the job list. */
static struct job *
allocate_job(bool includeinjoblist)
{
    struct job * job = malloc(sizeof *job);
    job->num_processes_alive = 0;
    job->jid = -1;
    if (!includeinjoblist)
        return job;

    list_push_back(&job_list, &job->elem);
    for (int i = 1; i < MAXJOBS; i++) {
        if (jid2job[i] == NULL) {
            jid2job[i] = job;
            job->jid = i;
            return job;
        }
    }
    fprintf(stderr, "Maximum number of jobs exceeded\n");
    abort();
    return NULL;
}

/* Delete a job.
 * This should be called only when all processes that were
 * forked for this job are known to have terminated.
 */
static void
delete_job(struct job *job, bool removeFromJobList)
{
    if (removeFromJobList) {
        int jid = job->jid;
        assert(jid != -1);
        assert(jid2job[jid] == job);
        jid2job[jid]->jid = -1;
        jid2job[jid] = NULL;
    } else {
        assert(job->jid == -1);
    }
    /* add any other job cleanup here. */
    free(job);
}


/*
 * Suggested SIGCHLD handler.
 *
 * Call waitpid() to learn about any child processes that
 * have exited or changed status (been stopped, needed the
 * terminal, etc.)
 * Just record the information by updating the job list
 * data structures.  Since the call may be spurious (e.g.
 * an already pending SIGCHLD is delivered even though
 * a foreground process was already reaped), ignore when
 * waitpid returns -1.
 * Use a loop with WNOHANG since only a single SIGCHLD 
 * signal may be delivered for multiple children that have 
 * exited. All of them need to be reaped.
 */
static void
sigchld_handler(int sig, siginfo_t *info, void *_ctxt)
{
    pid_t child;
    int status;

    assert(sig == SIGCHLD);

    while ((child = waitpid(-1, &status, WUNTRACED|WNOHANG)) > 0) {
        handle_child_status(child, status);
    }
}

/* Wait for all processes in this job to complete, or for
 * the job no longer to be in the foreground.
 *
 * You should call this function from where you wait for
 * jobs started without the &; you would only use this function
 * if you were to implement the 'fg' command (job control only).
 * 
 * Implement handle_child_status such that it records the 
 * information obtained from waitpid() for pid 'child.'
 *
 * If a process exited, it must find the job to which it
 * belongs and decrement num_processes_alive.
 *
 * However, note that it is not safe to call delete_job
 * in handle_child_status because wait_for_job assumes that
 * even jobs with no more num_processes_alive haven't been
 * deallocated.  You should postpone deleting completed
 * jobs from the job list until when your code will no
 * longer touch them.
 *
 * The code below relies on `job->status` having been set to FOREGROUND
 * and `job->num_processes_alive` having been set to the number of
 * processes successfully forked for this job.
 */
static void
wait_for_job(struct job *job)
{
    assert(signal_is_blocked(SIGCHLD));

    while (job->status == FOREGROUND && job->num_processes_alive > 0) {
        int status;

        pid_t child = waitpid(-1, &status, WUNTRACED);

        // When called here, any error returned by waitpid indicates a logic
        // bug in the shell.
        // In particular, ECHILD "No child process" means that there has
        // already been a successful waitpid() call that reaped the child, so
        // there's likely a bug in handle_child_status where it failed to update
        // the "job" status and/or num_processes_alive fields in the required
        // fashion.
        // Since SIGCHLD is blocked, there cannot be races where a child's exit
        // was handled via the SIGCHLD signal handler.
        if (child != -1)
            handle_child_status(child, status);
        else
            utils_fatal_error("waitpid failed, see code for explanation");
    }
}


static void
handle_child_status(pid_t pid, int status)
{
    assert(signal_is_blocked(SIGCHLD));

    /* To be implemented. 
     * Step 1. Given the pid, determine which job this pid is a part of
     *         (how to do this is not part of the provided code.)
     * Step 2. Determine what status change occurred using the
     *         WIF*() macros.
     * Step 3. Update the job status accordingly, and adjust 
     *         num_processes_alive if appropriate.
     *         If a process was stopped, save the terminal state.
     */

}

// NEW: handle a single NAME=VALUE statement (032 only: plain word value)
static void handle_variable_assignment(TSNode assign_node) {            // [032]
    // Prefer grammar fields; fall back to first/second named child.
    TSNode varn = ts_node_child_by_field_id(assign_node, variableId);   // [032]
    TSNode valn = ts_node_child_by_field_id(assign_node, valueId);      // [032]
    if (ts_node_is_null(varn)) varn = ts_node_named_child(assign_node, 0); // [032]
    if (ts_node_is_null(valn)) valn = ts_node_named_child(assign_node, 1); // [032]

    char *vname = ts_extract_node_text(input, varn);                    // [032]
    char *vval  = ts_node_is_null(valn) ? strdup("")                    // [032]
                                        : ts_extract_node_text(input, valn); // [032]
    if (vname && vval) {
        /* Minimal for 032: set in process env so echo $VAR sees it.     */
        /* (We can switch to shell_vars later if a test requires it.)    */
        setenv(vname, vval, 1);                                         // [032]
    }
    free(vname);                                                         // [032]
    free(vval);                                                          // [032]
    last_status = 0;                                                     // [032]
}
  
// Capture the stdout of a command_substitution node "$( ... )" into a malloc'ed string.
// Caller must free the returned pointer. Trailing newlines are stripped.
static char *capture_command_subst(TSNode sub, char *input) {
    int fds[2];
    if (pipe(fds) != 0)
        return strdup("");

    pid_t pid = fork();
    if (pid == 0) {
        /* ---------- child: write end ---------- */
        close(fds[0]);
        dup2(fds[1], STDOUT_FILENO);
        close(fds[1]);

        // Find and execute the inner command
        uint32_t m = ts_node_named_child_count(sub);
        for (uint32_t i = 0; i < m; i++) {
            TSNode inner = ts_node_named_child(sub, i);
            if (ts_node_symbol(inner) == sym_command) {
                handle_command(inner);

                /* --- NEW: best-effort teardown in the child for clean Valgrind output --- */
                fflush(NULL);                // flush stdio to the pipe

                if (parser) {                // free Tree-sitter parser in child
                    ts_parser_delete(parser);
                    parser = NULL;
                }

                // free the shell variable table in child (mirrors main() teardown)
                tommy_hashdyn_foreach(&shell_vars, hash_free);
                tommy_hashdyn_done(&shell_vars);

                if (input) {                 // free script buffer copy
                    free(input);
                    input = NULL;
                }
                /* --- END NEW --- */

                break;
            }
        }
        _exit(0);
    }

    /* ---------- parent: read from pipe ---------- */
    close(fds[1]);
    char *buf = NULL;
    size_t len = 0, cap = 0;
    char tmp[4096];
    ssize_t n;

    while ((n = read(fds[0], tmp, sizeof tmp)) > 0) {
        if (len + (size_t)n + 1 > cap) {
            size_t newcap = cap ? cap : 64;
            while (len + (size_t)n + 1 > newcap) newcap *= 2;
            char *t = realloc(buf, newcap);
            if (!t) {
                free(buf);
                close(fds[0]);
                (void)waitpid(pid, NULL, 0);
                return strdup("");
            }
            buf = t;
            cap = newcap;
        }
        memcpy(buf + len, tmp, (size_t)n);
        len += (size_t)n;
        buf[len] = '\0';
    }
    close(fds[0]);
    (void)waitpid(pid, NULL, 0);

    if (!buf) return strdup("");

    // Trim trailing newlines (bash behavior)
    while (len > 0 && buf[len - 1] == '\n') {
        buf[--len] = '\0';
    }
    return buf;
}
// [040]


// NEW: expand a simple parameter expansion ($?, $$, $VAR) to a malloc'ed string.
// Caller must free the returned pointer. Returns strdup("") if unset/unknown.
static char *expand_simple(TSNode simple_expansion, char *input, int last_status) {   // [fixed: input is char *]
    char *txt = ts_extract_node_text(input, simple_expansion);                         // ok: expects char *
    if (!txt) return strdup("");

    if (strcmp(txt, "$$") == 0) {
        free(txt);
        char buf[32];
        snprintf(buf, sizeof buf, "%d", (int)getpid());
        return strdup(buf);
    }
    if (strcmp(txt, "$?") == 0) {
        free(txt);
        char buf[32];
        snprintf(buf, sizeof buf, "%d", last_status);
        return strdup(buf);
    }

    TSNode v = ts_node_named_child(simple_expansion, 0);
    if (!ts_node_is_null(v) && ts_node_symbol(v) == sym_variable_name) {
        char *vname = ts_extract_node_text(input, v);                                  // ok: expects char *
        free(txt);
        if (!vname) return strdup("");
        const char *val = getenv(vname);
        free(vname);
        return strdup(val ? val : "");
    }

    return txt;  // raw fallback
}


// NEW: expand a brace parameter expansion (${VAR}) to a malloc'ed string.
// Caller must free the returned pointer. Returns strdup("") if unset/unknown.
static char *expand_brace(TSNode expansion, char *input) {                                // [added]
    // ${VAR} -> first named child is variable_name                                         // [added]
    TSNode v = ts_node_named_child(expansion, 0);                                          // [added]
    if (!ts_node_is_null(v) && ts_node_symbol(v) == sym_variable_name) {                   // [added]
        char *vname = ts_extract_node_text(input, v);                                      // [added]
        if (!vname) return strdup("");                                                     // [added]
        const char *val = getenv(vname);                                                   // [added]
        free(vname);                                                                        // [added]
        return strdup(val ? val : "");                                                     // [added]
    }                                                                                      // [added]
    // Fallback: print the raw text literally                                               // [added]
    char *raw = ts_extract_node_text(input, expansion);                                    // [added]
    return raw ? raw : strdup("");                                                         // [added]
}

// NEW: grow-and-append helper to avoid macro pitfalls. Returns 0 on success, -1 on OOM.  // [fix]
static int append_bytes(char **out, size_t *len, size_t *cap, const char *src, size_t nbytes) { // [fix]
    if (!src || nbytes == 0) return 0;                                                   // [fix]
    if (*len + nbytes + 1 > *cap) {                                                      // [fix]
        size_t newcap = *cap ? *cap : 64;                                                // [fix]
        while (*len + nbytes + 1 > newcap) newcap *= 2;                                   // [fix]
        char *tmp = realloc(*out, newcap);                                               // [fix]
        if (!tmp) return -1;                                                             // [fix]
        *out = tmp;                                                                       // [fix]
        *cap = newcap;                                                                    // [fix]
    }                                                                                    // [fix]
    memcpy(*out + *len, src, nbytes);                                                    // [fix]
    *len += nbytes;                                                                       // [fix]
    (*out)[*len] = '\0';                                                                  // [fix]
    return 0;                                                                             // [fix]
}                                                                                        // [fix]
// Render a double-quoted string node (sym_string).
// Expands ${VAR} and $VAR/$?/$$, but most importantly: if the string has
// NO named children (e.g., it is just spaces), fall back to raw text and
// strip the quotes so interior spaces are preserved.
static char *render_dq_string(TSNode s, char *input, int last_status) {
    uint32_t m = ts_node_named_child_count(s);

    // Fallback: no named parts (e.g., string is just "  ")
    if (m == 0) {
        char *raw = ts_extract_node_text(input, s);
        if (!raw) return strdup("");
        size_t L = strlen(raw);
        if (L >= 2 && raw[0] == '"' && raw[L-1] == '"') {
            char *out = strndup(raw + 1, L - 2);
            free(raw);
            return out ? out : strdup("");
        }
        return raw; // unexpected, but fine
    }

    // Normal path: build from parts (string_content, expansions)
    char *out = NULL;
    size_t len = 0, cap = 0;

    for (uint32_t j = 0; j < m; j++) {
        TSNode part = ts_node_named_child(s, j);
        switch (ts_node_symbol(part)) {
            case sym_string_content: {
                char *seg = ts_extract_node_text(input, part);
                if (seg) {
                    append_bytes(&out, &len, &cap, seg, strlen(seg));
                    free(seg);
                }
                break;
            }
            case sym_expansion: { // ${VAR}
                char *v = expand_brace(part, input);
                if (v) { append_bytes(&out, &len, &cap, v, strlen(v)); free(v); }
                break;
            }
            case sym_simple_expansion: { // $VAR / $? / $$
                char *v = expand_simple(part, input, last_status);
                if (v) { append_bytes(&out, &len, &cap, v, strlen(v)); free(v); }
                break;
            }
            default: {
                // Safety fallback: include raw text of unknown part
                char *raw = ts_extract_node_text(input, part);
                if (raw) { append_bytes(&out, &len, &cap, raw, strlen(raw)); free(raw); }
                break;
            }
        }
    }

    if (!out) return strdup("");  // nothing appended
    return out;
}



// Render a single echo argument node into a malloc'ed string.
// Caller must free. Never returns NULL; returns strdup("") on error.
// NOTE: default branch returns token text **as-is** (no outer-quote stripping).
static char *render_arg(TSNode ch, char *input, int last_status) {
    switch (ts_node_symbol(ch)) {
        case sym_string: {
            return render_dq_string(ch, input, last_status);
        }
        case sym_raw_string: {
            char *text = ts_extract_node_text(input, ch);
            if (!text) return strdup("");
            size_t L = strlen(text);
            if (L >= 2 && text[0] == '\'' && text[L-1] == '\'') {
                char *out = strndup(text + 1, L - 2);
                free(text);
                return out ? out : strdup("");
            }
            return text;
        }
        case sym_simple_expansion: {
            char *v = expand_simple(ch, input, last_status);
            return v ? v : strdup("");
        }
        case sym_expansion: {
            char *v = expand_brace(ch, input);
            return v ? v : strdup("");
        }
        case sym_command_substitution: {                                 // [040]
            return capture_command_subst(ch, input);                      // [040]
        }
        default: {
            char *text = ts_extract_node_text(input, ch);
            return text ? text : strdup("");
        }
    }
}


static void handle_command(TSNode command_node) {
    TSNode name_node = ts_node_child_by_field_id(command_node, nameId);
    char *cmd = ts_extract_node_text(input, name_node);
    if (!cmd) return;

    /* ---- builtin: echo ---- */
    if (strcmp(cmd, "echo") == 0) {
        uint32_t n = ts_node_named_child_count(command_node);
        bool first = true;

        for (uint32_t i = 0; i < n; i++) {
            TSNode ch = ts_node_named_child(command_node, i);
            if (ts_node_eq(ch, name_node)) continue;

            char *out = render_arg(ch, input, last_status);                 // [changed] render first
            if (!out) out = strdup("");

            if (!first && out[0] != '\0') putchar(' ');                     // [changed] print separator only if arg non-empty
            fputs(out, stdout);                                             // [unchanged] print arg exactly as rendered
            first = first && (out[0] == '\0');                               // [changed] stay in "first" state if arg was empty
            free(out);
        }

        putchar('\n');
        free(cmd);
        last_status = 0;
        return;
    }
    /* ---- end builtin: echo ---- */

    /* externals (unchanged): words-only argv + execvp/execv */
    uint32_t named = ts_node_named_child_count(command_node);
    int word_argc = 0;
    for (uint32_t i = 0; i < named; i++) {
        TSNode ch = ts_node_named_child(command_node, i);
        if (ts_node_eq(ch, name_node)) continue;
        if (ts_node_symbol(ch) == sym_word) word_argc++;
    }

    char **argv = calloc((size_t)word_argc + 2, sizeof(char *));
    if (!argv) {
        argv = (char **)malloc(2 * sizeof(char *));
        if (!argv) { free(cmd); return; }
        argv[0] = cmd; argv[1] = NULL; word_argc = 0;
    } else {
        argv[0] = cmd;
        int idx = 1;
        for (uint32_t i = 0; i < named; i++) {
            TSNode ch = ts_node_named_child(command_node, i);
            if (ts_node_eq(ch, name_node)) continue;
            if (ts_node_symbol(ch) != sym_word) continue;
            char *arg = ts_extract_node_text(input, ch);
            if (arg) argv[idx++] = arg;
        }
        argv[word_argc + 1] = NULL;
    }

    int use_execvp = (strchr(cmd, '/') == NULL);
    pid_t pid = fork();

    if (pid == 0) {
        if (use_execvp) execvp(cmd, argv);
        else            execv(cmd, argv);
        _exit(127);
    } else {
        int status; (void)waitpid(pid, &status, 0);
        if (WIFEXITED(status))        last_status = WEXITSTATUS(status);
        else if (WIFSIGNALED(status)) last_status = 128 + WTERMSIG(status);
        else                          last_status = 1;
    }

    for (int i = 1; i <= word_argc; i++) free(argv[i]);
    free(argv);
    free(cmd);
}


/* --- helper: collect all command nodes in a pipeline --- */
static int collect_pipeline_commands(TSNode pipeline, TSNode **out_cmds) {
    int total_named = (int) ts_node_named_child_count(pipeline);

    /* First pass: count command children */
    int ncmds = 0;
    for (int i = 0; i < total_named; i++) {
        TSNode ch = ts_node_named_child(pipeline, (uint32_t)i);
        if (ts_node_symbol(ch) == sym_command)
            ncmds++;
    }

    if (ncmds == 0) {
        *out_cmds = NULL;
        return 0;
    }

    /* Second pass: collect them */
    TSNode *cmds = malloc((size_t)ncmds * sizeof *cmds);
    if (!cmds) {
        *out_cmds = NULL;
        return -1;                  /* signal OOM to caller */
    }

    int j = 0;
    for (int i = 0; i < total_named; i++) {
        TSNode ch = ts_node_named_child(pipeline, (uint32_t)i);
        if (ts_node_symbol(ch) == sym_command)
            cmds[j++] = ch;         /* TSNode is a small struct; copy by value */
    }

    *out_cmds = cmds;
    return ncmds;
}

/* --- helper: build argv from a command node (include command_name->word) --- */
static char **build_argv_words_only(TSNode command, char *input) {
    /* name: command_name -> word */
    TSNode name_node = ts_node_child_by_field_id(command, nameId);
    if (ts_node_is_null(name_node)) return NULL;

    TSNode name_word = ts_node_named_child(name_node, 0);
    if (ts_node_is_null(name_word) || ts_node_symbol(name_word) != sym_word)
        return NULL;

    /* count argv: 1 for the program name + each argument that is a plain word */
    uint32_t named = ts_node_named_child_count(command);
    int argc = 1;  // for program name
    for (uint32_t i = 0; i < named; i++) {
        TSNode ch = ts_node_named_child(command, i);
        if (ts_node_symbol(ch) == sym_word) argc++;
    }

    char **argv = calloc((size_t)argc + 1, sizeof *argv);
    if (!argv) return NULL;

    /* argv[0] = program name text */
    argv[0] = ts_extract_node_text(input, name_word);
    if (!argv[0]) { free(argv); return NULL; }

    /* append each argument word (top-level word children of the command) */
    int k = 1;
    for (uint32_t i = 0; i < named; i++) {
        TSNode ch = ts_node_named_child(command, i);
        if (ts_node_symbol(ch) != sym_word) continue;
        char *w = ts_extract_node_text(input, ch);
        if (!w) w = strdup("");
        argv[k++] = w;
    }
    argv[k] = NULL;
    return argv;
}

/* --- helper: free argv allocated by build_argv_words_only --- */
static void free_argv(char **argv) {
    if (!argv) return;
    for (int i = 0; argv[i]; i++) free(argv[i]);
    free(argv);
}

static void handle_pipeline(TSNode pipeline_node) {
    TSNode *cmds = NULL;
    int ncmds = collect_pipeline_commands(pipeline_node, &cmds);
    if (ncmds <= 0) return;

    char ***argvs = calloc((size_t)ncmds, sizeof *argvs);
    if (!argvs) { free(cmds); return; }

    for (int i = 0; i < ncmds; i++) {
        argvs[i] = build_argv_words_only(cmds[i], input);
        if (!argvs[i] || !argvs[i][0]) {
            for (int j = 0; j <= i; j++) free_argv(argvs[j]);
            free(argvs);
            free(cmds);
            return;
        }
    }

    int (*pipes)[2] = NULL;
    if (ncmds > 1) {
        pipes = calloc((size_t)(ncmds - 1), sizeof *pipes);
        if (!pipes) {
            for (int i = 0; i < ncmds; i++) free_argv(argvs[i]);
            free(argvs); free(cmds);
            return;
        }
        for (int i = 0; i < ncmds - 1; i++) {
            if (pipe(pipes[i]) != 0) {
                for (int j = 0; j < i; j++) { close(pipes[j][0]); close(pipes[j][1]); }
                for (int k = 0; k < ncmds; k++) free_argv(argvs[k]);
                free(pipes); free(argvs); free(cmds);
                return;
            }
        }
    }

    pid_t *pids = calloc((size_t)ncmds, sizeof *pids);
    if (!pids) {
        if (pipes) {
            for (int i = 0; i < ncmds - 1; i++) { close(pipes[i][0]); close(pipes[i][1]); }
            free(pipes);
        }
        for (int i = 0; i < ncmds; i++) free_argv(argvs[i]);
        free(argvs); free(cmds);
        return;
    }

    for (int i = 0; i < ncmds; i++) {
        pid_t pid = fork();
        if (pid == 0) {
            if (i > 0)          dup2(pipes[i-1][0], STDIN_FILENO);
            if (i < ncmds - 1)  dup2(pipes[i][1], STDOUT_FILENO);
            if (pipes) {
                for (int j = 0; j < ncmds - 1; j++) { close(pipes[j][0]); close(pipes[j][1]); }
            }
            execvp(argvs[i][0], argvs[i]);
            _exit(127);
        } else if (pid > 0) {
            pids[i] = pid;
        } else {
            /* fork failed: best-effort cleanup */
        }
    }

    if (pipes) {
        for (int i = 0; i < ncmds - 1; i++) { close(pipes[i][0]); close(pipes[i][1]); }
        free(pipes);
    }

    for (int i = 0; i < ncmds; i++) free_argv(argvs[i]);
    free(argvs);

    for (int i = 0; i < ncmds; i++) {
        int st;
        (void)waitpid(pids[i], &st, 0);
        if (i == ncmds - 1) {
            if (WIFEXITED(st)) last_status = WEXITSTATUS(st);
            else if (WIFSIGNALED(st)) last_status = 128 + WTERMSIG(st);
            else last_status = 1;
        }
    }
    free(pids);

    /* FIX: free the cmds array we allocated */
    free(cmds);
}



static void execute_node(TSNode child) {

    switch (ts_node_symbol(child)) {

        case sym_comment:
            break;
        
        case sym_variable_assignment:          // [032]
            handle_variable_assignment(child); // [032]
            break;  
        case sym_command:
            handle_command(child);
            break;
        case sym_pipeline:
            handle_pipeline(child);
            break;
        default:
            ts_print_node_info(child, "Unimplemented node");
            break;
    }
}
/*
 * Run a program.
 *
 * A program's named children are various types of statements which 
 * you can start implementing here.
 */
static void 
run_program(TSNode program)
{
    uint32_t n = ts_node_named_child_count(program);
    for (uint32_t i = 0; i < n; i++) {
        TSNode child = ts_node_named_child(program, i);
        execute_node(child);
    }
}

/*
 * Read a script from this (already opened) file descriptor,
 * return a newly allocated buffer.
 */
static char *
read_script_from_fd(int readfd)
{
    struct stat st;
    if (fstat(readfd, &st) != 0) {
        utils_error("Could not fstat input");
        return NULL;
    }

    char *userinput = malloc(st.st_size+1);
    if (read(readfd, userinput, st.st_size) != st.st_size) {
        utils_error("Could not read input");
        free(userinput);
        return NULL;
    }
    userinput[st.st_size] = 0;
    return userinput;
}


/* 
 * Execute the script whose content is provided in `script`
 */
static void 
execute_script(char *script)
{
    input = script;
    TSTree *tree = ts_parser_parse_string(parser, NULL, input, strlen(input));
    TSNode  program = ts_tree_root_node(tree);
    signal_block(SIGCHLD);
    run_program(program);
    signal_unblock(SIGCHLD);
    ts_tree_delete(tree);
}

int
main(int ac, char *av[])
{
    int opt;
    tommy_hashdyn_init(&shell_vars);

    /* Process command-line arguments. See getopt(3) */
    while ((opt = getopt(ac, av, "h")) > 0) {
        
        switch (opt) {
        case 'h':
            usage(av[0]);
            break;
        }
    }

    parser = ts_parser_new();
    const TSLanguage *bash = tree_sitter_bash();
#define DEFINE_FIELD_ID(name) \
    name##Id = ts_language_field_id_for_name(bash, #name, strlen(#name))
    DEFINE_FIELD_ID(body);
    DEFINE_FIELD_ID(condition);
    DEFINE_FIELD_ID(name);
    DEFINE_FIELD_ID(right);
    DEFINE_FIELD_ID(left);
    DEFINE_FIELD_ID(operator);
    DEFINE_FIELD_ID(value);
    DEFINE_FIELD_ID(redirect);
    DEFINE_FIELD_ID(destination);
    DEFINE_FIELD_ID(variable);
    ts_parser_set_language(parser, bash);

    list_init(&job_list);
    signal_set_handler(SIGCHLD, sigchld_handler);


    /* Read/eval loop. */
    bool shouldexit = false;
    for (;;) {
        if (shouldexit)
            break;

        /* If you fail this assertion, you were about to enter readline()
         * while SIGCHLD is blocked.  This means that your shell would be
         * unable to receive SIGCHLD signals, and thus would be unable to
         * wait for background jobs that may finish while the
         * shell is sitting at the prompt waiting for user input.
         */
        assert(!signal_is_blocked(SIGCHLD));

        char *userinput = NULL;
        /* Do not output a prompt unless shell's stdin is a terminal */
        if (isatty(0) && av[optind] == NULL) {
            char *prompt = isatty(0) ? build_prompt() : NULL;
            userinput = readline(prompt);
            free (prompt);
            if (userinput == NULL)
                break;
        } else {
            int readfd = 0;
            if (av[optind] != NULL)
                readfd = open(av[optind], O_RDONLY);

            userinput = read_script_from_fd(readfd);
            close(readfd);
            if (userinput == NULL)
                utils_fatal_error("Could not read input");
            shouldexit = true;
        }
        execute_script(userinput);
        free(userinput);
    }

    /* 
     * Even though it is not necessary for the purposes of resource
     * reclamation, we free all allocated data structure prior to exiting
     * so that we can use valgrind's leak checker.
     */
    ts_parser_delete(parser);
    tommy_hashdyn_foreach(&shell_vars, hash_free);
    tommy_hashdyn_done(&shell_vars);
    return EXIT_SUCCESS;
}