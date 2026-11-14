/* newlib (3DS): <sys/types.h> define «quad» como macro y choca con el campo «quad» de MuPDF */
#include <sys/types.h>
#undef quad
/* newlib no trae timegm: Cloud3DS la da (en la consola no hay zona horaria: mktime ya es UTC) */
#include <time.h>
time_t timegm(struct tm *tm);
