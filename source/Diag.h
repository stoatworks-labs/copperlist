#pragma once

#include <string>

/**
    Logging for a plugin that lives inside somebody else's process.

    A small member of the fleet's `diag` family, carried from orrery. The rest of
    the repos get a rotating log, a crash report and a diagnostics bundle; an
    FFGL plugin gets only the log, for two reasons:

    - **No crash handler.** A plugin loaded into Resolume must not install a
      process-wide signal handler. It would intercept faults that are not ours
      and interfere with the host's own handling. A plugin has no business
      deciding what happens when Resolume dies.
    - **No bundle command.** There is no UI to hang one off -- a plugin is a
      list of sliders in someone else's inspector.

    ## Why this exists here, specifically

    Carried from gridiron, whose v0.1.0 shipped with no logging at all; its first
    outside bug report (gridiron#1, a black wall in Resolume on Windows) arrived with
    "Doesn't seem to create a log file" in the box that asks for one. That
    reporter was right, and the report could not be answered from the evidence
    because there was none to give.

    Every way this plugin fails looks identical from the operator's side -- a
    black clip, or one that never moves -- and the causes are not
    distinguishable without saying which one happened:

    - **The scaling shader would not compile.** The vendor, renderer and
      version strings go next to it, because a shader that builds on one
      machine and not another is a driver answer, not a source answer.
    - **The host clock unit.** Resolume sends milliseconds and an offline
      harness sends seconds. Here that decides the field rate: a clock read a
      thousand times fast runs the demo at 50,000 fields a second, which the
      catch-up treats as a jump every frame -- a scroller that teleports.
      What the clock settled on is stated outright rather than inferred.

    ## Rate

    `ProcessOpenGL` runs fifty times a second. Nothing here is called from a
    per-frame path except through `stateChanged`, which logs a *transition* --
    so a wall that is black for an hour writes one line, not 180,000.
*/
namespace copperlist::diag
{

/// Open the log file and record the plugin build, once per process.
void init();

void info( const std::string& message );
void warn( const std::string& message );
void error( const std::string& message );

/// Log `message` only when it differs from the last message logged under `key`.
/// For the per-frame paths, where the interesting event is the change and the
/// steady state is noise.
void stateChanged( const std::string& key, const std::string& message );

/// Full path of the log file, for the README to point at.
std::string logPath();

} // namespace copperlist::diag
