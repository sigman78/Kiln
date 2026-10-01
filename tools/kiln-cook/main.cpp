// tools/kiln-cook/main.cpp — kiln-cook: the cook CLI with no CookPolicy (kiln/cook/cli.h).
#include "kiln/cook/cli.h"
#include "no_crash_dialogs.h"

int main(int argc, char** argv) {
    kiln::no_crash_dialogs();
    return kiln::cook::cook_cli_main(argc, argv);
}
