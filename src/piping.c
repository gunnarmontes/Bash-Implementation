// piping.c
// Pipeline wiring/execution (no expansions, no redirections, no globals)

#include "piping.h"

#include <tree_sitter/api.h>
#include "ts_symbols.h"

#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>
#include <errno.h>

/* -------- optional minimal debug ------- */
#ifndef PIPE_DEBUG
#define PIPE_DEBUG 0
#endif
#if PIPE_DEBUG
#  include <stdio.h>
#  define PDBG(...) fprintf(stderr, __VA_ARGS__)
#else
#  define PDBG(...) ((void)0)
#endif
/* --------------------------------------- */

/* Collect the list of sym_command children inside a sym_pipeline.
 * Returns the number of commands on success (>=0).
 * On success, *out_cmds is a malloc'ed array of TSNode of length ncmds (caller frees).
 * Returns -1 on OOM.
 */
static int
collect_pipeline_commands(TSNode pipeline_node, TSNode **out_cmds)
{
    *out_cmds = NULL;

    uint32_t total_named = ts_node_named_child_count(pipeline_node);
    int ncmds = 0;

    for (uint32_t i = 0; i < total_named; i++) {
        TSNode ch = ts_node_named_child(pipeline_node, i);
        if (ts_node_symbol(ch) == sym_command)
            ncmds++;
    }

    if (ncmds == 0) {
        return 0;
    }

    TSNode *cmds = (TSNode *)malloc((size_t)ncmds * sizeof *cmds);
    if (!cmds) {
        return -1;
    }

    int j = 0;
    for (uint32_t i = 0; i < total_named; i++) {
        TSNode ch = ts_node_named_child(pipeline_node, i);
        if (ts_node_symbol(ch) == sym_command)
            cmds[j++] = ch;
    }

    *out_cmds = cmds;
    return ncmds;
}

/* Close N-1 pipes (2 fds each). Safe on NULL. */
static void
close_all_pipes(int (*pipes)[2], int ncmds)
{
    if (!pipes || ncmds <= 1) return;
    for (int i = 0; i < ncmds - 1; i++) {
        if (pipes[i][0] >= 0) close(pipes[i][0]);
        if (pipes[i][1] >= 0) close(pipes[i][1]);
    }
}

/* Normalize a wait status to a shell-like exit status. */
static int
status_to_exitcode(int st)
{
    if (WIFEXITED(st))  return WEXITSTATUS(st);
    if (WIFSIGNALED(st)) return 128 + WTERMSIG(st);
    return 1;
}

int
piping_run_pipeline_with_io(TSNode pipeline_node,
                            int pipe_in_fd,
                            int pipe_out_fd,
                            piping_exec_child_cb exec_cb)
{
    if (!exec_cb) return 1; /* invalid usage */

    /* Collect commands */
    TSNode *cmds = NULL;
    int ncmds = collect_pipeline_commands(pipeline_node, &cmds);
    if (ncmds < 0) return 1;       /* OOM */
    if (ncmds == 0) return 0;      /* empty pipeline => no-op success */

    /* Create N-1 pipes if needed */
    int (*pipes)[2] = NULL;
    if (ncmds > 1) {
        pipes = (int (*)[2])calloc((size_t)(ncmds - 1), sizeof *pipes);
        if (!pipes) { free(cmds); return 1; }
        for (int i = 0; i < ncmds - 1; i++) {
            if (pipe(pipes[i]) != 0) {
                for (int k = 0; k < i; k++) {
                    close(pipes[k][0]);
                    close(pipes[k][1]);
                }
                free(pipes);
                free(cmds);
                return 1;
            }
        }
    }

    pid_t *pids = (pid_t *)calloc((size_t)ncmds, sizeof *pids);
    if (!pids) {
        close_all_pipes(pipes, ncmds);
        free(pipes);
        free(cmds);
        return 1;
    }

    /* Fork/wire children */
    for (int i = 0; i < ncmds; i++) {
        pid_t pid = fork();
        if (pid == 0) {
            /* ----- child ----- */

            /* stdin wiring */
            if (i == 0) {
                if (pipe_in_fd != -1) {
                    (void)dup2(pipe_in_fd, STDIN_FILENO);
                }
            } else {
                (void)dup2(pipes[i - 1][0], STDIN_FILENO);
            }

            /* stdout wiring */
            if (i == ncmds - 1) {
                if (pipe_out_fd != -1) {
                    (void)dup2(pipe_out_fd, STDOUT_FILENO);
                }
            } else {
                (void)dup2(pipes[i][1], STDOUT_FILENO);
            }

            /* close all pipe FDs in the child (now that dup2 is done) */
            close_all_pipes(pipes, ncmds);

            /* close the optional in/out FDs in the child if they were used */
            if (i == 0 && pipe_in_fd  != -1) close(pipe_in_fd);
            if (i == ncmds - 1 && pipe_out_fd != -1) close(pipe_out_fd);

            /* hand off to caller-provided exec callback (should not return) */
            exec_cb(cmds[i]);

            /* if it returns, fail hard */
            _exit(127);
        }

        if (pid < 0) {
            /* fork failed: clean up parent, reap any started children */
            int saved_errno = errno;

            close_all_pipes(pipes, ncmds);
            free(pipes);

            for (int k = 0; k < i; k++) {
                int st; (void)waitpid(pids[k], &st, 0);
            }

            free(pids);
            free(cmds);
            (void)saved_errno;
            return 1;
        }

        /* parent */
        pids[i] = pid;
    }

    /* parent closes its pipe FDs; caller retains ownership of pipe_in_fd/out_fd */
    close_all_pipes(pipes, ncmds);
    free(pipes);
    free(cmds);

    /* wait for all children; return the last one's exit status */
    int last_status = 0;
    for (int i = 0; i < ncmds; i++) {
        int st = 0;
        (void)waitpid(pids[i], &st, 0);
        if (i == ncmds - 1) last_status = st;
    }
    free(pids);

    return status_to_exitcode(last_status);
}

int
piping_handle_pipeline(TSNode pipeline_node, piping_exec_child_cb exec_cb)
{
    return piping_run_pipeline_with_io(pipeline_node, -1, -1, exec_cb);
}
