#ifndef HOST_CONFIG_H
#define HOST_CONFIG_H

#if defined __x86_64 || defined __x86_64__
#define HOST_ARCH ISx86_64
#else
#define HOST_ARCH ISxxx
#endif

#if defined __linux__
#define HOST_OS OSlinux
#else
#define HOST_OS OSunknown
#endif

#if defined __linux__
#define HOST_ABI ABIgnu
#else
#define HOST_ABI ABInone
#endif

#ifndef HOST_INCLUDE_DIRS
#define HOST_INCLUDE_DIRS "/usr/include", "/usr/local/include"
#endif

#endif

