#ifndef MD_SOCKET_TYPES_H
#define MD_SOCKET_TYPES_H
#include <stdint.h>
/* Keep native Windows headers out of public interfaces. */
#ifdef _WIN32
typedef intptr_t MdSocket;
#else
typedef int MdSocket;
#endif
#endif
