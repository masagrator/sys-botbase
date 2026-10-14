#pragma once
#include <stdbool.h>
// SD card file commands; true when argv[0] was one of them (reply already sent on fd)
bool fileCommand(int argc, char** argv, int fd);
