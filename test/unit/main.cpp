#include "synth/tables.h"
#include "test_framework.h"

int main() {
    // The baked tables must exist before any patch is built or any voice runs.
    // Idempotent, control thread only.
    msynth::tables_init();
    return testfw::run_all();
}
