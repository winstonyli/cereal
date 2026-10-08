void f(void *p, int n) {
  __builtin___memcpy_chk(0, p, n, n); __builtin___memcpy_chk(p, 0, n, n);
  __builtin___memmove_chk(0, p, n, n); __builtin___memmove_chk(p, 0, n, n);
  __builtin___mempcpy_chk(0, p, n, n); __builtin___mempcpy_chk(p, 0, n, n);
  __builtin___memset_chk(0, 1, n, n);
  __builtin___strcpy_chk(0, p, n); __builtin___strcpy_chk(p, 0, n);
  __builtin___stpcpy_chk(0, p, n); __builtin___stpcpy_chk(p, 0, n);
  __builtin___strncpy_chk(0, p, n, n); __builtin___strncpy_chk(p, 0, n, n);
  __builtin___stpncpy_chk(0, p, n, n); __builtin___stpncpy_chk(p, 0, n, n);
  __builtin___strcat_chk(0, p, n); __builtin___strcat_chk(p, 0, n);
  __builtin___strncat_chk(0, p, n, n); __builtin___strncat_chk(p, 0, n, n);
  __builtin___sprintf_chk(0, 1, n, p); __builtin___sprintf_chk(p, 1, n, 0);
  __builtin___snprintf_chk(0, n, 1, n, p); __builtin___snprintf_chk(p, n, 1, n, 0);
  __builtin___printf_chk(1, 0); __builtin___fprintf_chk(0, 1, p); __builtin___fprintf_chk(p, 1, 0);
}
void f2(void *p, int n, __builtin_va_list v) {
  __builtin___vsprintf_chk(0, 1, n, p, v); __builtin___vsprintf_chk(p, 1, n, 0, v);
  __builtin___vsnprintf_chk(0, n, 1, n, p, v); __builtin___vsnprintf_chk(p, n, 1, n, 0, v);
  __builtin___vprintf_chk(1, 0, v); __builtin___vfprintf_chk(0, 1, p, v); __builtin___vfprintf_chk(p, 1, 0, v);
  __builtin_vprintf(0, v); __builtin_vfprintf(0, p, v); __builtin_vfprintf(p, 0, v); __builtin_vsprintf(0,p,v); __builtin_vsprintf(p,0,v);
  __builtin_vsnprintf(p, n, 0, v); __builtin_vscanf(0, v); __builtin_vsscanf(0, p, v); __builtin_vsscanf(p, 0, v); __builtin_vfscanf(0, p, v); __builtin_vfscanf(p, 0, v);
}
