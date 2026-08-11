#pragma once

#include "wdlstring.h"

// Platform-specific file dialog helpers.
// Returns true if the user selected a file/directory, false on cancel.

bool NAM_ShowOpenFileDialog(const char* title, const char* extension, WDL_String& outPath);
bool NAM_ShowOpenDirectoryDialog(const char* title, WDL_String& outPath);
