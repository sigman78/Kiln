// kiln/cook/cli.h — the kiln-cook command line as a library call, so a project's own cook tool
// can add its CookPolicy (docs/design/settings.md, "Resolution layers").
#pragma once

#include "kiln/cook/settings.h"

namespace kiln::cook {

/// Runs kiln-cook with `argv` (options: `kiln-cook --help`) and `policy` as resolution layer 6.
/// Returns the exit code: 0 every input cooked, 1 usage, 2 IO failure, 3 one or more cook errors.
/// A project tool is `int main(int argc, char** argv) { return cook_cli_main(argc, argv, policy); }`.
/// `policyVersion` is ProviderDesc::policyVersion: bump it when the policy changes.
KILN_API int cook_cli_main(int argc, char** argv, CookPolicy const& policy = {}, u32 policyVersion = 0);

} // namespace kiln::cook
