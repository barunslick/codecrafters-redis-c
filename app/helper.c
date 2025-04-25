
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include "helper.h"

void exit_with_error(char * msg){
	fprintf(stderr, "%s: %s\n", msg, strerror(errno));
	exit(1);
}


void error(char * msg){
	fprintf(stderr, "%s: %s\n", msg, strerror(errno));
}