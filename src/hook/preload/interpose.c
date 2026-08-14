#include <stdarg.h>
#include <sys/prctl.h>
#include <sys/syscall.h>

#include "core/log.h"

/* Intercept prctl(PR_SET_NAME) to refresh the cached process ident
 * when a game renames itself (used by Wine/Proton, Steam child procs). */
int prctl(int option, ...) {
  va_list ap;
  va_start(ap, option);
  unsigned long a2 = va_arg(ap, unsigned long);
  unsigned long a3 = va_arg(ap, unsigned long);
  unsigned long a4 = va_arg(ap, unsigned long);
  unsigned long a5 = va_arg(ap, unsigned long);
  va_end(ap);
  int ret = syscall(SYS_prctl, option, a2, a3, a4, a5);
  if (option == PR_SET_NAME && ret == 0)
    idk_process_ident_invalidate();
  return ret;
}
