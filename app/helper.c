
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include "helper.h"

#ifdef _WIN32
    #include <windows.h>
#elif __linux__
    #include <sys/utsname.h>
#elif __APPLE__
    #include <sys/sysctl.h>
#endif

// Function to get OS information
void get_os_info(char *buffer, size_t buffer_size) {
    #ifdef _WIN32
		snprintf(buffer, buffer_size, "Windows");
    #elif __linux__
        struct utsname unamedata;
        if (uname(&unamedata) == 0) {
            snprintf(buffer, buffer_size, "%s %s %s", 
                    unamedata.sysname, unamedata.release, unamedata.machine);
        } else {
			snprintf(buffer, buffer_size, "Linux (unknown version)");
        }
    #elif __APPLE__
    #else
		snprintf(buffer, buffer_size, "Unknown OS");
    #endif
    
    // Ensure null termination
    buffer[buffer_size - 1] = '\0';
}

void exit_with_error(char * msg){
	fprintf(stderr, "%s: %s\n", msg, strerror(errno));
	exit(1);
}


void error(char * msg){
	fprintf(stderr, "%s: %s\n", msg, strerror(errno));
}