#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

#include "topsig-file.h"

int is_directory(const char *filename) {
	struct stat st;
	if( stat(filename, &st) == 0)
		return S_ISDIR(st.st_mode);
	return 0;
}



