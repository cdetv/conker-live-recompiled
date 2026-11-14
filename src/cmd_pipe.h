/**
 * The command pipe (to-do 174, --debug): console commands from any program
 * on this PC through a local named pipe, replies back the same way. See
 * cmd_pipe.c for the protocol.
 */
#ifndef CONKER_CMD_PIPE_H
#define CONKER_CMD_PIPE_H

#ifdef __cplusplus
extern "C" {
#endif

/* Starts the pipe's thread (main thread, with --debug). */
void cmd_pipe_start(void);

/* Executor (console_frame): the next line a client sent, copied into line;
 * 0 when none waits. After a 1 the pipe waits for cmd_pipe_done. */
int cmd_pipe_take(char *line, int size);

/* Executor: the reply to the line taken (everything the console printed for
 * it, lines ended by '\n') and whether any of it was an error. */
void cmd_pipe_done(const char *reply, int errors);

#ifdef __cplusplus
}
#endif

#endif
