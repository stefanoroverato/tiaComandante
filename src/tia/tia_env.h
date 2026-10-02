#ifndef TC_TIA_ENV_H
#define TC_TIA_ENV_H

#include <stddef.h>
#include <wchar.h>

/* Locates the Openness V21 net48 assembly directory: environment variable
   TIACMD_OPENNESS_DIR, then the registry key
   HKLM\SOFTWARE\Siemens\Automation\Openness\21.0\PublicAPI\<ver>\net48,
   then %ProgramFiles%\Siemens\Automation\Portal V21\PublicAPI\V21\net48.
   Returns 0 on success. */
int tia_find_openness_dir(wchar_t *out, size_t cap);

/* TIA Portal version string from the registry ("V21"), or empty. */
void tia_portal_version(char *out, size_t cap);

#endif
