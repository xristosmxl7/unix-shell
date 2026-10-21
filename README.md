# Unix Shell

## Overview

Unix Shell is a Unix-like command-line shell implemented in C. It supports command execution, I/O 
redirection, pipelines, and foreground/background execution.

## Features

- Simple command execution such as `ls`, `pwd`, and `echo`
- Input and output redirection using `<`, `>`, and `>>`
- Pipeline execution using `|`
- Foreground and background execution using `&`
- Built-in commands: `cd`, `status`, `pinfo`, and `exit`
- Signal handling and background process management

## Implementation

- Creates child processes using `fork()` and executes external commands with `execvp()`
- Manages and waits for child processes using `waitpid()`
- Implements pipelines using `pipe()` and `dup2()` for file descriptor redirection
- Handles signals using `sigaction()`, including `SIGCHLD` for background process management

## Build

A Linux environment with GCC and Make is required. To build the shell, run:

```bash
make
```

## Usage

After building the project, start the shell with:

```bash
./osh
```

The shell displays the `osh>` prompt and waits for commands.

### Examples

Execute a command:

```bash
ls -l
```

Redirect output:

```bash
echo hello > output.txt
```

Append output:

```bash
echo world >> output.txt
```

Create a pipeline:

```bash
ls | wc -l
```

Run a command in the background:

```bash
sleep 5 &
```

Use a built-in command:

```bash
pinfo
```

## Notes

- Operators such as `|`, `<`, `>`, `>>`, and `&` must be separated from other arguments by spaces.
- Background execution is supported for simple commands, but not for pipelines.
- The shell does not implement full job control.
- The `pinfo` command relies on the Linux `/proc` filesystem.
