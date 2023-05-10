// SPDX-License-Identifier: BSD-3-Clause

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>

#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "cmd.h"
#include "utils.h"

#define READ		0
#define WRITE		1

word_t* currentDir;

/**
 * Internal change-directory command.
 */
static bool shell_cd(word_t *dir)
{
	/* TODO: Execute cd. */
	if (dir == NULL) {
		// TODO: change to home
		return false;
	}

	return 0;
}

/**
 * Internal exit/quit command.
 */
static int shell_exit(void)
{
	/* Execute exit/quit. */
	exit(1);
}

/**
 * Parse a simple command (internal, environment variable assignment,
 * external command).
 */
static int parse_simple(simple_command_t *s, int level, command_t *father)
{
	/* Sanity checks. */
	if (s == NULL)
		return SHELL_EXIT;
	/* If builtin command, execute the command. */
	if (strcmp(s->verb->string, "cd") == 0) {
		return shell_cd(s->params);
	}
	else if (strcmp(s->verb->string, "pwd") == 0) {
		if (currentDir == NULL) {
			currentDir = (word_t *)malloc(sizeof(word_t));
			currentDir->string = "/";
		}
		printf("%s\n", currentDir->string);
		return 0;
	}
	else if (strcmp(s->verb->string, "exit") == 0 || strcmp(s->verb->string, "quit") == 0) {
		return shell_exit();
	}
	/* TODO: If variable assignment, execute the assignment and return
	 * the exit status.
	 */

	/* If external command:
	 *   1. Fork new process
	 *     2c. Perform redirections in child
	 *     3c. Load executable in child
	 *   2. Wait for child
	 *   3. Return exit status
	 */
	int pid = fork();
	if (pid == 0) {
		// child
		// redirect
		if (s->in != NULL) {
			int fd = open(s->in->string, O_RDONLY);
			dup2(fd, STDIN_FILENO);
			close(fd);
		}
		if (s->out != NULL) {
			int fd = open(s->out->string, O_WRONLY | O_CREAT | O_TRUNC, 0644);
			dup2(fd, STDOUT_FILENO);
			close(fd);
		}
		// execute
		execvp(s->verb->string, (char *const *)s->params);
		exit(0);
	} else {
		// parent
		int status;
		waitpid(pid, &status, 0);
		return WEXITSTATUS(status);
	}
}

/**
 * Process two commands in parallel, by creating two children.
 */
static bool run_in_parallel(command_t *cmd1, command_t *cmd2, int level,
		command_t *father)
{
	/* Execute cmd1 and cmd2 simultaneously. */
	int pid = fork();
	int ret = 0;
	if (pid == 0) {
		// child
		ret = parse_command(cmd1, level + 1, father);
		exit(1);
	} else if (pid > 0) {
		// parent
		ret = parse_command(cmd2, level + 1, father);
	} else {
		return false;
	}
	return ret;
}

/**
 * Run commands by creating an anonymous pipe (cmd1 | cmd2).
 */
static bool run_on_pipe(command_t *cmd1, command_t *cmd2, int level,
		command_t *father)
{
	/* Redirect the output of cmd1 to the input of cmd2. */
	int fd[2];
	int ret = pipe(fd);
	if (ret < 0) {
		return false;
	}
	int pid = fork();
	if (pid == 0) {
		// child
		close(fd[READ]);
		dup2(fd[WRITE], STDOUT_FILENO);
		close(fd[WRITE]);
		ret = parse_command(cmd1, level + 1, father);
		exit(0);
	} else if (pid > 0) {
		// parent
		close(fd[WRITE]);
		dup2(fd[READ], STDIN_FILENO);
		close(fd[READ]);
		ret = parse_command(cmd2, level + 1, father);
	} else {
		return false;
	}

	return ret;
}

/**
 * Parse and execute a command.
 */
int parse_command(command_t *c, int level, command_t *father)
{
	/* sanity checks */
	if (c == NULL)
		return SHELL_EXIT;

	if (c->op == OP_NONE) {
		/* Execute a simple command. */
		return parse_simple(c->scmd, level, father);
	}

	int ret = 0;
	switch (c->op) {
	case OP_SEQUENTIAL:
		/* Execute the commands one after the other. */
		ret = parse_command(c->cmd1, level + 1, c);
		ret = parse_command(c->cmd2, level + 1, c);
		break;

	case OP_PARALLEL:
		/* Execute the commands simultaneously. */
		ret = run_in_parallel(c->cmd1, c->cmd2, level + 1, c);
		break;

	case OP_CONDITIONAL_NZERO:
		/* Execute the second command only if the first one
		 * returns non zero.
		 */
		ret = parse_command(c->cmd1, level + 1, c);
		if (ret != 0) {
			ret = parse_command(c->cmd2, level + 1, c);
		}
		break;

	case OP_CONDITIONAL_ZERO:
		/* Execute the second command only if the first one
		 * returns zero.
		 */
		ret = parse_command(c->cmd1, level + 1, c);
		if (ret == 0) {
			ret = parse_command(c->cmd2, level + 1, c);
		}
		break;

	case OP_PIPE:
		/* Redirect the output of the first command to the
		 * input of the second.
		 */
		ret = run_on_pipe(c->cmd1, c->cmd2, level + 1, c);
		break;

	default:
		return SHELL_EXIT;
	}

	return ret;
}
