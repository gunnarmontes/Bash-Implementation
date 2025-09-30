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
static void handle_command(TSNode command_node) {

    TSNode name_node = ts_node_child_by_field_id(command_node, nameId);
    char *cmd = ts_extract_node_text(input, name_node);

    if (cmd == NULL) {
        printf("COuldnt extract command name\n");
        return;
    }

    /* ---- builtin: echo — handles plain/""/'' args and "$?" ---- */
    if (strcmp(cmd, "echo") == 0) {
        uint32_t n = ts_node_named_child_count(command_node);
        bool first = true;

        for (uint32_t i = 0; i < n; i++) {
            TSNode ch = ts_node_named_child(command_node, i);
            if (ts_node_eq(ch, name_node))
                continue;

            char *text = ts_extract_node_text(input, ch);
            if (!text)
                continue;

            if (!first) putchar(' ');

            /* Expand "$?" exactly */
            if (ts_node_symbol(ch) == sym_simple_expansion && strcmp(text, "$?") == 0) {
                char buf[32];
                snprintf(buf, sizeof buf, "%d", last_status);
                fputs(buf, stdout);
                free(text);
                first = false;
                continue;
            }

            /* Strip exactly one matching pair of outer quotes, if present. */
            size_t len = strlen(text);
            if (len >= 2) {
                char q = text[0];
                if ((q == '"' || q == '\'') && text[len - 1] == q) {
                    fwrite(text + 1, 1, len - 2, stdout);
                    free(text);
                    first = false;
                    continue;
                }
            }

            /* No outer quotes — print as-is. */
            fputs(text, stdout);
            free(text);
            first = false;
        }

        putchar('\n');
        free(cmd);
        last_status = 0;
        return;
    }
    /* ---- end builtin: echo ---- */

    /* -------- externals: build argv and choose execvp/execv -------- */

    /* 1) Count plain word arguments (022 only needs simple words like "-segfault"). */      // [022]
    uint32_t named = ts_node_named_child_count(command_node);                                 // [022]
    int word_argc = 0;                                                                        // [022]
    for (uint32_t i = 0; i < named; i++) {                                                    // [022]
        TSNode ch = ts_node_named_child(command_node, i);                                     // [022]
        if (ts_node_eq(ch, name_node))                                                        // [022]
            continue;                                                                         // [022]
        if (ts_node_symbol(ch) == sym_word)                                                   // [022]
            word_argc++;                                                                      // [022]
        /* TODO: handle quoted words / expansions / redirects as args when tests require. */  // [022]
    }                                                                                         // [022]

    /* 2) Allocate argv = { cmd, <word args...>, NULL } */                                     // [022]
    char **argv = calloc((size_t)word_argc + 2, sizeof(char *));                              // [022]
    if (!argv) {                                                                              // [022]
        /* Minimal failure handling: try to exec with just cmd and NULL */                    // [022]
        argv = (char **)malloc(2 * sizeof(char *));                                           // [022]
        if (!argv) { free(cmd); return; }                                                     // [022]
        argv[0] = cmd;                                                                        // [022]
        argv[1] = NULL;                                                                       // [022]
        word_argc = 0;                                                                        // [022]
    } else {                                                                                  // [022]
        argv[0] = cmd;                                                                        // [022]
        int idx = 1;                                                                          // [022]
        for (uint32_t i = 0; i < named; i++) {                                                // [022]
            TSNode ch = ts_node_named_child(command_node, i);                                 // [022]
            if (ts_node_eq(ch, name_node))                                                    // [022]
                continue;                                                                     // [022]
            if (ts_node_symbol(ch) != sym_word)                                               // [022]
                continue;                                                                     // [022]
            char *arg = ts_extract_node_text(input, ch);                                      // [022]
            if (arg) argv[idx++] = arg;                                                       // [022]
        }                                                                                     // [022]
        argv[word_argc + 1] = NULL;                                                           // [022]
    }                                                                                         // [022]

    /* 3) Choose exec: bareword → execvp (PATH), absolute path → execv */                     // [022]
    int use_execvp = (strchr(cmd, '/') == NULL);                                              // [022]

    pid_t pid = fork();

    if (pid == 0) {
        if (use_execvp) {                                                                     // [022]
            execvp(cmd, argv);                                                                // [022]
        } else {
            execv(cmd, argv);
        }
        _exit(127);
    } else {
        int status;
        (void)waitpid(pid, &status, 0);
        if (WIFEXITED(status))
            last_status = WEXITSTATUS(status);
        else if (WIFSIGNALED(status))
            last_status = 128 + WTERMSIG(status);
        else
            last_status = 1; /* conservative default */
    }

    /* 4) Free argv entries (skip argv[0] — that's `cmd`, freed below) */                      // [022]
    for (int i = 1; i <= word_argc; i++) {                                                    // [022]
        free(argv[i]);                                                                         // [022]
    }                                                                                          // [022]
    free(argv);                                                                                // [022]

    free(cmd);
}




static void execute_node(TSNode child) {

    switch (ts_node_symbol(child)) {

        case sym_comment:
            break;
        case sym_command:
            handle_command(child);
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