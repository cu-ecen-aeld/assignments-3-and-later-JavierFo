#include "systemcalls.h"
#include <stdarg.h>
#include <stdbool.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <fcntl.h>

/**
 * @param cmd the command to execute with system()
 * @return true if the command in @param cmd was executed
 *   successfully using the system() call, false if an error occurred,
 *   either in invocation of the system() call, or if a non-zero return
 *   value was returned by the command issued in @param cmd.
*/
bool do_system(const char *cmd)
{

// If cmd is NULL, system() behavior is implementation-defined.
    // The assignment expects us to treat this as failure.
    if (cmd == NULL)
        return false;

    int ret = system(cmd);

    // system() returns -1 if it failed to execute
    if (ret == -1)
        return false;

    // WEXITSTATUS only valid when WIFEXITED(ret) is true
    if (WIFEXITED(ret) && WEXITSTATUS(ret) == 0)
        return true;


    return true;
}

/**
* @param count -The numbers of variables passed to the function. The variables are command to execute.
*   followed by arguments to pass to the command
*   Since exec() does not perform path expansion, the command to execute needs
*   to be an absolute path.
* @param ... - A list of 1 or more arguments after the @param count argument.
*   The first is always the full path to the command to execute with execv()
*   The remaining arguments are a list of arguments to pass to the command in execv()
* @return true if the command @param ... with arguments @param arguments were executed successfully
*   using the execv() call, false if an error occurred, either in invocation of the
*   fork, waitpid, or execv() command, or if a non-zero return value was returned
*   by the command issued in @param arguments with the specified arguments.
*/

bool do_exec(int count, ...)
{
    va_list args;
    va_start(args, count);

    // Build argv array (count arguments + NULL terminator)
    char *command[count + 1];
    for(int i = 0; i < count; i++)
    {
        command[i] = va_arg(args, char *);
    }
    command[count] = NULL;   // execv() requires NULL-terminated argv

    va_end(args);

    // Fork the process
    pid_t pid = fork();
    if (pid < 0)
    {
        // Fork failed
        return false;
    }
    else if (pid == 0)
    {
        // ----- CHILD -----
        // Execute the command
        execv(command[0], command);

        // If execv returns, an error occurred
        exit(EXIT_FAILURE);
    }
    else
    {
        // ----- PARENT -----
        int status = 0;
        pid_t w = waitpid(pid, &status, 0);

        if (w == -1)
        {
            // waitpid failed
            return false;
        }

        // Check if child exited normally and with exit code 0
        if (WIFEXITED(status) && WEXITSTATUS(status) == 0)
        {
            return true;
        }
        else
        {
            return false;
        }
    }
}

/**
* @param outputfile - The full path to the file to write with command output.
*   This file will be closed at completion of the function call.
* All other parameters, see do_exec above
*/
bool do_exec_redirect(const char *outputfile, int count, ...)
{
     va_list args;
    va_start(args, count);

    // Build argument vector for execv()
    char *command[count + 1];
    for (int i = 0; i < count; i++)
    {
        command[i] = va_arg(args, char *);
    }
    command[count] = NULL;

    va_end(args);

    // Fork the child
    pid_t pid = fork();
    if (pid < 0)
    {
        // fork failed
        return false;
    }

    if (pid == 0)
    {
        // ------- CHILD PROCESS -------

        // Open output file for writing (create if needed)
        int fd = open(outputfile, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd < 0)
        {
            // Cannot open file, exit with failure
            exit(EXIT_FAILURE);
        }

        // Redirect stdout to the file
        if (dup2(fd, STDOUT_FILENO) < 0)
        {
            // redirection failed
            close(fd);
            exit(EXIT_FAILURE);
        }

        close(fd); // fd no longer needed after dup2

        // Execute the command
        execv(command[0], command);

        // If execv returns, it failed
        exit(EXIT_FAILURE);
    }

    // ------- PARENT PROCESS -------
    int status = 0;
    pid_t wpid = waitpid(pid, &status, 0);

    if (wpid < 0)
    {
        // waitpid failed
        return false;
    }

    // Check exit status
    if (WIFEXITED(status) && WEXITSTATUS(status) == 0)
    {
        return true;
    }

    return false;
}
