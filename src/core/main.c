#include "backend_shm.h"

#if defined(BACKEND_FBDEV)
#include "backend_fbdev.h"
#endif

int main(int argc, char **argv)
{
	(void)argc;
	(void)argv;
#if defined(BACKEND_FBDEV)
	return run_fbdev_backend();
#else
	return run_shm_backend();
#endif
}
