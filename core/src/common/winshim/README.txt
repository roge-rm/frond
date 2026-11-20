Windows (MSYS2 UCRT64) stand-ins for the POSIX headers the core includes. Makefile.common puts this
folder on CPATH on Windows only. Each one includes the real header when there is one.
