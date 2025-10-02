/*
 * minibash - an open-ended subset of bash
 *
 * Developed by Godmar Back for CS 3214 Fall 2025 
 * Virginia Tech.
 */


#define _GNU_SOURCE   
 
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

#include "expand.h"
#include "tree_sitter/tree-sitter-bash.h"
#include "ts_symbols.h"
/* Since the handed out code contains a number of unused functions. */
#pragma GCC diagnostic ignored "-Wunused-function"

#include "hashtable.h"
#include "signal_support.h"
#include "utils.h"
#include "list.h"
#include "ts_helpers.h"


/* -------- debug helper -------- */
#ifndef MB_DEBUG
#define MB_DEBUG 1   /* set to 0 to mute (or compile with -DMB_DEBUG=0) */
#endif

#if MB_DEBUG
  #define DBG(...)  fprintf(stderr, __VA_ARGS__)
  #define DFD(...)  dprintf(2, __VA_ARGS__)
#else
  #define DBG(...)  ((void)0)
  #define DFD(...)  ((void)0)
#endif


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
static int run_command_with_io(TSNode cmd, int in_fd /*-1*/, int out_fd /*-1*/);


static int  apply_command_redirections(TSNode command_node);
static void exec_command_in_child(TSNode command_node);
static int  run_pipeline_with_io(TSNode pipeline_node, int pipe_in_fd, int pipe_out_fd);

static void handle_redirected_statement(TSNode rs);

static int eval_node_status(TSNode n);
static int eval_andor(TSNode andor_node);


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

static void handle_command(TSNode command_node) {
    int argc = 0;
    int err  = EXPAND_OK;

    /* Build argv with full expansion. */
    char **argv = expand_to_argv(command_node, input, last_status, &argc, &err);
    if (!argv || argc == 0 || !argv[0]) {
        /* Nothing to run or expansion failed. Choose status policy. */
        last_status = 1;
        if (argv) free_argv(argv);
        return;
    }

    /* Builtin: echo (already expanded) */
    if (strcmp(argv[0], "echo") == 0) {
        /* Print argv[1..] separated by a single space; trailing newline. */
        for (int i = 1; i < argc; i++) {
            if (i > 1) fputc(' ', stdout);
            fputs(argv[i], stdout);
        }
        fputc('\n', stdout);
        free_argv(argv);
        last_status = 0;
        return;
    }

    /* External command */
    pid_t pid = fork();
    if (pid == 0) {
        /* child */
        if (strchr(argv[0], '/') == NULL) {
            execvp(argv[0], argv);
        } else {
            execv(argv[0], argv);
        }
        _exit(127); /* exec failed */
    }

    /* parent */
    int st = 0;
    (void)waitpid(pid, &st, 0);
    if (WIFEXITED(st))        last_status = WEXITSTATUS(st);
    else if (WIFSIGNALED(st)) last_status = 128 + WTERMSIG(st);
    else                      last_status = 1;

    free_argv(argv);
}



static int collect_pipeline_commands(TSNode pipeline, TSNode **out_cmds) {
    int total_named = (int) ts_node_named_child_count(pipeline);

    int ncmds = 0;
    for (int i = 0; i < total_named; i++) {
        TSNode ch = ts_node_named_child(pipeline, (uint32_t)i);
        if (ts_node_symbol(ch) == sym_command)
            ncmds++;
    }

    if (ncmds == 0) {
        *out_cmds = NULL;
        DBG("[COLLECT] ncmds=0\n");
        return 0;
    }

    TSNode *cmds = malloc((size_t)ncmds * sizeof *cmds);
    if (!cmds) {
        *out_cmds = NULL;
        return -1;
    }

    int j = 0;
    for (int i = 0; i < total_named; i++) {
        TSNode ch = ts_node_named_child(pipeline, (uint32_t)i);
        if (ts_node_symbol(ch) == sym_command)
            cmds[j++] = ch;
    }

    *out_cmds = cmds;
    DBG("[COLLECT] ncmds=%d\n", ncmds);
    return ncmds;
}

static char **build_argv_words_only(TSNode command, char *input) {
    TSNode name_node = ts_node_child_by_field_id(command, nameId);
    if (ts_node_is_null(name_node)) return NULL;

    TSNode name_word = ts_node_named_child(name_node, 0);
    if (ts_node_is_null(name_word) || ts_node_symbol(name_word) != sym_word)
        return NULL;

    uint32_t named = ts_node_named_child_count(command);
    int argc = 1;  // program name
    for (uint32_t i = 0; i < named; i++) {
        TSNode ch = ts_node_named_child(command, i);
        if (ts_node_symbol(ch) == sym_word)
            argc++;
    }

    char **argv = calloc((size_t)argc + 1, sizeof *argv);
    if (!argv) return NULL;

    argv[0] = ts_extract_node_text(input, name_word);
    if (!argv[0]) { free(argv); return NULL; }

    int k = 1;
    for (uint32_t i = 0; i < named; i++) {
        TSNode ch = ts_node_named_child(command, i);
        if (ts_node_symbol(ch) != sym_word) continue;
        char *w = ts_extract_node_text(input, ch);
        if (!w) w = strdup("");
        argv[k++] = w;
    }
    argv[k] = NULL;

#if MB_DEBUG
    for (int a = 0; argv[a]; a++)
        DBG("[ARGV] argv[%d] = '%s'\n", a, argv[a]);
#endif

    return argv;
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

/* Run a single command node assuming stdio is already set up (dup2 done).
   This is used by pipeline children and by the fork in run_command_with_io(). */
static void exec_command_in_child(TSNode command_node) {
    /* apply per-command redirects first */
    if (apply_command_redirections(command_node) != 0) {
        /* flush stdio before exiting the child */
        fflush(NULL);
        _exit(1);
    }

    int argc = 0, err = EXPAND_OK;
    char **argv = expand_to_argv(command_node, input, last_status, &argc, &err);
    if (!argv || argc == 0 || !argv[0]) {
        /* nothing to exec (or expansion error) */
        if (argv) free_argv(argv);
        _exit(127);
        return; /* not reached */
    }

    /* builtin: echo (args already expanded) */
    if (strcmp(argv[0], "echo") == 0) {
        for (int i = 1; i < argc; i++) {
            if (i > 1) (void)!write(STDOUT_FILENO, " ", 1);
            size_t L = strlen(argv[i]);
            if (L) (void)!write(STDOUT_FILENO, argv[i], L);
        }
        (void)!write(STDOUT_FILENO, "\n", 1);
        free_argv(argv);
        _exit(0);
    }

    /* external */
    if (strchr(argv[0], '/') == NULL) {
        execvp(argv[0], argv);
    } else {
        execv(argv[0], argv);
    }

    /* exec failed */
    free_argv(argv);
    fflush(NULL);
    _exit(127);
}

/* Run a pipeline with optional overall in/out FDs (apply to first/last stage).
   Uses words-only argv/exec for externals and builtin echo via exec_command_in_child. */
static int run_pipeline_with_io(TSNode pipeline_node, int pipe_in_fd, int pipe_out_fd) {
    TSNode *cmds = NULL;
    int n = collect_pipeline_commands(pipeline_node, &cmds);
    if (n <= 0) return 0;

    int (*pipes)[2] = NULL;
    if (n > 1) {
        pipes = calloc((size_t)(n - 1), sizeof *pipes);
        if (!pipes) { free(cmds); return 1; }
        for (int i = 0; i < n - 1; i++) {
            if (pipe(pipes[i]) != 0) {
                for (int k = 0; k < i; k++) { close(pipes[k][0]); close(pipes[k][1]); }
                free(pipes); free(cmds);
                return 1;
            }
        }
    }

    pid_t *pids = calloc((size_t)n, sizeof *pids);
    if (!pids) {
        if (pipes) { for (int i = 0; i < n - 1; i++) { close(pipes[i][0]); close(pipes[i][1]); } free(pipes); }
        free(cmds);
        return 1;
    }

    for (int i = 0; i < n; i++) {
        pid_t pid = fork();
        if (pid == 0) {
            DFD("[PL] child[%d] pid=%d\n", i, (int)getpid());

            /* stdin */
            if (i == 0) {
                if (pipe_in_fd != -1) {
                    DFD("[PL] stage0 dup2(%d->0)\n", pipe_in_fd);
                    dup2(pipe_in_fd, STDIN_FILENO);
                }
            } else {
                DFD("[PL] stage%d dup2(%d->0)\n", i, pipes[i-1][0]);
                dup2(pipes[i-1][0], STDIN_FILENO);
            }

            /* stdout */
            if (i == n - 1) {
                if (pipe_out_fd != -1) {
                    DFD("[PL] stageLast dup2(%d->1)\n", pipe_out_fd);
                    dup2(pipe_out_fd, STDOUT_FILENO);
                }
            } else {
                DFD("[PL] stage%d dup2(%d->1)\n", i, pipes[i][1]);
                dup2(pipes[i][1], STDOUT_FILENO);
            }

            /* close all pipe fds in child */
            if (pipes) {
                for (int j = 0; j < n - 1; j++) {
                    close(pipes[j][0]); close(pipes[j][1]);
                }
            }
            if (i == 0 && pipe_in_fd  != -1)  close(pipe_in_fd);
            if (i == n-1 && pipe_out_fd != -1) close(pipe_out_fd);

            DFD("[PL] child[%d] fds wired, exec...\n", i);
            exec_command_in_child(cmds[i]); /* never returns on success */
            _exit(127);
        }
        pids[i] = pid;
    }

    if (pipes) {
        for (int i = 0; i < n - 1; i++) { close(pipes[i][0]); close(pipes[i][1]); }
        free(pipes);
    }
    free(cmds);

    DBG("[PL] parent waiting for %d stages\n", n);
    int st = 0;
    for (int i = 0; i < n; i++) {
        int cur = 0;
        (void)waitpid(pids[i], &cur, 0);
        DBG("[PL] waitpid pid=%d status=%d%s\n", (int)pids[i], cur, i==n-1 ? " (last)" : "");
        if (i == n - 1) st = cur;
    }
    free(pids);

    if (WIFEXITED(st))        last_status = WEXITSTATUS(st);
    else if (WIFSIGNALED(st)) last_status = 128 + WTERMSIG(st);
    else                      last_status = 1;
    return last_status;
}

/* =========================
 * COMMAND EXECUTION HELPERS
 * ========================= */

/* Run a single command with optional in/out FDs.
   Returns the command’s exit status (0..255) and updates last_status. */
static int run_command_with_io(TSNode cmd, int in_fd, int out_fd) {
    pid_t pid = fork();
    if (pid == 0) {
        if (in_fd  != -1) dup2(in_fd,  STDIN_FILENO);
        if (out_fd != -1) dup2(out_fd, STDOUT_FILENO);
        exec_command_in_child(cmd);   /* never returns on success */
        _exit(127);
    }

    int st = 0;
    (void)waitpid(pid, &st, 0);
    if (WIFEXITED(st))        last_status = WEXITSTATUS(st);
    else if (WIFSIGNALED(st)) last_status = 128 + WTERMSIG(st);
    else                      last_status = 1;
    return last_status;
}

/* ======== REDIRECTS FOR A SINGLE COMMAND (used inside pipeline/exec) ======== */
/* Return 0 on success, -1 on error (prints a message and _exit(1) in child). */
/* prototype goes near the top: static int apply_command_redirections(TSNode); */
/* ======== REDIRECTS FOR A SINGLE COMMAND (used inside pipeline/exec) ======== */
/* Return 0 on success, -1 on error (prints a message and _exit(1) in child). */
static int apply_command_redirections(TSNode command_node) {
    uint32_t n = ts_node_named_child_count(command_node);
    for (uint32_t i = 0; i < n; i++) {
        TSNode ch = ts_node_named_child(command_node, i);
        if (ts_node_symbol(ch) != sym_file_redirect)
            continue;

        char *redir_txt = ts_extract_node_text(input, ch);
        if (!redir_txt) redir_txt = strdup("");
        const char *p = redir_txt;
        while (*p == ' ' || *p == '\t') p++;
        char op1 = *p;
        char op2 = (p[0] && p[1]) ? p[1] : '\0';
        int is_input  = (op1 == '<');
        int is_append = (op1 == '>' && op2 == '>');

        TSNode dest = ts_node_child_by_field_id(ch, destinationId);
        char *path = ts_extract_node_text(input, dest);
        if (!path) path = strdup("");

        DBG("[CR] op='%c%c' path='%s'\n", op1, op2 ? op2 : ' ', path);

        int fd = -1;
        if (is_input) {
            fd = open(path, O_RDONLY);
            if (fd < 0) {
                utils_error("minibash: cannot open for input: %s", path);
                free(path); free(redir_txt);
                return -1;
            }
            if (dup2(fd, STDIN_FILENO) < 0) {
                close(fd); free(path); free(redir_txt);
                return -1;
            }
            DBG("[CR] dup2(%d -> STDIN) ok\n", fd);
            close(fd);
        } else {
            int flags = O_WRONLY | O_CREAT | (is_append ? O_APPEND : O_TRUNC);
            fd = open(path, flags, 0666);
            if (fd < 0) {
                utils_error("minibash: cannot open for output: %s", path);
                free(path); free(redir_txt);
                return -1;
            }
            if (dup2(fd, STDOUT_FILENO) < 0) {
                close(fd); free(path); free(redir_txt);
                return -1;
            }
            DBG("[CR] dup2(%d -> STDOUT) ok\n", fd);
            close(fd);
        }

        free(path);
        free(redir_txt);
    }
    return 0;
}


static int eval_node_status(TSNode n) {
    switch (ts_node_symbol(n)) {
        case sym_command:
            handle_command(n);
            return last_status;

        case sym_pipeline:
            (void)run_pipeline_with_io(n, -1, -1);
            return last_status;

        case sym_redirected_statement:
            handle_redirected_statement(n);
            return last_status;

        case sym_list: {
    uint32_t m = ts_node_named_child_count(n);
    if (m == 0) { last_status = 0; return last_status; }

    /* Evaluate the first chunk. */
    TSNode prev = ts_node_named_child(n, 0);
    int status = eval_node_status(prev);

    /* Walk the rest, inspecting the operator text between prev and cur. */
    for (uint32_t i = 1; i < m; i++) {
        TSNode cur = ts_node_named_child(n, i);

        uint32_t prev_end  = ts_node_end_byte(prev);
        uint32_t cur_start = ts_node_start_byte(cur);
        const char *seg    = input + prev_end;
        size_t seglen      = (cur_start > prev_end) ? (size_t)(cur_start - prev_end) : 0;

        /* Find the operator between prev and cur: &&, ||, ;, & (ignore whitespace/newlines). */
        const char *p = seg, *pend = seg + seglen;
        int is_and = 0, is_or = 0, is_semi = 0, is_bg = 0;
        while (p < pend) {
            if (p + 1 < pend && p[0] == '&' && p[1] == '&') { is_and = 1; break; }
            if (p + 1 < pend && p[0] == '|' && p[1] == '|') { is_or  = 1; break; }
            if (*p == ';') { is_semi = 1; break; }
            if (*p == '&') { is_bg   = 1; break; }  /* not fully implemented here */
            p++;
        }

        /* Decide whether to run the right child based on the operator. */
        int run_right = 1;
        if (is_and)      run_right = (status == 0);
        else if (is_or)  run_right = (status != 0);
        else if (is_semi || is_bg) run_right = 1;  /* sequencing */

        if (run_right) {
            status = eval_node_status(cur);
        } else {
            /* short-circuited: keep previous status; skip cur */
        }

        prev = cur;
    }

    last_status = status;
    return last_status;
}


        /* If your generated headers have an explicit sym_and_or, prefer it. */
#ifdef sym_and_or
        case sym_and_or:
            return eval_andor(n);
#endif

#ifdef sym_subshell
        case sym_subshell: {
            uint32_t m = ts_node_named_child_count(n);
            for (uint32_t i = 0; i < m; i++) {
                TSNode ch = ts_node_named_child(n, i);
                execute_node(ch);
            }
            return last_status;
        }
#endif

        /* Some grammars represent &&/|| as a binary_expression with fields. */
#ifdef sym_binary_expression
        case sym_binary_expression:
            return eval_andor(n);
#endif

        default: {
            /* Fallback: if the node has an operator field, treat it as and/or. */
            TSNode opn = ts_node_child_by_field_id(n, operatorId);
            if (!ts_node_is_null(opn)) {
                return eval_andor(n);
            }
            ts_print_node_info(n, "eval_node_status: unimplemented node");
            last_status = 1;
            return last_status;
        }
    }
}


static int eval_andor(TSNode andor_node) {
    uint32_t npipes = ts_node_named_child_count(andor_node);
    if (npipes == 0) { last_status = 0; return last_status; }

    /* Evaluate the first pipeline. */
    TSNode left = ts_node_named_child(andor_node, 0);
    int status = eval_node_status(left);

    /* Walk the rest, respecting &&/|| short-circuit. */
    for (uint32_t i = 1; i < npipes; i++) {
        TSNode prev = ts_node_named_child(andor_node, i - 1);
        TSNode cur  = ts_node_named_child(andor_node, i);

        /* Slice text between prev and cur to discover the operator. */
        uint32_t prev_end  = ts_node_end_byte(prev);
        uint32_t cur_start = ts_node_start_byte(cur);
        const char *seg    = input + prev_end;
        size_t seglen      = (cur_start > prev_end) ? (size_t)(cur_start - prev_end) : 0;

        /* Scan for "&&" or "||" ignoring whitespace. */
        const char *p = seg, *pend = seg + seglen;
        int is_and = 0, is_or = 0;
        while (p + 1 < pend) {
            if (p[0] == '&' && p[1] == '&') { is_and = 1; break; }
            if (p[0] == '|' && p[1] == '|') { is_or  = 1; break; }
            ++p;
        }

        int run_right = 1; /* default: sequence if no operator found */
        if (is_and) run_right = (status == 0);
        else if (is_or) run_right = (status != 0);

        if (run_right) {
            status = eval_node_status(cur);  /* update with right status */
        } else {
            /* short-circuited: keep prior status, skip cur */
        }
    }

    last_status = status;
    return last_status;
}


/* Handle: redirected_statement := (body: command|pipeline) (file_redirect ...)+ */
static void handle_redirected_statement(TSNode rs)
{
    TSNode body = ts_node_child_by_field_id(rs, bodyId);
    if (ts_node_is_null(body)) return;

    DBG("[RS] body=%s\n", ts_node_type(body));

    int in_fd  = -1;
    int out_fd = -1;
    bool have_in  = false;
    bool have_out = false;

    uint32_t n = ts_node_named_child_count(rs);
    for (uint32_t i = 0; i < n; i++) {
        TSNode ch = ts_node_named_child(rs, i);
        if (ts_node_symbol(ch) != sym_file_redirect) continue;

        char *redir_txt = ts_extract_node_text(input, ch);
        if (!redir_txt) redir_txt = strdup("");
        const char *p = redir_txt;
        while (*p == ' ' || *p == '\t') p++;
        char op1 = *p;
        char op2 = (p[0] && p[1]) ? p[1] : '\0';
        bool is_input  = (op1 == '<');
        bool is_append = (op1 == '>' && op2 == '>');

        TSNode dest = ts_node_child_by_field_id(ch, destinationId);
        char *path = ts_extract_node_text(input, dest);
        if (!path) path = strdup("");

        DBG("[RS] redirect op='%c%c' path='%s'\n", op1, op2 ? op2 : ' ', path);

        if (is_input) {
            if (have_in && in_fd >= 0) close(in_fd);
            int fd = open(path, O_RDONLY);
            if (fd < 0) {
                utils_error("minibash: cannot open for input: %s", path);
                free(path); free(redir_txt);
                if (have_out && out_fd >= 0) close(out_fd);
                if (have_in  && in_fd  >= 0) close(in_fd);
                last_status = 1;
                return;
            }
            in_fd = fd; have_in = true;
        } else {
            if (have_out && out_fd >= 0) close(out_fd);
            int flags = O_WRONLY | O_CREAT | (is_append ? O_APPEND : O_TRUNC);
            int fd = open(path, flags, 0666);
            if (fd < 0) {
                utils_error("minibash: cannot open for output: %s", path);
                free(path); free(redir_txt);
                if (have_out && out_fd >= 0) close(out_fd);
                if (have_in  && in_fd  >= 0) close(in_fd);
                last_status = 1;
                return;
            }
            out_fd = fd; have_out = true;
            (void)is_append;
        }

        free(path);
        free(redir_txt);
    }

    DBG("[RS] in_fd=%d have_in=%d  out_fd=%d have_out=%d\n",
        in_fd, (int)have_in, out_fd, (int)have_out);

    int rc = 0;
    switch (ts_node_symbol(body)) {
        case sym_command:
            DBG("[RS] run command with in=%d out=%d\n",
                have_in ? in_fd : -1, have_out ? out_fd : -1);
            rc = run_command_with_io(body, have_in ? in_fd : -1, have_out ? out_fd : -1);
            break;
        case sym_pipeline:
            DBG("[RS] run pipeline with in=%d out=%d\n",
                have_in ? in_fd : -1, have_out ? out_fd : -1);
            rc = run_pipeline_with_io(body, have_in ? in_fd : -1, have_out ? out_fd : -1);
            break;
        default:
            ts_print_node_info(body, "redirected_statement: unexpected body");
            rc = 1;
            break;
    }

    if (have_in  && in_fd  >= 0) close(in_fd);
    if (have_out && out_fd >= 0) close(out_fd);

    last_status = rc;
}


static void execute_node(TSNode child) {
    switch (ts_node_symbol(child)) {
        case sym_comment:
            break;

        case sym_variable_assignment:
            handle_variable_assignment(child);
            break;

        /* NEW: handle top-level list nodes */
        case sym_list:
            (void)eval_node_status(child);   /* updates last_status */
            break;

        /* NEW: handle explicit and_or nodes if your grammar exposes it */
#ifdef sym_and_or
        case sym_and_or:
            (void)eval_andor(child);         /* updates last_status */
            break;
#endif

#ifdef sym_binary_expression
        case sym_binary_expression:          /* Some grammars use this for &&/|| */
            (void)eval_andor(child);
            break;
#endif

        case sym_command:
            handle_command(child);
            break;

        case sym_redirected_statement:
            handle_redirected_statement(child);
            break;

        case sym_pipeline:
            handle_pipeline(child);
            break;

        default: {
            /* If there’s an operator field we can still treat it like and/or */
            TSNode opn = ts_node_child_by_field_id(child, operatorId);
            if (!ts_node_is_null(opn)) {
                (void)eval_andor(child);
                break;
            }
            ts_print_node_info(child, "Unimplemented node");
            break;
        }
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