/* TODO: copyright? */

#pragma once

#include "recovery_ui/ui.h"

// Read block devices available from recovery to ensure that the
// underlying storage works nominally
void scan_storage(RecoveryUI* ui);
