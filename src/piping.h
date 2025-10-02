#pragma once
#include <tree_sitter/api.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*piping_exec_child_cb)(TSNode command_node);

int piping_run_pipeline_with_io(TSNode pipeline_node,
                                int pipe_in_fd,
                                int pipe_out_fd,
                                piping_exec_child_cb exec_cb);

int piping_handle_pipeline(TSNode pipeline_node,
                           piping_exec_child_cb exec_cb);

#ifdef __cplusplus
}
#endif
