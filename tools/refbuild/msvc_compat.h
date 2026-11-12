/* Force-included compatibility header: emulate MSVC 2008 environment on clang/mingw-w64 (i686). */
#pragma once
#ifdef __cplusplus
#include <algorithm>
#include <string>
#include <vector>
#include <map>
#include <set>
#include <list>
#include <fstream>
#include <iostream>
#include <sstream>
#include <memory>
#include <limits>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#endif
#include <windows.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
/* MSVC's windows.h defines min/max as macros in C++ too */
#ifndef min
#define min(a,b) (((a) < (b)) ? (a) : (b))
#endif
#ifndef max
#define max(a,b) (((a) > (b)) ? (a) : (b))
#endif
#include "svnrev.h"
#define vscprintf _vscprintf
