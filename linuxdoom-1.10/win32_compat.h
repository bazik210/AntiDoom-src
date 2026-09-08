#ifndef DOOM_WIN32_COMPAT_H
#define DOOM_WIN32_COMPAT_H
int _mkdir(const char *path);
void I_InitLog(void);
void I_Log(const char *fmt, ...);
#endif
