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

/**
 * Internal change-directory command.
 */
static bool shell_cd(word_t *dir)
{
	/* Execute cd. */
	char *path = get_word(dir);
	int ret = false;
	if (dir == NULL) {
		char *home = getenv("HOME");
		if (home != NULL) {
			ret = chdir(home);
			free(home);
		}
	} else if (strcmp(path, "-") == 0) {
		char *oldpwd = getenv("OLDPWD");
		if (oldpwd != NULL) {
			ret = chdir(oldpwd);
			free(oldpwd);
		}
	} else {
		ret = chdir(path);
	}
	free(path);
	return ret < 0 ? false : true;
}

/**
 * Internal exit/quit command.
 */
static int shell_exit(void)
{
	/* Execute exit/quit. */
	exit(0);
	return SHELL_EXIT;
}

static void redirect(simple_command_t *s)
{
	if (s->in != NULL) {
		char* in = get_word(s->in);
		int fd = open(in, O_RDONLY);
		free(in);
		dup2(fd, STDIN_FILENO);
		close(fd);
	}
	if (s->out != NULL) {
		char* out = get_word(s->out);
		int fd = open(out, O_WRONLY | O_CREAT | (s->io_flags == IO_REGULAR ? O_TRUNC : O_APPEND), 0644);
		free(out);
		dup2(fd, STDOUT_FILENO);
		if (s->err != NULL) {
			dup2(fd, STDERR_FILENO);
		}
		close(fd);
	} else if (s->err != NULL) {
		char* err = get_word(s->err);
		int fd = open(err, O_WRONLY | O_CREAT | (s->io_flags == IO_REGULAR ? O_TRUNC : O_APPEND), 0644);
		free(err);
		dup2(fd, STDERR_FILENO);
		close(fd);
	}
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
	char* command = get_word(s->verb);
	/* If builtin command, execute the command. */
	if (strcmp(command, "cd") == 0) {
		int saved_stdin = dup(0);
		int saved_stdout = dup(1);
		int saved_stderr = dup(2);
		redirect(s);
		// print
		bool ret = shell_cd(s->params);
		// restore
		dup2(saved_stdin, STDIN_FILENO);
		dup2(saved_stdout, STDOUT_FILENO);
		dup2(saved_stderr, STDERR_FILENO);
		close(saved_stdin);
		close(saved_stdout);
		close(saved_stderr);
		free(command);
		return ret == true ? 0 : 1;
	} else if (strcmp(command, "exit") == 0 || strcmp(command, "quit") == 0) {
		free(command);
		return shell_exit();
	}
	/* If variable assignment, execute the assignment and return
	 * the exit status.
	 */
	if (s->verb->next_part != NULL) {
		if (strcmp(s->verb->next_part->string, "=") == 0) {
			free(command);
			return putenv(get_word(s->verb));
		}
	}
	/* If external command:
	 *   1. Fork new process
	 *     2c. Perform redirections in child
	 *     3c. Load executable in child
	 *   2. Wait for child
	 *   3. Return exit status
	 */
	int size;
	char** params = get_argv(s, &size);

	int pid = fork();
	if (pid < 0) {
		return SHELL_EXIT;
	} else if (pid == 0) {
		// child
		// redirect
		redirect(s);
		// execute
		if (strcmp(command, "pwd") == 0) {
			char cwd[1024];
			if (getcwd(cwd, sizeof(cwd)) != NULL) {
				// print
				printf("%s\n", cwd);
			} else {
				exit(0);
			}
		} else {
			int ret = execvp(command, params);
			if (ret < 0) {
				printf("Execution failed for '%s'\n", command);
				exit(ret);
			}
		}
		exit(0);
	} else {
		// parent
		int status;
		waitpid(pid, &status, 0);
		free(command);
		for (int i = 0; i < size; i++) {
			free(params[i]);
		}
		free(params);
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
	int ret;
	if (pid == 0) {
		// child
		ret = parse_command(cmd1, level + 1, father);
		exit(ret);
	} else if (pid > 0) {
		// parent
		int pid2 = fork(); 
		if (pid2 == 0) {
			// child
			ret = parse_command(cmd2, level + 1, father);
			exit(ret);
		} else if (pid2 > 0) {
			// parent
			int status;
			int status2;
			waitpid(pid, &status, 0);
			waitpid(pid2, &status2, 0);
			if (WEXITSTATUS(status) != 0 || WEXITSTATUS(status2) != 0) {
				return false;
			}
		} else {
			return false;
		}
	} else {
		return false;
	}
	return true;
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
		exit(ret);
	} else if (pid > 0) {
		// parent
		int pid2 = fork();
		if (pid2 == 0) {
			// child
			close(fd[WRITE]);
			dup2(fd[READ], STDIN_FILENO);
			close(fd[READ]);
			ret = parse_command(cmd2, level + 1, father);
			exit(ret);
		} else if (pid2 > 0) {
			// parent
			close(fd[READ]);
			close(fd[WRITE]);
			int status;
			waitpid(pid, &status, 0);
			waitpid(pid2, &status, 0);
			if (WEXITSTATUS(status)) {
				return false;
			}
		} else {
			return false;
		}
	} else {
		return false;
	}

	return true;
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
		ret = run_in_parallel(c->cmd1, c->cmd2, level + 1, c) == true ? 0 : 1;
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
		ret = run_on_pipe(c->cmd1, c->cmd2, level + 1, c) == true ? 0 : 1;
		break;

	default:
		return SHELL_EXIT;
	}

	return ret;
}
