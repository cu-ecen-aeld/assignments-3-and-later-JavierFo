#include <stdio.h>
#include <stdlib.h>
#include <syslog.h>
#include <string.h>

int main(int argc, char *argv[])
{
    // Open syslog with LOG_USER facility
    openlog("writer", LOG_PID, LOG_USER);

    // Verify correct number of arguments
    if (argc != 3) {
        syslog(LOG_ERR, "Error: Two arguments required: <writefile> <writestr>");
        fprintf(stderr, "Error: Two arguments required: <writefile> <writestr>\n");
        closelog();
        exit(1);
    }

    const char *writefile = argv[1];
    const char *writestr = argv[2];

    // Log the debug message
    syslog(LOG_DEBUG, "Writing %s to %s", writestr, writefile);

    // Open file for writing
    FILE *fp = fopen(writefile, "w");
    if (fp == NULL) {
        syslog(LOG_ERR, "Error: Could not open file %s", writefile);
        perror("fopen");
        closelog();
        exit(1);
    }

    // Write the string to the file
    if (fprintf(fp, "%s", writestr) < 0) {
        syslog(LOG_ERR, "Error: Failed to write to file %s", writefile);
        perror("fprintf");
        fclose(fp);
        closelog();
        exit(1);
    }

    fclose(fp);
    closelog();
    return 0;
}

