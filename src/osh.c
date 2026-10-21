#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>

#define MAX_ARGS 64
#define MAX_BG_EVENTS 64

static pid_t bg_pids[MAX_BG_EVENTS];
static int bg_statuses[MAX_BG_EVENTS];
static volatile sig_atomic_t bg_count = 0;

static void on_chld(int s);
int parse_redirections(char *args[], char **input_file, char **output_file, int *append);
void setup_redirections(char *input_file, char *output_file, int append);
int execute_command(char *args[], char *input_file, char *output_file, int append, int background);
int count_pipes(char *args[], int argc);
int execute_pipeline(char *args[], int argc, int num_pipes,
		int num_commands, char *input_file,
		char *output_file, int append);

int main(void) {

	// Install SIGCHLD handler.
	struct sigaction sa = {0};
	sa.sa_handler = on_chld;
	sa.sa_flags = SA_RESTART;
	sigemptyset(&sa.sa_mask);
	sigaction(SIGCHLD, &sa, NULL);

	// Install SIGINT handler.
	struct sigaction sa_ign = {0};
	sa_ign.sa_handler = SIG_IGN;
	sigemptyset(&sa_ign.sa_mask); // Block no other signal during handler execution.
	sa_ign.sa_flags = 0; // No special flags.
	sigaction(SIGINT, &sa_ign, NULL);

	char *line = NULL;
	size_t capacity = 0;

	int last_status = 0;

	// Keep the shell running until an exit command or EOF is received.
	while(1) {

		char *args[MAX_ARGS];
		int argc = 0;

		// Check whether the command is background or foreground.
		int background = 0;

		// Create a signal set containing SIGCHLD.
		sigset_t block, old_mask;
		sigemptyset(&block);
		sigaddset(&block, SIGCHLD);

		// Block SIGCHLD and save the previous signal mask.
		sigprocmask(SIG_BLOCK, &block, &old_mask);

		// Process all completed background events while SIGCHLD is blocked.
		for (int i = 0; i < bg_count; i++) {

			// The background process terminated normally.
			if (WIFEXITED(bg_statuses[i])) {
				printf("[bg] %d done\n", bg_pids[i]);
			}

			// The background process was terminated by a signal.
			else if (WIFSIGNALED(bg_statuses[i])) {
				printf("[bg] %d signal %d\n", bg_pids[i], WTERMSIG(bg_statuses[i]));
			}
		}

		// All stored background events have now been processed.
		bg_count = 0;

		// Restore the previous signal mask, allowing SIGCHLD again if it was not blocked before.
		sigprocmask(SIG_SETMASK, &old_mask, NULL);

		// Display the shell prompt.
		printf("osh> ");
		fflush(stdout);

		// Read a complete line from the standard input.
		ssize_t n = getline(&line, &capacity, stdin);

		// Exit the loop when EOF is reached or reading fails.
		if (n == -1) {
			break;
		}

		// Split the input into arguments using spaces, tabs and newlines.
		char *token = strtok(line, " \t\n");

		while (token != NULL && argc < MAX_ARGS - 1) {
			args[argc] = token;
			argc++;

			token = strtok(NULL, " \t\n");
		}

		// Reject commands that exceed the maximum number of arguments.
		if (token != NULL) {
			fprintf(stderr, "Too many arguments\n");
			continue;
		}

		// Terminate the argument array with NULL.
		args[argc] = NULL;

		// Ignore empty commands.
		if (argc == 0) {
			continue;
		}

		// Check whether the command should run in the background and remove '&' from the args.
		if (strcmp(args[argc - 1], "&") == 0) {
			background = 1;
			argc -= 1;
			args[argc] = NULL;

			if (argc == 0) {
				fprintf(stderr, "osh: syntax error: missing command before '&'\n");
				last_status = 1;
				continue;
			}
		}

		// Handle the built-in exit command.
		if (strcmp(args[0], "exit") == 0) {

			// Exit with the specified status code, if provided.
			if (args[1]) {
				int code = atoi(args[1]);
				free(line);
				exit(code);
			}

			// Exit successfully when no status code is provided.
			free(line);
			exit(0);

		}

		// Handle the built-in cd command.
		else if (strcmp(args[0], "cd") == 0) {

			char *path;

			// Use the provided directory or HOME if no argument is given.
			if (args[1]) {
				path = args[1];
			}
			else {
				path = getenv("HOME");
			}

			// Check whether the destination directory is available.
			if (path == NULL) {
				fprintf(stderr, "cd: HOME not set\n");
				last_status = 1;
			}
			else if (chdir(path) == -1) {
				// Report an error if changing directories fails.
				perror("cd");
				last_status = 1;
			}
			else {
				last_status = 0;
			}

		}

		// Handle the built-in status command.
		else if (strcmp(args[0], "status") == 0) {
			printf("%d\n", last_status);
		}

		// Handle the built-in pinfo command.
		else if (strcmp(args[0], "pinfo") == 0) {

			if (argc != 2) {
				fprintf(stderr, "Usage: pinfo PID\n");
				last_status = 1;
			}
			else {
				// Build the path to the process status file.
				char path[50];
				snprintf(path, sizeof(path), "/proc/%s/status", args[1]);

				// Open the process status file for reading.
				FILE *file = fopen(path, "r");

				// Handle a PID that does not correspond to an existing process.
				if (file == NULL) {
					fprintf(stderr, "pinfo: %s: No such process\n", args[1]);
					last_status = 1;
					continue;
				}

				char buffer[256];

				// Fields to display from /proc/PID/status.
				const char *fields[] = {
					"Name:",
					"State:",
					"PPid:",
					"Threads:",
					"VmRSS:",
					"voluntary_ctxt_switches:",
					"nonvoluntary_ctxt_switches:"
				};
				int size = 7;

				// Read the status file line by line.
				while (fgets(buffer, sizeof(buffer), file) != NULL) {

					// Print the line if it starts with one of the required fields.
					for (int i = 0; i < size; i++) {
						if (strncmp(buffer, fields[i], strlen(fields[i])) == 0) {
							fputs(buffer, stdout);
							break;
						}
					}
				}

				// Close the status file after reading it.
				fclose(file);
				last_status = 0;
			}
		}

		// Handle external commands.
		else {

			char *input_file = NULL;
			char *output_file = NULL;
			int append = 0;

			int num_pipes = count_pipes(args, argc);

			if (num_pipes > 0) {

				int num_commands = num_pipes + 1;

				last_status = execute_pipeline(args, argc, num_pipes, num_commands, input_file,
								output_file, append);
			}
			else {

				if (parse_redirections(args, &input_file, &output_file, &append) == 0) {
					last_status = execute_command(args, input_file, output_file, append, background);
				}
				else {
					last_status = 1;
					continue;
				}
			}
		}

	}

	// Release the memory allocated by getline().
	free(line);

	return 0;
}

// -- Functions for the program --

static void on_chld(int s) {

	pid_t completed_pid;
	int status;

	// The signal number is not used by this handler.
	(void)s;

	// Preserve errno because waitpid() may modify it.
	int saved = errno;

	// Read all child processes that have already terminated.
	while ((completed_pid = waitpid(-1, &status, WNOHANG)) > 0) {

		// Store the PID and status if there is space for another event.
		if (bg_count < MAX_BG_EVENTS) {
			bg_pids[bg_count] = completed_pid;
			bg_statuses[bg_count] = status;
			bg_count++;
		}
	}

	// Restore the value of errno that existed before the handler ran.
	errno = saved;
}

int is_operator(const char *token) {
	return strcmp(token, "|") == 0 ||
		strcmp(token, "<") == 0 ||
		strcmp(token, ">") == 0 ||
		strcmp(token, ">>") == 0;
}

int parse_redirections(char *args[],
			char **input_file, char **output_file,
			int *append) {

	int j = 0;

	// Find redirection operators and remove them and their filenames
	// from the argument list.
	for (int i = 0; args[i] != NULL; ) {

		if (strcmp(args[i], "<") == 0) {
			if (args[i + 1] == NULL || is_operator(args[i + 1])) {
				fprintf(stderr, "osh: syntax error: missing filename after '%s'\n", args[i]);
				return 1;
			}

			// Store the input filename.
			*input_file = args[i + 1];
			i += 2;
		}

		else if (strcmp(args[i], ">") == 0) {
			if (args[i + 1] == NULL || is_operator(args[i + 1])) {
				fprintf(stderr, "osh: syntax error: missing filename after '%s'\n", args[i]);
				return 1;
			}

			// Store the output filename for overwrite mode.
			*output_file = args[i + 1];
			i += 2;
		}
		else if (strcmp(args[i], ">>") == 0) {
			if (args[i + 1] == NULL || is_operator(args[i + 1])) {
				fprintf(stderr, "osh: syntax error: missing filename after '%s'\n", args[i]);
				return 1;
			}

			// Store the output filename and enable append mode.
			*output_file = args[i + 1];
			*append = 1;
			i += 2;
		}
		else {
			// Keep normal command arguments.
			args[j] = args[i];
			i++;
			j++;
		}
	}

	// Terminate the modified argument list for execvp();
	args[j] = NULL;

	return 0;
}

void setup_redirections(char *input_file, char *output_file, int append) {

	int fd;

	// Redirect standard input from the specified file.
	if (input_file != NULL) {

		fd = open(input_file, O_RDONLY);

		if (fd == -1) {
			perror("open");
			_exit(1);
		}

		if (dup2(fd, STDIN_FILENO) == -1) {
			perror("dup2");
			_exit(1);
		}

		close(fd);
	}

	// Redirect standard output to the specified file.
	if (output_file != NULL) {

		int flags;

		// Choose between append and truncate mode.
		if (append) {
			flags = O_WRONLY | O_CREAT | O_APPEND;
		}
		else {
			flags = O_WRONLY | O_CREAT | O_TRUNC;
		}

		// Create new output files with permission 0644.
		fd = open(output_file, flags, 0644);

		if (fd == -1) {
			perror("open");
			_exit(1);
		}

		if (dup2(fd, STDOUT_FILENO) == -1) {
			perror("dup2");
			_exit(1);
		}

		close(fd);
	}

}

int execute_command(char *args[], char *input_file, char *output_file, int append, int background) {

	sigset_t block, old_mask;

	sigemptyset(&block);
	sigaddset(&block, SIGCHLD);

	// Block SIGCHLD before forking a foreground command.
	if (background == 0) {
		sigprocmask(SIG_BLOCK, &block, &old_mask);
	}

	pid_t pid = fork();

	if (pid < 0) {
		perror("fork");

		// Restore the previous signal mask if SIGCHLD was blocked.
		if (background == 0) {
			sigprocmask(SIG_SETMASK, &old_mask, NULL);
		}

		return 1;
	}

	if (pid == 0) {

		if (background == 0) {
			// Child stops ignoring the SIGINT.
			struct sigaction sa_dfl = {0};
			sa_dfl.sa_handler = SIG_DFL;
			sigemptyset(&sa_dfl.sa_mask);
			sa_dfl.sa_flags = 0;

			sigaction(SIGINT, &sa_dfl, NULL);

			// Restore SIGCHLD.
			sigprocmask(SIG_SETMASK, &old_mask, NULL);
		}

		setup_redirections(input_file, output_file, append);

		// Execute the command in the child process.
		execvp(args[0], args);

		// execvp() returns only if an error occurs.
		if (errno == ENOENT) {
			fprintf(stderr, "osh: %s: command not found\n", args[0]);
			_exit(127);
		}

		// Handle other execution errors.
		perror(args[0]);
		_exit(126);
	}

	// For a background command, report its PID and return without waiting for it.
	if (background == 1) {

		printf("[bg] %d\n", pid);
		return 0;
	}

	// Wait for the child process to terminate.
	int status;

	pid_t result;

	do {
		result = waitpid(pid, &status, 0);
	} while (result == -1 && errno == EINTR);

	// Restore the previous signal mask regardless of whether waitpid() succeeded.
	sigprocmask(SIG_SETMASK, &old_mask, NULL);

	if (result == -1) {
		perror("waitpid");
		return 1;
	}


	// Check whether the child terminated normally.
	if (WIFEXITED(status)) {

		// Get the exit code of the child process.
		int code = WEXITSTATUS(status);

		// Report non-zero exit codes to standard error.
		if (code != 0) {
			fprintf(stderr, "[exit %d]\n", code);
		}

		// Return the exit code to update the shell's last status.
		return code;
	}

	// Check whether the child was terminated by a signal.
	else if (WIFSIGNALED(status)) {

		// Get the number of the signal that terminated the child.
		int sig = WTERMSIG(status);

		// Display the signal number and its description.
		fprintf(stderr, "[signal %d: %s]\n", sig, strsignal(sig));

		// Return the shell-style exit status for signal termination.
		return 128 + sig;
	}

	// Handle any unexpected termination status.
	return 1;
}

int count_pipes(char *args[], int argc) {

	int pipelines = 0;

	// Count the pipe operators in the argument list.
	for (int i = 0; i < argc; i++) {
		if (strcmp(args[i], "|") == 0) {
			pipelines++;
		}
	}

	return pipelines;
}

int execute_pipeline(char *args[],int argc, int num_pipes,
			int num_commands, char *input_file,
			char *output_file, int append) {

	sigset_t block, old_mask;

	sigemptyset(&block);
	sigaddset(&block, SIGCHLD);

	// Prevent the SIGCHLD handler from reaping foreground pipeline children.
	if (sigprocmask(SIG_BLOCK, &block, &old_mask) == -1) {
		perror("sigprocmask");
		return 1;
	}

	char *commands[num_commands][64];
	pid_t pids[num_commands];

	int cmd = 0;
	int arg = 0;

	// Split the argument list into separate commands at each pipe.
	for (int i = 0; i < argc; i++) {

		if (strcmp(args[i], "|") != 0) {
			commands[cmd][arg] = args[i];
			arg++;
		}
		else {
			if (arg == 0) {
				fprintf(stderr, "osh: syntax error: empty command in pipeline\n");
				sigprocmask(SIG_SETMASK, &old_mask, NULL);
				return 1;
			}

			// Terminate the current command's argument list for execvp().
			commands[cmd][arg] = NULL;
			cmd++;
			arg = 0;
		}
	}

	if (arg == 0) {
		fprintf(stderr, "osh: syntax error: empty command in pipeline\n");
		sigprocmask(SIG_SETMASK, &old_mask, NULL);
		return 1;
	}

	// Terminate the argument list of the last command.
	commands[cmd][arg] = NULL;

	if (parse_redirections(commands[0], &input_file, &output_file, &append) == 1) {
		sigprocmask(SIG_SETMASK, &old_mask, NULL);
		return 1;
	}
	if (parse_redirections(commands[num_commands - 1], &input_file, &output_file, &append) == 1) {
		sigprocmask(SIG_SETMASK, &old_mask, NULL);
		return 1;
	}

	int pipes[num_pipes][2];

	// Create one pipe between each pair of consecutive commands.
	for (int i = 0; i < num_pipes; i++) {
		if (pipe(pipes[i]) == -1) {
			perror("pipe");

			// Close all previously created pipes.
			for (int j = 0; j < i; j++) {
				close(pipes[j][0]);
				close(pipes[j][1]);
			}

			sigprocmask(SIG_SETMASK, &old_mask, NULL);
			return 1;
		}
	}

	// Create one child process for each command in the pipeline.
	for (int i = 0; i < num_commands; i++) {

		pid_t pid = fork();

		if (pid < 0) {
			perror("fork");

			// Close all pipe file descriptors.
			for (int j = 0; j < num_pipes; j++) {
				close(pipes[j][0]);
				close(pipes[j][1]);
			}

			// Terminate children that were already created.
			for (int j = 0; j < i; j++) {
				kill(pids[j], SIGTERM);
			}

			// Reap the terminated children.
			for (int j = 0; j < i; j++) {
				while (waitpid(pids[j], NULL, 0) == -1 && errno == EINTR)  {
					// Retry if interrupted by a signal.
				}
			}

			// Restore the previous signal mask.
			sigprocmask(SIG_SETMASK, &old_mask, NULL);

			return 1;
		}

		if (pid == 0) {

			// Foreground pipeline children must respond normally to Ctrl-C.
			struct sigaction sa_dfl = {0};
			sa_dfl.sa_handler = SIG_DFL;
			sigemptyset(&sa_dfl.sa_mask);
			sa_dfl.sa_flags = 0;
			sigaction(SIGINT, &sa_dfl, NULL);

			// Restore the signal mask inherited from the shell.
			if (sigprocmask(SIG_SETMASK, &old_mask, NULL) == -1) {
				perror("sigprocmask");
				_exit(1);
			}

			// All commands, except the first, read from the previous pipe.
			if (i != 0) {
				if (dup2(pipes[i - 1][0], STDIN_FILENO) == -1) {
					perror("dup2");
					_exit(1);
				}
			}

			// All commands, except the last, write to the next pipe.
			if (i != num_commands - 1) {
				if (dup2(pipes[i][1], STDOUT_FILENO) == -1) {
					perror("dup2");
					_exit(1);
				}
			}

			// Apply input redirection only to the first command.
			if (i == 0) {
				setup_redirections(input_file, NULL, 0);
			}

			// Apply output redirection only to the last command.
			if (i == num_commands - 1) {
				setup_redirections(NULL, output_file, append);
			}

			// Close all inherited pipe descriptors after the required dup2().
			for (int j = 0; j < num_pipes; j++) {
				close(pipes[j][0]);
				close(pipes[j][1]);
			}

			// Replace the child process with the corresponding command.
			execvp(commands[i][0], commands[i]);

			// execvp() returns only if execution failed.
			if (errno == ENOENT) {
				fprintf(stderr, "osh: %s: command not found\n", commands[i][0]);
				_exit(127);
			}

			perror(commands[i][0]);
			_exit(126);

		}
		else {
			// Store the PID for each child process.
			pids[i] = pid;
		}

	}

	// Parent closes all pipe descriptors after creating all children.
	for (int j = 0; j < num_pipes; j++) {
		close(pipes[j][0]);
		close(pipes[j][1]);
	}

	int last_status = 1;

	// Wait for every child process in the pipeline.
	for (int i = 0; i < num_commands; i++) {

		int status;
		pid_t result;

		do {
			result = waitpid(pids[i], &status, 0);
		} while (result == -1 && errno == EINTR);

		if (result == -1) {
			perror("waitpid");
			continue;
		}

		// The pipeline status is the status of the last command.
		if (pids[i] == pids[num_commands - 1]) {

			if (WIFEXITED(status)) {
				last_status = WEXITSTATUS(status);
			}

			if (WIFSIGNALED(status)) {
				last_status = 128 + WTERMSIG(status);
			}
		}
	}

	// Foreground pipeline children have been collected.
	// Allow SIGCHLD again.
	sigprocmask(SIG_SETMASK, &old_mask, NULL);

	return last_status;
}
